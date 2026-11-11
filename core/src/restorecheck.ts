/**
 * Before a content-mode restore: will the OP-Z take this backup of its disk? Reads only file names and sizes (the
 * backup folder on the computer, optionally the OP-Z disk as it is now) and the settings our tools last read, and
 * predicts what the importer will do with each slot folder. Port of tools/opz_restorecheck.py (same numbers, same
 * lines). Why: docs/roadmap/restore-space.md. Every rule below is the importer of OS 1.2.45 (`0x80038174`,
 * `0x80038cd8`, `0x80037df4`) as described there [code]; what it does with folders merged by Finder is partly
 * [inferred].
 */

import { type Doc, isUserSample } from "./plan.ts";
import { PLUG_LIMIT, SAMPLE_LIMIT, isOurPlug } from "./space.ts";

/** The content-mode disk: a FAT of 0x2100000 bytes in RAM (`0x8002ede0`, 33 MB) [code]. */
export const DISK_BYTES = 0x2100000;

/** A file on a disk or in a backup folder: path relative to the folder picked, size in bytes. */
export interface DiskEntry {
  path: string;
  size: number;
}

interface SlotFile {
  name: string;
  size: number;
}

interface SlotFolder {
  /** "samplepacks/1-kick/05" as on the disk. */
  dir: string;
  files: SlotFile[];
}

export interface RestoreCheck {
  /** Slot folders in the backup, and how many of them match what the slot holds now (skipped by the OP-Z). */
  folders: number;
  unchanged: number;
  /** Full-size samples no installed plug names: each becomes a new user sample (one plug definition, its size). */
  imports: DiskEntry[];
  /** Full-size samples an installed plug already names: only assigned, but checked against 24 MB at full size. */
  assigns: DiskEntry[];
  /** 0-byte `~name` placeholders of packs the OP-Z has (assigned, free). */
  placeholders: number;
  /** `~<id>.engine` placeholders whose plug isn't installed: those slots would point at nothing. */
  dangling: Array<{ path: string; id: number }>;
  /** Slots the backup restores that hold one of our plugs now. */
  ourSlots: Array<[number, number]>;
  /** Sample memory: what stays on the OP-Z (bytes known only with the disk), the backup's imports, the worst check. */
  keptBytes: number | null;
  keptSamples: number;
  importBytes: number;
  peakBytes: number;
  /** Plug definitions free once the OP-Z has dropped the samples this restore replaces (null without settings). */
  freeDefinitions: number | null;
  /** Of ours on the OP-Z (the definitions `remove all of ours` frees). */
  ours: number;
  projects: number;
  backupBytes: number;
  diskBytes: number | null;
  problems: string[];
  warnings: string[];
  /** Taking our plugs off first would fix a problem or a warning here. */
  removeOursHelps: boolean;
}

const SLOT_RE = /(?:^|\/)(samplepacks\/(\d+)-[^/]+\/(\d+))\/([^/]+)$/;
const SAMPLE_RE = /\.(wav|aiff?)$/i;
/** The export cuts placeholder names to 11 characters, so ours read `~61020.engi` [proven: import.log]. */
const ENGINE_RE = /\.engi(ne?)?$/i;

/** Megabytes to one decimal, rounded half up in integers (so opz_restorecheck.py prints the same). */
function mb(n: number): string {
  const tenths = Math.floor((n * 10 + 524288) / 1048576);
  return `${Math.floor(tenths / 10)}.${tenths % 10} MB`;
}

/** "track:slot" -> that slot folder's files (dot files skipped, as the OP-Z does), sorted by name. */
function slotFolders(entries: readonly DiskEntry[]): Map<string, SlotFolder> {
  const out = new Map<string, SlotFolder>();
  for (const e of entries) {
    const m = SLOT_RE.exec(e.path.replaceAll("\\", "/"));
    if (!m || m[4].startsWith(".")) continue;
    if (!SAMPLE_RE.test(m[4]) && !ENGINE_RE.test(m[4])) continue;
    const n = Number(m[2]);
    const s = Number(m[3]);
    if (n < 1 || n > 8 || s < 1 || s > 10) continue;
    const key = `${n - 1}:${s}`;
    if (!out.has(key)) out.set(key, { dir: m[1], files: [] });
    out.get(key)!.files.push({ name: m[4], size: e.size });
  }
  for (const f of out.values()) f.files.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  return out;
}

const bare = (name: string) => name.replace(/^~/, "");
const basename = (path: string) => String(path.split("/").at(-1));
const placeholderId = (name: string) => Number.parseInt(bare(name).split(".")[0], 10) || 0;

/**
 * What a content-mode restore of `backup` would do. `device`: the settings our tools last read from this OP-Z (null:
 * no prediction about matches, definitions or dangling slots). `disk`: the OP-Z disk as it is now (null: the size of
 * the samples that stay is unknown).
 */
