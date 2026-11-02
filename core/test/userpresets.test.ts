/**
 * config/userPresets.dat (src/userpresets.ts): the shared vectors from tools/record_vectors.py (`opz.py presets bake`
 * on a temporary content disk, log lines and bytes), then a port of tools/tests/test_userpresets.py's format and bake
 * tests.
 */

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { describe, it } from "node:test";

import {
  type Doc, USER_PRESETS_ID_AT, USER_PRESETS_MASK_AT, USER_PRESETS_PRESET, USER_PRESETS_RECORD, USER_PRESETS_RECORDS,
  USER_PRESETS_SIZE, USER_PRESETS_SLOT_STRIDE, USER_PRESETS_USER_AT, UserPresetsError, bakeUserPresets, bytesEqual, changedRecords,
  fromHex, parseUserPresets, planBake, planRestore, presetBytes, readUserPresetsFile, recordMask, recordPlugId,
  serialiseUserPresets, storedUserPresets, userPresetsSummary,
} from "../src/index.ts";

interface Sparse { size: number; runs: Array<[number, string]> }
interface Case {
  name: string;
  description: string;
  before: Sparse;
  engines: string[];
  overwrite_user: boolean;
  show: boolean;
  restore: Sparse | null;
  result: number;
  logs: string[];
  after: Sparse | null;
}
const vector: { engines: Record<string, { manifest: Doc; presets: Doc[] }>; cases: Case[] } =
  JSON.parse(readFileSync(new URL("../test-vectors/userpresets.json", import.meta.url), "utf8"));

function unsparse(s: Sparse): Uint8Array {
  const out = new Uint8Array(s.size);
  for (const [at, hex] of s.runs) out.set(fromHex(hex), at);
  return out;
}

describe("userPresets.dat vectors (opz.py presets bake)", () => {
  for (const c of vector.cases) {
    it(`${c.name}: ${c.description}`, async () => {
      // the Python's lines up to the write; the backup is the app's (it downloads one), so its line is not ours
      const end = c.logs.findIndex((l) => l.startsWith("written and read back"));
      const want = c.logs.slice(0, end < 0 ? undefined : end).filter((l) => !l.startsWith("backup: "));
      const before = unsparse(c.before);
      const read = await readUserPresetsFile(before);
      const lines = [...read.lines];
      let ok = read.records !== null;
      let data: Uint8Array | null = null;
      if (ok && !c.show) {
        if (c.restore) {
          const backup = unsparse(c.restore);
          const plan = await planRestore(backup, "<restore>");
          lines.push(...plan.lines);
          ok = plan.ok;
          if (ok) data = backup;
        } else {
          const engines = c.engines.map((k) => ({ name: vector.engines[k].manifest.engine, ...vector.engines[k] }));
          const plan = planBake(before, engines, c.overwrite_user);
          lines.push(...plan.lines);
          ok = plan.ok;
          data = plan.data;
        }
      }
      assert.deepEqual(lines, want);
      assert.equal(ok ? 0 : 1, c.result);
      if (c.after) assert.ok(data && bytesEqual(data, unsparse(c.after)), "same bytes as the Python wrote");
      else assert.ok(data === null || bytesEqual(data, before), "nothing to write");
    });
  }
});

// -- port of tools/tests/test_userpresets.py ---------------------------------------------------------------------------

const TABLE = 61016;

function preset(slot = 0, index = 0, name = "p", values: number[] = Array(18).fill(7), octave = 0): Doc {
  return { slot, index, name, defaultOctave: octave, values: [...values] };
}

function u16(r: Uint8Array, at: number, v: number): void {
  new DataView(r.buffer, r.byteOffset, r.byteLength).setUint16(at, v, true);
}

function u32(r: Uint8Array, at: number, v: number): void {
  new DataView(r.buffer, r.byteOffset, r.byteLength).setUint32(at, v, true);
}

