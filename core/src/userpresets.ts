/**
 * config/userPresets.dat: the OP-Z's own store of user presets (the ones stored with track + white key, 2 s). Port of
 * tools/opz_userpresets.py and of the offline part of `opz.py presets bake` (no files, no device: the caller reads
 * and writes the file, on the content-mode disk).
 *
 * Layout, from the OS image (research/presets.md, "Presets that stay"): a u16 version (1), then exactly 200
 * records of 0x6dc bytes, little-endian. A record is a plug's preset block plus the plug id (u32 at +0x6d8; 0 =
 * unused). In the block, slot s (0-3) index i (0-13) is 31 bytes at s*434 + i*31: name (12 bytes, NUL-terminated, 11
 * characters), the 18 values, defaultOctave (signed byte). Then the preset masks u16[4] at 1736 and the user masks
 * u16[4] at 1744. At every start the OS copies, for each record whose plug is installed, the presets whose
 * **user-mask** bit is set; it rewrites the whole file from RAM whenever the user stores a preset.
 */

import { bytesEqual, readU16le, readU32le, toHex } from "./bytes.ts";
import { type Doc, USER_PLUG_MIN, presetProblems } from "./plan.ts";
import { pyInt, pyRepr } from "./pyjson.ts";

/** Where the file sits on the content-mode disk. */
export const USER_PRESETS = "config/userPresets.dat";
export const USER_PRESETS_VERSION = 1;
export const USER_PRESETS_RECORDS = 200;
export const USER_PRESETS_RECORD = 0x6dc; // 1756
export const USER_PRESETS_ID_AT = 0x6d8; // 1752
export const USER_PRESETS_SLOT_STRIDE = 434; // 14 * 31
export const USER_PRESETS_PRESET = 31;
export const USER_PRESETS_NAME = 12;
export const USER_PRESETS_MASK_AT = 1736;
export const USER_PRESETS_USER_AT = 1744;
export const USER_PRESETS_SIZE = 2 + USER_PRESETS_RECORDS * USER_PRESETS_RECORD; // 351,202
const SLOTS = 4;
const INDEXES = 14;
const ALL_BITS = (1 << INDEXES) - 1;
const RECORD = USER_PRESETS_RECORD;
const PRESET = USER_PRESETS_PRESET;
const NAME = USER_PRESETS_NAME;

export class UserPresetsError extends Error {
  override name = "UserPresetsError";
}

/**
 * The 200 records, after the checks the OS makes (version, size) and stricter ones of ours: no plug id twice, no
 * mask bit past index 13. Throws UserPresetsError.
 */
export function parseUserPresets(data: Uint8Array): Uint8Array[] {
  if (data.length !== USER_PRESETS_SIZE) {
    throw new UserPresetsError(`userPresets.dat must be ${USER_PRESETS_SIZE} bytes, this one is ${data.length}`);
  }
  const version = readU16le(data, 0);
  if (version !== USER_PRESETS_VERSION) {
    throw new UserPresetsError(`userPresets.dat version is ${version}, expected ${USER_PRESETS_VERSION}`);
  }
  const records = Array.from({ length: USER_PRESETS_RECORDS }, (_, n) => data.slice(2 + n * RECORD, 2 + (n + 1) * RECORD));
  const seen = new Map<number, number>();
  records.forEach((r, n) => {
    const pid = recordPlugId(r);
    if (pid === 0) return;
    if (seen.has(pid)) throw new UserPresetsError(`plug ${pid} has two records (${seen.get(pid)} and ${n})`);
    seen.set(pid, n);
    for (let s = 0; s < SLOTS; s++) {
      for (const at of [USER_PRESETS_MASK_AT, USER_PRESETS_USER_AT]) {
        const m = recordMask(r, at, s);
        if (m & ~ALL_BITS) {
          throw new UserPresetsError(`plug ${pid} slot ${s}: mask bits past index 13 (0x${m.toString(16).padStart(4, "0")})`);
        }
      }
    }
  });
  return records;
}