export function checkContentRestore(
  backup: readonly DiskEntry[], device: { plugs: Doc; slots: Doc } | null, disk: readonly DiskEntry[] | null = null,
): RestoreCheck {
  const plugs: Doc[] = device?.plugs.plugs ?? [];
  const byId = new Map<number, Doc>(plugs.map((p) => [p.id, p]));
  const named = new Set(plugs.map((p) => String(p.samples ?? "")).filter((s) => /^samplepacks\/(user\/)?[^/]+$/.test(s)).map(basename));
  const cell = new Map<string, number>();
  for (const t of device?.slots.tracks ?? []) for (const p of t.packs ?? []) cell.set(`${t.index}:${p.slot}`, p.id);

  const folders = slotFolders(backup);
  const problems: string[] = [];
  const warnings: string[] = [];
  const imports: DiskEntry[] = [];
  const assigns: DiskEntry[] = [];
  const dangling: Array<{ path: string; id: number }> = [];
  const ourSlots: Array<[number, number]> = [];
  const restored = new Set<string>(); // folders the restore changes
  const names = new Set<string>(); // every sample name the backup holds
  let unchanged = 0;
  let placeholders = 0;
  const order = (k: string) => k.split(":").map(Number).reduce((a, b) => a * 100 + b);
  for (const [key, { dir, files }] of [...folders.entries()].sort((a, b) => order(a[0]) - order(b[0]))) {
    const [t, s] = key.split(":").map(Number);
    for (const f of files) if (SAMPLE_RE.test(f.name)) names.add(bare(f.name));
    const now = cell.get(key);
    const plug = now === undefined ? undefined : byId.get(now);
    // the slot is unchanged when a file names its pack, or (TE's engines, ids up to 200) its ~<id>.engine [code 0x80036108, 0x80036298]
    if (plug && ((plug.samples && files.some((f) => bare(f.name) === basename(String(plug.samples))))
      || (!plug.samples && now! <= 200 && files.some((f) => ENGINE_RE.test(f.name) && placeholderId(f.name) === now)))) {
      unchanged++;
      continue;
    }
    restored.add(key);
    if (plug && isOurPlug(plug)) ourSlots.push([t, s]);
    let sampleSeen = false;
    for (const f of files) {
      const path = `${dir}/${f.name}`;
      if (ENGINE_RE.test(f.name)) {
        const id = placeholderId(f.name);
        if (!id) warnings.push(`${path}: not a plug id; the OP-Z rejects it`);
        else if (device && !byId.has(id)) dangling.push({ path, id });
        continue;
      }
      if (sampleSeen) {
        warnings.push(`${path}: a second sample in one slot folder; the OP-Z uses only the first that imports`);
        continue;
      }
      sampleSeen = true;
      if (f.size === 0) {
        if (device && !named.has(bare(f.name))) warnings.push(`${path}: a placeholder for a pack this OP-Z doesn't have; that slot ends up empty`);
        else placeholders++;
      } else if (device && named.has(bare(f.name))) assigns.push({ path, size: f.size });
      else imports.push({ path, size: f.size });
    }
  }

  // what stays in sample memory: user samples in slots the restore leaves, or named by the backup again
  const userCells = new Map<number, string[]>();
  for (const [k, id] of cell) if (byId.has(id) && isUserSample(byId.get(id)!)) userCells.set(id, [...(userCells.get(id) ?? []), k]);
  const kept = [...userCells.entries()].filter(([id, ks]) => ks.some((k) => !restored.has(k)) || names.has(basename(String(byId.get(id)!.samples))));
  let keptBytes: number | null = null;
  let diskBytes: number | null = null;
  if (disk) {
    diskBytes = disk.reduce((a, e) => a + e.size, 0);
    keptBytes = 0;
    const seen = new Set<string>();
    for (const [key, { files }] of slotFolders(disk)) {
      for (const f of files) {
        if (f.size === 0 || !SAMPLE_RE.test(f.name) || seen.has(f.name)) continue;
        if (!restored.has(key) || names.has(f.name)) {
          seen.add(f.name);
          keptBytes += f.size;
        }
      }
    }
  }
  const importBytes = imports.reduce((a, e) => a + e.size, 0);
  const peakBytes = (keptBytes ?? 0) + importBytes + Math.max(0, ...assigns.map((e) => e.size));
  if (peakBytes > SAMPLE_LIMIT) {
    problems.push(`sample memory: the OP-Z keeps 24 MB of your samples; this restore needs about ${mb(peakBytes)}`
      + (keptBytes === null ? "" : ` (${mb(keptBytes)} that stay on it, ${mb(importBytes)} brought back)`)
      + ". It refuses the rest (\"sample memory full\", logged only at the next content mode) and those slots stay as they were"
      + ". Restore fewer slots at a time, or first delete samples you don't need on the disk and eject");
  } else if (keptBytes === null && kept.length && importBytes > SAMPLE_LIMIT / 2) {
    warnings.push(`${kept.length} of your samples stay on the OP-Z and count towards its 24 MB too (their size is known only with the disk: check it in content mode)`);
  }

  // plug definitions: once the OP-Z drops the samples this restore replaces, each import needs a free one
  let freeDefinitions: number | null = null;
  const ours = plugs.filter(isOurPlug).length;
  let removeOursHelps = ourSlots.length > 0;
  if (device) {
    const dropped = [...userCells.keys()].filter((id) => !kept.some(([k]) => k === id)).length;
    freeDefinitions = PLUG_LIMIT - plugs.length + dropped;
    if (imports.length > freeDefinitions) {
      problems.push(`sound definitions: this restore brings back ${imports.length} samples, and only ${freeDefinitions} of the OP-Z's `
        + `${PLUG_LIMIT} definitions will be free. The OP-Z logs "SUCCESS" for the others, then deletes them`
        + (ours ? `. Taking our ${ours} plugs off first frees ${ours}` : ""));
      if (ours && imports.length <= freeDefinitions + ours) removeOursHelps = true;
    }
  }
  if (dangling.length) {
    problems.push(`${dangling.length} slot${dangling.length === 1 ? "" : "s"} in the backup name a plug that isn't installed now `
      + `(${[...new Set(dangling.map((d) => d.id))].join(", ")}): the OP-Z would put that id in the slot anyway and the slot would be silent. `
      + "Delete those ~<number>.engine files from the copy you restore, or install those engines again first");
  }
  if (ourSlots.length) {
    warnings.push(`${ourSlots.length} slot${ourSlots.length === 1 ? "" : "s"} the backup restores hold our sounds now. If the copy merges `
      + "folders, our sound can stay in a slot whose sample fails to import. Take our plugs off first, or replace whole slot folders");
  }
  const backupBytes = backup.reduce((a, e) => a + e.size, 0);
  if (backupBytes > DISK_BYTES) {
    problems.push(`the backup is ${mb(backupBytes)}; the OP-Z disk holds ${mb(DISK_BYTES)} in all. Copy it in parts: samplepacks, then projects`);
  } else if (diskBytes !== null && diskBytes + backupBytes > DISK_BYTES) {
    warnings.push(`the disk holds ${mb(diskBytes)} now and the backup is ${mb(backupBytes)}; together more than the disk's ${mb(DISK_BYTES)}. `
      + "If the computer says there isn't enough space, delete the old files on the OP-Z disk first (empty the Trash, or delete "
      + "them for good), then copy");
  }
  const projects = backup.filter((e) => /(?:^|\/)projects\/[^/]+\.opz$/i.test(e.path.replaceAll("\\", "/"))).length;
  return {
    folders: folders.size, unchanged, imports, assigns, placeholders, dangling, ourSlots, keptBytes, keptSamples: kept.length,
    importBytes, peakBytes, freeDefinitions, ours, projects, backupBytes, diskBytes, problems, warnings, removeOursHelps,
  };
}

