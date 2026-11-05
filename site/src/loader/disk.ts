/**
 * Ported from web/app/src/disk.ts; only the undo advice names the loader's own Restore step.
 *
 * Bake our presets into the OP-Z's own user-preset store, config/userPresets.dat, on its content-mode disk (the
 * OP-Z mounted as a USB disk: hold track while switching it on). What `opz.py presets bake --disk /Volumes/OP-Z` does,
 * through Chrome's File System Access API: the user picks the disk, and this reads the one file, checks it strictly,
 * plans (the core's userpresets.ts), backs the original up, asks, writes it in place and reads it back. The OP-Z
 * imports the file when the disk is ejected; from then on the presets are user presets and survive every restart
 * with nothing connected (docs/research/presets.md, "Presets that stay"; hardware check H-9).
 *
 * Nothing else on the disk is written: only config/userPresets.dat, and only after the confirm. (Chrome writes
 * through a swap file next to it, `userPresets.dat.crswap`, and moves it over the file when the write closes.)
 *
 * An OP-Z on which no preset was ever stored by hand has no userPresets.dat, so its disk has none either. The bake then
 * makes the file: an empty store (version 1, 200 free records) with our presets in it. Content mode imports the file at
 * the eject even onto an OP-Z that has none [code, device: the config import callback `0x80034d68`,
 * docs/research/presets.md]; nothing is backed up because there was nothing; a file that reads back differently is
 * deleted again. Hardware check H-9b (src/loader/README.md).
 */

import {
  type BakeEngine, type BakePlan, USER_PRESETS, USER_PRESETS_RECORD, USER_PRESETS_RECORDS, bytesEqual as same, planBake, planRestore,
  readUserPresetsFile, serialiseUserPresets, toHex,
} from "./core.ts";

/** The part of FileSystemFileHandle the bake uses (a fake in the tests). */
export interface DiskFile {
  readonly kind: "file";
  readonly name: string;
  getFile(): Promise<{ arrayBuffer(): Promise<ArrayBuffer> }>;
  createWritable(options?: { keepExistingData?: boolean }): Promise<{ write(data: Uint8Array): Promise<void>; close(): Promise<void>; abort?(): Promise<void> }>;
}

/** The part of FileSystemDirectoryHandle the bake uses. */
export interface DiskDir {
  readonly kind: "directory";
  readonly name: string;
  getDirectoryHandle(name: string): Promise<DiskDir>;
  getFileHandle(name: string, options?: { create?: boolean }): Promise<DiskFile>;
  removeEntry(name: string): Promise<void>;
}

/** An empty user-preset store as the OP-Z writes it (version 1, 200 free records). */
export function emptyUserPresets(): Uint8Array {
  return serialiseUserPresets(Array.from({ length: USER_PRESETS_RECORDS }, () => new Uint8Array(USER_PRESETS_RECORD)));
}

type Picker = (options: { id?: string; mode: "read" | "readwrite"; startIn?: string }) => Promise<DiskDir>;

/** Chrome and Edge on a computer have showDirectoryPicker; Safari, Firefox and phones don't. */
export function diskPicker(w: unknown = globalThis): Picker | null {
  const pick = (w as { showDirectoryPicker?: Picker }).showDirectoryPicker;
  return typeof pick === "function" ? pick.bind(w) : null;
}

/** The user's request: our presets (bake), or a backed-up file put back (restore), or a look only (show). */
export type BakeRequest =
  | { kind: "bake"; engines: BakeEngine[]; overwriteUser: boolean }
  | { kind: "restore"; data: Uint8Array; source: string }
  | { kind: "show" };

/** What the confirm sheet shows. */
export interface BakeConfirm {
  request: BakeRequest;
  /** The disk's name as picked (the volume: "OP-Z"). */
  disk: string;
  /** The plan (bake only). */
  plan: BakePlan | null;
  /** The backup just made of the file on the disk; null when the file is new (there was none to back up). */
  backupId: string | null;
  /** The disk had no userPresets.dat: the bake makes one. */
  created: boolean;
  /** The core's lines so far (read, summary, plan). */
  lines: string[];
}

export interface BakeDeps {
  log: (line: string) => void;
  /** Keep a copy of the original (in this browser; also a download where the browser can't keep it); returns its id. Throws: nothing is written. */
  backup: (data: Uint8Array, source: string, sha256: string) => Promise<string>;
  confirm: (c: BakeConfirm) => Promise<boolean>;
}

export interface BakeResult {
  ok: boolean;
  cancelled: boolean;
  /** Written and read back. */
  written: boolean;
  /** The file was not on the disk and was made. */
  created: boolean;
  lines: string[];
  error: string | null;
  backupId: string | null;
  plan: BakePlan | null;
  /** The file's summary (show, and every read). */
  summary: string[];
  warnings: string[];
}

const APPLE_DOUBLE = "._userPresets.dat";