export function serialiseUserPresets(records: Uint8Array[]): Uint8Array {
  if (records.length !== USER_PRESETS_RECORDS || records.some((r) => r.length !== RECORD)) {
    throw new UserPresetsError(`need 200 records of ${RECORD} bytes`);
  }
  const out = new Uint8Array(USER_PRESETS_SIZE);
  out[0] = USER_PRESETS_VERSION & 0xff;
  out[1] = USER_PRESETS_VERSION >> 8;
  records.forEach((r, n) => out.set(r, 2 + n * RECORD));
  return out;
}

export function recordPlugId(record: Uint8Array): number {
  return readU32le(record, USER_PRESETS_ID_AT);
}

export function recordMask(record: Uint8Array, at: number, slot: number): number {
  return readU16le(record, at + 2 * slot);
}

function setMask(record: Uint8Array, at: number, slot: number, value: number): void {
  record[at + 2 * slot] = value & 0xff;
  record[at + 2 * slot + 1] = (value >> 8) & 0xff;
}

/**
 * 31 bytes as the device keeps a preset: 11 characters + NUL (zero-padded), 18 values, octave (clamped -4..4, as
 * readPresetSettings clamps it).
 */
export function presetBytes(name: string, values: unknown, octave: unknown): Uint8Array {
  if (!/^[\x00-\x7f]*$/.test(name)) throw new UserPresetsError(`preset name ${pyRepr(name)}: only ASCII fits the device`);
  const v = Array.isArray(values) ? values : [];
  if (v.length !== 18 || !v.every((x) => Number.isInteger(x) && x >= 0 && x <= 255)) {
    throw new UserPresetsError(`preset ${pyRepr(name)} needs 18 values in 0..255`);
  }
  const out = new Uint8Array(PRESET);
  for (let i = 0; i < Math.min(name.length, NAME - 1); i++) out[i] = name.charCodeAt(i);
  out.set(v as number[], NAME);
  out[30] = Math.max(-4, Math.min(4, pyInt(octave))) & 0xff;
  return out;
}

export interface StoredPreset {
  slot: number;
  index: number;
  name: string;
  values: number[];
  defaultOctave: number;
  /** The user-mask bit: the OS loads it at every start. */
  user: boolean;
  raw: Uint8Array;
}

export function readStoredPreset(record: Uint8Array, slot: number, index: number): StoredPreset {
  const at = slot * USER_PRESETS_SLOT_STRIDE + index * PRESET;
  const raw = record.slice(at, at + PRESET);
  const end = raw.subarray(0, NAME).indexOf(0);
  const name = String.fromCharCode(...raw.subarray(0, end < 0 ? NAME : end)); // latin-1
  return {
    slot, index, name, values: [...raw.subarray(NAME, NAME + 18)], defaultOctave: (raw[30] << 24) >> 24,
    user: Boolean((recordMask(record, USER_PRESETS_USER_AT, slot) >> index) & 1), raw,
  };
}

/** The presets the OS would load from this record (user-mask bit set). */
export function storedUserPresets(record: Uint8Array): StoredPreset[] {
  const out: StoredPreset[] = [];
  for (let s = 0; s < SLOTS; s++) {
    const m = recordMask(record, USER_PRESETS_USER_AT, s);
    for (let i = 0; i < INDEXES; i++) if ((m >> i) & 1) out.push(readStoredPreset(record, s, i));
  }
  return out;
}

export function userPresetsSummary(records: Uint8Array[]): string[] {
  const used = records.filter((r) => recordPlugId(r));
  const lines = [`${used.length} plug records, ${USER_PRESETS_RECORDS - used.length} free; `
    + `${used.filter((r) => storedUserPresets(r).length).length} with user presets`];
  for (const r of used) {
    const ups = storedUserPresets(r);
    if (ups.length) lines.push(`  plug ${recordPlugId(r)}: ${ups.map((p) => `slot ${p.slot} index ${p.index} ${pyRepr(p.name)}`).join(", ")}`);
  }
  return lines;
}

/** A key on the keyboard: slot (0 bass … 3 chord) and preset index (white key 0–13), plus the name stored there. */
export type StoredKey = [slot: number, index: number, name: string];