/** A userPresets.dat the way savePresets writes it (see the Python's os_file). */
function osFile(user: Array<[number, number, number, string, number[]]> = [[TABLE, 0, 1, "my bass", Array(18).fill(9)]]): Uint8Array {
  const records = Array.from({ length: USER_PRESETS_RECORDS }, () => new Uint8Array(USER_PRESETS_RECORD));
  u32(records[0], USER_PRESETS_ID_AT, 10);
  records[0].set(presetBytes("factory", Array(18).fill(1), -1), 0);
  u16(records[0], USER_PRESETS_MASK_AT, 1);
  u32(records[1], USER_PRESETS_ID_AT, TABLE);
  for (const [pid, slot, index, name, values] of user) {
    const r = records.find((x) => recordPlugId(x) === pid)!;
    r.set(presetBytes(name, values, 0), slot * USER_PRESETS_SLOT_STRIDE + index * USER_PRESETS_PRESET);
    for (const m of [USER_PRESETS_MASK_AT, USER_PRESETS_USER_AT]) u16(r, m + 2 * slot, recordMask(r, m, slot) | (1 << index));
  }
  records[5].fill(0xaa, 0, 40); // a free record with stale bytes (id 0)
  return serialiseUserPresets(records);
}

/** loadPresets' user loop: for installed ids, the presets whose user bit is set, by "slot/index". */
function osLoad(data: Uint8Array, installed: Set<number>): Map<number, Map<string, ReturnType<typeof storedUserPresets>[number]>> {
  const out = new Map();
  for (const r of parseUserPresets(data)) {
    const pid = recordPlugId(r);
    if (pid && installed.has(pid)) out.set(pid, new Map(storedUserPresets(r).map((p) => [`${p.slot}/${p.index}`, p])));
  }
  return out;
}

const counts = (r: { new: number; same: number; updated: number; replaced: number; kept: unknown[] }) =>
  ({ new: r.new, same: r.same, updated: r.updated, replaced: r.replaced, kept: r.kept });

describe("userPresets.dat format", () => {
  it("round trip and summary", () => {
    const data = osFile();
    const records = parseUserPresets(data);
    assert.ok(bytesEqual(serialiseUserPresets(records), data));
    assert.equal(data.length, USER_PRESETS_SIZE);
    assert.equal(USER_PRESETS_SIZE, 351202);
    assert.equal(userPresetsSummary(records)[0], "2 plug records, 198 free; 1 with user presets");
    assert.match(userPresetsSummary(records)[1], /plug 61016: slot 0 index 1 'my bass'/);
  });

  it("strict checks", () => {
    const data = osFile();
    assert.throws(() => parseUserPresets(data.slice(0, -1)), /must be 351202 bytes/);
    const v2 = data.slice();
    v2[0] = 2;
    assert.throws(() => parseUserPresets(v2), /version is 2/);
    let records = parseUserPresets(data);
    u32(records[3], USER_PRESETS_ID_AT, TABLE);
    assert.throws(() => parseUserPresets(serialiseUserPresets(records)), /plug 61016 has two records/);
    records = parseUserPresets(data);
    u16(records[1], USER_PRESETS_USER_AT, 1 << 14);
    assert.throws(() => parseUserPresets(serialiseUserPresets(records)), (e) => e instanceof UserPresetsError && /past index 13/.test(e.message));
  });

  it("preset bytes", () => {
    const b = presetBytes("a name longer than eleven", Array.from({ length: 18 }, (_, i) => i), 9);
    assert.equal(b.length, 31);
    assert.deepEqual([...b.subarray(0, 12)], [..."a name long"].map((c) => c.charCodeAt(0)).concat(0));
    assert.deepEqual([...b.subarray(12, 30)], Array.from({ length: 18 }, (_, i) => i));
    assert.equal((b[30] << 24) >> 24, 4, "octave clamped like readPresetSettings");
    assert.equal((presetBytes("x", Array(18).fill(0), -9)[30] << 24) >> 24, -4);
    assert.throws(() => presetBytes("café", Array(18).fill(0), 0), UserPresetsError);
    assert.throws(() => presetBytes("x", Array(17).fill(0), 0), UserPresetsError);
    assert.throws(() => presetBytes("x", [...Array(17).fill(0), 256], 0), UserPresetsError);
  });
});