/** The lines `opz.py restore-check` prints. */
export function describeRestoreCheck(r: RestoreCheck): string[] {
  const lines = [
    `backup: ${mb(r.backupBytes)}, ${r.folders} slot folders (${r.unchanged} as on the OP-Z now), ${r.projects} projects`,
    `samples: ${r.imports.length} brought back (${mb(r.importBytes)}), ${r.assigns.length} already on the OP-Z, ${r.placeholders} placeholders`,
    `sample memory: about ${mb(r.peakBytes)} of 24.0 MB at the worst point`
      + (r.keptBytes === null && r.keptSamples ? ` (+ ${r.keptSamples} of your samples that stay, size unknown without the disk)` : ""),
  ];
  if (r.freeDefinitions !== null) lines.push(`sound definitions: ${r.freeDefinitions} free for ${r.imports.length} new samples (${r.ours} of ours on the OP-Z)`);
  if (r.diskBytes !== null) lines.push(`OP-Z disk now: ${mb(r.diskBytes)} of ${mb(DISK_BYTES)}`);
  for (const p of r.problems) lines.push("problem: " + p);
  for (const w of r.warnings) lines.push("warning: " + w);
  if (r.removeOursHelps) lines.push("advice:  take our plugs off first (opz.py remove --all-ours, or Remove all of ours in the web app), then restore");
  if (!r.problems.length) lines.push(r.warnings.length ? "ok with warnings: the restore should go through" : "ok: the restore should go through");
  return lines;
}

/** What a content-mode `import.log` says went wrong (it is written at the next content mode, from the last ejects). */
export interface ImportLogSummary {
  /** The last "Calculating used sample space...x/24.0 MB". */
  used: string | null;
  /** Files refused for sample memory. */
  memoryFull: string[];
  /** Other refusals: bad format, rate, engine names. */
  failed: string[];
  imports: number;
}

export function parseImportLog(text: string): ImportLogSummary {
  const out: ImportLogSummary = { used: null, memoryFull: [], failed: [], imports: 0 };
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    const used = /Calculating used sample space\.\.\.([\d.]+\/[\d.]+ MB)/.exec(line);
    if (used) out.used = used[1];
    const full = /Can't import (.+), sample memory full/.exec(line);
    if (full) out.memoryFull.push(full[1]);
    if (/^importing .+\.\.\.SUCCESS/.test(line)) out.imports++;
    else if (/^importing .+\.\.\./.test(line) || /^rejecting /.test(line)) out.failed.push(line);
  }
  return out;
}