export interface BakeCounts {
  new: number;
  same: number;
  updated: number;
  replaced: number;
  /** Hand-stored user presets left in place (no overwriteUser). */
  kept: StoredKey[];
  /** Hand-stored user presets overwritten (overwriteUser): their keys and the name they had (not in the Python). */
  replacedKeys: StoredKey[];
}

/**
 * Write our presets into `records` (in place) as user presets. plugs = [plug id, label, presets], presets in
 * presets.json form (slot, index, name, values, defaultOctave or default_octave). A plug without a record gets a
 * free one (zeroed first). A user preset at the same key that carries our preset's name is ours from an earlier bake
 * and is updated; any other one (a preset stored on the device gets no name) is kept unless overwriteUser.
 */
export function bakeUserPresets(records: Uint8Array[], plugs: Array<[number, string, Doc[]]>, overwriteUser = false): Map<string, BakeCounts> {
  const report = new Map<string, BakeCounts>();
  const byId = new Map<number, Uint8Array>();
  for (const r of records) if (recordPlugId(r)) byId.set(recordPlugId(r), r);
  for (const [pid, label, presets] of plugs) {
    if (pid < USER_PLUG_MIN) throw new UserPresetsError(`plug ${pid} is in TE's range: not ours to bake`);
    let r = byId.get(pid);
    if (r === undefined) {
      r = records.find((x) => recordPlugId(x) === 0);
      if (r === undefined) throw new UserPresetsError(`no free record for plug ${pid} (all 200 used)`);
      r.fill(0);
      new DataView(r.buffer, r.byteOffset, r.byteLength).setUint32(USER_PRESETS_ID_AT, pid, true);
      byId.set(pid, r);
    }
    let rep = report.get(label);
    if (!rep) report.set(label, (rep = { new: 0, same: 0, updated: 0, replaced: 0, kept: [], replacedKeys: [] }));
    for (const p of presets) {
      const slot = pyInt(p.slot ?? 0);
      const index = pyInt(p.index ?? 0);
      if (!(slot >= 0 && slot < SLOTS && index >= 0 && index < INDEXES)) {
        throw new UserPresetsError(`${label} preset ${pyRepr(p.name ?? null)}: slot 0..3 and index 0..13`);
      }
      const octave = p.defaultOctave ?? p.default_octave ?? 0;
      const want = presetBytes(String(p.name ?? ""), p.values, octave);
      const have = readStoredPreset(r, slot, index);
      if (have.user && bytesEqual(have.raw, want)) {
        rep.same++;
        continue;
      }
      const ours = have.user && have.name !== "" && bytesEqual(have.raw.subarray(0, NAME), want.subarray(0, NAME));
      if (have.user && !ours && !overwriteUser) {
        rep.kept.push([slot, index, have.name]);
        continue;
      }
      r.set(want, slot * USER_PRESETS_SLOT_STRIDE + index * PRESET);
      setMask(r, USER_PRESETS_MASK_AT, slot, recordMask(r, USER_PRESETS_MASK_AT, slot) | (1 << index));
      setMask(r, USER_PRESETS_USER_AT, slot, recordMask(r, USER_PRESETS_USER_AT, slot) | (1 << index));
      if (ours) rep.updated++;
      else if (have.user) {
        rep.replaced++;
        rep.replacedKeys.push([slot, index, have.name]);
      } else rep.new++;
    }
  }
  return report;
}

export function changedRecords(before: Uint8Array[], after: Uint8Array[]): number[] {
  const out: number[] = [];
  for (let n = 0; n < USER_PRESETS_RECORDS; n++) if (!bytesEqual(before[n], after[n])) out.push(n);
  return out;
}

// -- the offline part of `opz.py presets bake` (bake_presets), with its log lines -------------------------------------

async function sha256(data: Uint8Array): Promise<string> {
  return toHex(new Uint8Array(await crypto.subtle.digest("SHA-256", data as Uint8Array<ArrayBuffer>)));
}

function storedKeyText([slot, index, name]: StoredKey): string {
  return `slot ${slot} index ${index} ${pyRepr(name)}`;
}

export interface ReadUserPresets {
  /** null when the file failed the checks (the error line says why). */
  records: Uint8Array[] | null;
  lines: string[];
  sha256: string;
}