describe("bake", () => {
  it("loads as user presets and keeps the user's", () => {
    const records = parseUserPresets(osFile());
    const ours = [preset(0, 0, "wave bass", Array(18).fill(1), -2), preset(0, 1, "reso bass", Array(18).fill(2)), preset(3, 13, "chord")];
    const report = bakeUserPresets(records, [[TABLE, "table", ours]]);
    assert.deepEqual(counts(report.get("table")!), { new: 2, same: 0, updated: 0, replaced: 0, kept: [[0, 1, "my bass"]] });
    const loaded = osLoad(serialiseUserPresets(records), new Set([10, TABLE]));
    assert.deepEqual([...loaded.get(TABLE)!.keys()], ["0/0", "0/1", "3/13"]);
    assert.equal(loaded.get(TABLE)!.get("0/0")!.name, "wave bass");
    assert.equal(loaded.get(TABLE)!.get("0/0")!.defaultOctave, -2);
    assert.equal(loaded.get(TABLE)!.get("0/1")!.name, "my bass", "the hand-stored preset wins by default");
    assert.equal(loaded.get(10)!.size, 0, "TE's record untouched");
  });

  it("overwrite user and same", () => {
    const records = parseUserPresets(osFile());
    const ours = [preset(0, 1, "reso bass", Array(18).fill(2))];
    const replaced = bakeUserPresets(records, [[TABLE, "table", ours]], true).get("table")!;
    assert.equal(replaced.replaced, 1);
    assert.deepEqual(replaced.replacedKeys, [[0, 1, "my bass"]], "which stored preset went, for the confirm sheet");
    assert.deepEqual(counts(bakeUserPresets(records, [[TABLE, "table", ours]]).get("table")!),
      { new: 0, same: 1, updated: 0, replaced: 0, kept: [] });
  });

  it("an earlier bake is updated but hand-stored presets stay", () => {
    const records = parseUserPresets(osFile([[TABLE, 0, 3, "", Array(18).fill(5)]])); // stored on the device: no name
    bakeUserPresets(records, [[TABLE, "table", [preset(0, 0, "wave bass", Array(18).fill(0)), preset(0, 3, "sub organ")]]]);
    const fixed = [preset(0, 0, "wave bass", [...Array(13).fill(0), 128, 100, 255, 0, 0]), preset(0, 3, "sub organ")];
    assert.deepEqual(counts(bakeUserPresets(records, [[TABLE, "table", fixed]]).get("table")!),
      { new: 0, same: 0, updated: 1, replaced: 0, kept: [[0, 3, ""]] });
    const loaded = osLoad(serialiseUserPresets(records), new Set([TABLE])).get(TABLE)!;
    assert.equal(loaded.get("0/0")!.values[13], 128);
    assert.deepEqual(loaded.get("0/3")!.values, Array(18).fill(5));
  });

  it("a plug without a record gets a zeroed free one", () => {
    const records = parseUserPresets(osFile());
    const before = records.map((r) => r.slice());
    bakeUserPresets(records, [[61017, "modal", [preset(1, 0, "glass")]]]);
    const changed = changedRecords(before, records);
    assert.equal(changed.length, 1);
    const r = records[changed[0]];
    assert.equal(recordPlugId(r), 61017);
    assert.deepEqual(storedUserPresets(r).map((p) => p.name), ["glass"]);
    assert.deepEqual([...r.subarray(USER_PRESETS_PRESET, USER_PRESETS_PRESET + 9)], Array(9).fill(0), "stale bytes cleared");
    assert.equal(recordMask(r, USER_PRESETS_MASK_AT, 1), 1);
  });

  it("only our range and good keys", () => {
    const records = parseUserPresets(osFile());
    assert.throws(() => bakeUserPresets(records, [[10, "analoga", [preset()]]]), /TE's range/);
    assert.throws(() => bakeUserPresets(records, [[TABLE, "table", [preset(0, 14)]]]), /slot 0..3 and index 0..13/);
  });

  it("every repo engine bakes (the app's catalogue)", async () => {
    const { readdirSync, existsSync } = await import("node:fs");
    const root = new URL("../../engines/", import.meta.url);
    const engines = readdirSync(root).filter((d) => existsSync(new URL(`${d}/plug.json`, root))).map((d) => {
      const manifest = JSON.parse(readFileSync(new URL(`${d}/plug.json`, root), "utf8"));
      const file = typeof manifest.presets === "string" ? new URL(`${d}/${manifest.presets}`, root) : null;
      return { name: manifest.engine ?? d, manifest, presets: file && existsSync(file) ? JSON.parse(readFileSync(file, "utf8")) : [] };
    }).filter((e) => e.presets.length);
    assert.ok(engines.length > 5);
    const plan = planBake(osFile([]), engines);
    assert.ok(plan.ok, plan.lines.join("\n"));
    const loaded = osLoad(plan.data!, new Set(plan.engines.map((e) => e.manifest.plug.id)));
    for (const e of plan.engines) assert.equal(loaded.get(e.manifest.plug.id)!.size, e.presets.length, e.name);
  });
});