function named(e: unknown): string {
  return (e as { name?: string })?.name ?? "";
}

async function bytesOf(f: DiskFile): Promise<Uint8Array> {
  return new Uint8Array(await (await f.getFile()).arrayBuffer());
}

async function exists(dir: DiskDir, name: string): Promise<boolean> {
  try {
    await dir.getFileHandle(name);
    return true;
  } catch {
    return false;
  }
}

/** Write in place (a truncating writable on the same file) and read back. */
async function writeChecked(file: DiskFile, data: Uint8Array): Promise<boolean> {
  const w = await file.createWritable({ keepExistingData: false });
  try {
    await w.write(data);
  } catch (e) {
    await w.abort?.().catch(() => {});
    throw e;
  }
  await w.close();
  return same(await bytesOf(file), data);
}

/**
 * Run a bake, a restore or a look on the picked disk. Every step logs the CLI's lines; nothing is written unless the
 * file passed the checks, the backup was made and the user said yes.
 */
export async function runBake(disk: DiskDir, request: BakeRequest, deps: BakeDeps): Promise<BakeResult> {
  const lines: string[] = [];
  const warnings: string[] = [];
  const result = (ok: boolean, extra: Partial<BakeResult> = {}): BakeResult => ({
    ok, cancelled: false, written: false, created: false, lines, error: null, backupId: null, plan: null, summary: [], warnings, ...extra,
  });
  const log = (line: string) => {
    lines.push(line);
    deps.log(line);
  };
  const fail = (error: string, extra: Partial<BakeResult> = {}) => {
    log(`error:   ${error}`);
    return result(false, { error, ...extra });
  };
  const path = `${disk.name}/${USER_PRESETS}`;

  let config: DiskDir;
  try {
    config = await disk.getDirectoryHandle("config");
  } catch (e) {
    if (named(e) !== "NotFoundError" && named(e) !== "TypeMismatchError") throw e;
    return fail(`${disk.name} has no config folder: pick the OP-Z's content-mode disk itself (the top folder, `
      + "usually OP-Z). Content mode: switch the OP-Z off, hold track while switching it on. Nothing written.");
  }
  let file: DiskFile | null = null;
  try {
    file = await config.getFileHandle("userPresets.dat");
  } catch (e) {
    if (named(e) === "TypeMismatchError") return fail(`${path} is a folder, not the OP-Z's preset file. Nothing written.`);
    if (named(e) !== "NotFoundError") throw e;
  }
  if (!file) {
    if (request.kind !== "bake") {
      return fail(`${path} is not on the disk: no preset was ever stored on this OP-Z, so there is ${request.kind === "show" ? "nothing to show" : "nothing to put back over"}.`);
    }
    return createStore(disk.name, config, request, deps, { lines, log, fail, result });
  }
  const data = await bytesOf(file);
  const read = await readUserPresetsFile(data, path);
  for (const l of read.lines) log(l);
  if (!read.records) return result(false, { error: read.lines[0].replace(/^error: +/, "") });
  const summary = read.lines.slice(1);
  if (request.kind === "show") return result(true, { summary });

  let plan: BakePlan | null = null;
  let next: Uint8Array;
  if (request.kind === "restore") {
    const r = await planRestore(request.data, request.source);
    for (const l of r.lines) log(l);
    if (!r.ok) return result(false, { error: r.lines[0].replace(/^error: +/, ""), summary });
    if (same(request.data, data)) {
      log("nothing to change: the file on the disk is already this backup.");
      return result(true, { summary });
    }
    next = request.data;
  } else {
    plan = planBake(data, request.engines, request.overwriteUser);
    for (const l of plan.lines) log(l);
    if (!plan.ok) return result(false, { error: plan.lines.at(-1)!.replace(/^error: +/, ""), plan, summary });
    if (!plan.data) return result(true, { plan, summary });
    next = plan.data;
  }

  let backupId: string;
  try {
    backupId = await deps.backup(data, path, read.sha256);
  } catch (e) {
    return fail(`the backup failed (${(e as Error).message}). Nothing written.`, { plan, summary });
  }
  log(`backup: ${backupId}`);
  if (!(await deps.confirm({ request, disk: disk.name, plan, backupId, created: false, lines: [...lines] }))) {
    log("cancelled.");
    return result(false, { cancelled: true, backupId, plan, summary });
  }
  // the disk could have changed while the sheet was open (another disk, a second tab): plan and backup are for this
  if (!same(await bytesOf(file), data)) return fail(`${path} changed since it was read. Nothing written; run it again.`, { backupId, plan, summary });

  const dotBefore = await exists(config, APPLE_DOUBLE);
  let ok: boolean;
  try {
    ok = await writeChecked(file, next);
  } catch (e) {
    // Chrome writes to a swap file and moves it over the original on close, so a failed write leaves the file as it was
    const still = same(await bytesOf(file).catch(() => new Uint8Array(0)), data);
    return fail(`writing ${path} failed (${(e as Error).message}). `
      + (still ? "The file on the disk is unchanged." : `Put the backup ${backupId} back.`), { backupId, plan, summary });
  }
  if (!ok) {
    log("verify:  the file read back differently; putting the original back");
    const back = await writeChecked(file, data).catch(() => false);
    return fail(back ? `${path} read back differently; the original is back on the disk.`
      : `${path} read back differently and the original could not be put back: put the backup ${backupId} back.`, { backupId, plan, summary });
  }
  log(`written and read back (sha256 ${(await sha256(next)).slice(0, 16)}).`);
  if (await exists(config, APPLE_DOUBLE)) {
    const w = `config/${APPLE_DOUBLE} is on the disk${dotBefore ? "" : " (macOS wrote it with this write: file attributes on a FAT disk)"}; `
      + "the OP-Z will move it to rejected/ at the eject (harmless). This app leaves it alone.";
    warnings.push(w);
    log(`warning: ${w}`);
  }
  log("Now eject the OP-Z disk: the OP-Z imports the file, reloads its presets and restarts. "
    + (request.kind === "restore" ? "The presets in the backup are back from then on."
      : "They are user presets from then on and stay with nothing connected.")
    + ` To undo: content mode again, then put the backup ${backupId} back (step 5, Restore).`);
  return result(true, { written: true, backupId, plan, summary });
}