/** Read and check the file as bake_presets does: strict parse, byte-for-byte round trip, then the summary. */
export async function readUserPresetsFile(data: Uint8Array, path = USER_PRESETS): Promise<ReadUserPresets> {
  const sum = await sha256(data);
  let records: Uint8Array[];
  try {
    records = parseUserPresets(data);
    if (!bytesEqual(serialiseUserPresets(records), data)) throw new UserPresetsError("the file does not read back byte for byte");
  } catch (e) {
    if (!(e instanceof UserPresetsError)) throw e;
    return { records: null, lines: [`error:   ${path}: ${e.message}. Nothing written.`], sha256: sum };
  }
  return { records, lines: [`read ${path} (${data.length} bytes, sha256 ${sum.slice(0, 16)})`, ...userPresetsSummary(records)], sha256: sum };
}

/** An engine as the bake needs it: its name, plug.json and presets.json. */
export interface BakeEngine {
  name: string;
  manifest: Doc;
  presets: Doc[];
}

export interface BakePlan {
  ok: boolean;
  lines: string[];
  /** The file to write; null on an error or when nothing changes. */
  data: Uint8Array | null;
  report: Map<string, BakeCounts>;
  /** The engines baked (presets present and free of errors), in order. */
  engines: BakeEngine[];
  /** Indexes of the records that change. */
  changed: number[];
}

/**
 * Plan a bake of `engines` into the checked file `data` (records from readUserPresetsFile): the plan lines of
 * bake_presets, from the engines left out to "N of 200 records change". Engines without presets are skipped, ones
 * whose presets.json has errors are left out with a warning.
 */
export function planBake(data: Uint8Array, engines: BakeEngine[], overwriteUser = false): BakePlan {
  const records = parseUserPresets(data);
  const lines: string[] = [];
  const plugs: Array<[number, string, Doc[]]> = [];
  const baked: BakeEngine[] = [];
  for (const e of engines) {
    if (!e.presets.length) continue;
    if (presetProblems(e.presets, pyInt(e.manifest.plug?.trackmask ?? 0))[0].length) {
      lines.push(`warning: ${e.name}: presets.json has errors (opz.py presets check): left out`);
      continue;
    }
    plugs.push([pyInt(e.manifest.plug.id), e.name, e.presets]);
    baked.push(e);
  }
  const after = records.map((r) => r.slice());
  let report: Map<string, BakeCounts>;
  try {
    report = bakeUserPresets(after, plugs, overwriteUser);
  } catch (e) {
    if (!(e instanceof UserPresetsError)) throw e;
    lines.push(`error:   ${e.message}. Nothing written.`);
    return { ok: false, lines, data: null, report: new Map(), engines: baked, changed: [] };
  }
  for (const [name, r] of report) {
    const kept = r.kept.length ? "; kept your stored " + r.kept.map(storedKeyText).join(", ") : "";
    lines.push(`plan: ${name}: ${r.new} new, ${r.updated} updated (from an earlier bake), ${r.replaced} replaced, ${r.same} already there${kept}`);
  }
  const out = serialiseUserPresets(after);
  if (bytesEqual(out, data)) {
    lines.push("nothing to change: these presets are already in the file as user presets.");
    return { ok: true, lines, data: null, report, engines: baked, changed: [] };
  }
  const changed = changedRecords(records, after);
  lines.push(`plan: ${changed.length} of 200 records change; every other byte stays`);
  return { ok: true, lines, data: out, report, engines: baked, changed };
}

/** Plan putting a backed-up file back (bake_presets --restore): it must pass the same checks. */
export async function planRestore(backup: Uint8Array, source: string): Promise<{ ok: boolean; lines: string[] }> {
  try {
    parseUserPresets(backup);
  } catch (e) {
    if (!(e instanceof UserPresetsError)) throw e;
    return { ok: false, lines: [`error:   ${source}: ${e.message}. Nothing written.`] };
  }
  return { ok: true, lines: [`plan: put ${source} back (sha256 ${(await sha256(backup)).slice(0, 16)})`] };
}