interface Steps {
  lines: string[];
  log(line: string): void;
  fail(error: string, extra?: Partial<BakeResult>): BakeResult;
  result(ok: boolean, extra?: Partial<BakeResult>): BakeResult;
}

/** The disk has no userPresets.dat (see the file comment): plan into an empty store, ask, create, read back. */
async function createStore(diskName: string, config: DiskDir, request: Extract<BakeRequest, { kind: "bake" }>, deps: BakeDeps, s: Steps): Promise<BakeResult> {
  const path = `${diskName}/${USER_PRESETS}`;
  s.log(`${path} is not on the disk: no preset was ever stored on this OP-Z. A new file is made with only our presets; `
    + "the OP-Z takes it at the eject even though it had none.");
  const plan = planBake(emptyUserPresets(), request.engines, request.overwriteUser);
  for (const l of plan.lines) s.log(l);
  if (!plan.ok) return s.fail(plan.lines.at(-1)!.replace(/^error: +/, ""), { plan });
  if (!plan.data) {
    s.log("nothing to write: none of the picks has presets.");
    return s.result(true, { plan });
  }
  const next = plan.data;
  s.log("backup: none needed (there is no file yet)");
  if (!(await deps.confirm({ request, disk: diskName, plan, backupId: null, created: true, lines: [...s.lines] }))) {
    s.log("cancelled.");
    return s.result(false, { cancelled: true, plan });
  }
  if (await exists(config, "userPresets.dat")) return s.fail(`${path} appeared since the disk was read. Nothing written; run it again.`, { plan });

  const dotBefore = await exists(config, APPLE_DOUBLE);
  let ok: boolean;
  try {
    ok = await writeChecked(await config.getFileHandle("userPresets.dat", { create: true }), next);
  } catch (e) {
    ok = false;
    s.log(`error:   writing ${path} failed (${(e as Error).message})`);
  }
  if (!ok) {
    s.log("verify:  the new file is not right; deleting it so the OP-Z doesn't take it");
    const gone = await config.removeEntry("userPresets.dat").then(() => true, (e) => named(e) === "NotFoundError");
    return s.fail(gone ? `${path} could not be written correctly; it is deleted again, the disk is as it was.`
      : `${path} could not be written correctly and could not be deleted: delete config/userPresets.dat on the disk before you eject.`, { plan });
  }
  s.log(`written and read back (sha256 ${(await sha256(next)).slice(0, 16)}).`);
  const warnings: string[] = [];
  if (await exists(config, APPLE_DOUBLE)) {
    const w = `config/${APPLE_DOUBLE} is on the disk${dotBefore ? "" : " (macOS wrote it with this write: file attributes on a FAT disk)"}; `
      + "the OP-Z will move it to rejected/ at the eject (harmless). This app leaves it alone.";
    warnings.push(w);
    s.log(`warning: ${w}`);
  }
  s.log("Now eject the OP-Z disk: the OP-Z imports the new file, loads the presets and restarts. They are user presets "
    + "from then on and stay with nothing connected. Nothing to undo: the OP-Z had no stored presets before.");
  return s.result(true, { written: true, created: true, plan, warnings });
}

async function sha256(data: Uint8Array): Promise<string> {
  return toHex(new Uint8Array(await crypto.subtle.digest("SHA-256", data as Uint8Array<ArrayBuffer>)));
}

/** backup.json as `opz.py presets bake` writes it next to the copy (source, sha256, size). */
export function backupMeta(source: string, sha: string, size: number): string {
  return JSON.stringify({ source, sha256: sha, size }, null, 2);
}
