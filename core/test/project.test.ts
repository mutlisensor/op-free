/** Project files and patterns (src/project.ts) against test-vectors/project.json from tools/opz_project.py. */

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { describe, it } from "node:test";

import { project as op } from "../src/index.ts";

const vector = JSON.parse(readFileSync(new URL("../test-vectors/project.json", import.meta.url), "utf8"));
const sha256 = (data: Uint8Array) => createHash("sha256").update(data).digest("hex");

/** The same edits as opz_project.vector_project(). */
function vectorProject(): op.Project {
  const pr = new op.Project();
  pr.setTempo(97);
  op.writeRhythm(pr, 0, 0, op.euclid(5, 16), 53, 110);
  op.writeRhythm(pr, 0, 1, op.euclid(3, 8, 2), 55, 100, 0.5);
  const chord = [60, 64, 67, 71, 74, 77, 81, 84];
  const micro = [-12, -4, 0, 3, 5, 8, 10, 11];
  pr.setNotes(0, 6, 3, chord.map((n, i) => ({ note: n, velocity: 64 + (n % 7), duration: 3100, micro: micro[i]! })));
  pr.setComponent(0, 4, 2, "glide", 7);
  pr.setComponent(0, 4, 2, "pulse", 4);
  pr.setComponent(0, 4, 2, "pulse", null);
  pr.setLock(0, 5, 0, "filter", 200);
  pr.setLock(0, 5, 1, "filter", 10);
  pr.setLock(0, 5, 1, "filter", null);
  pr.setSound(0, 5, Array.from({ length: 18 }, (_, i) => 100 + i));
  pr.setTrack(0, 4, { step_count: 12, step_length: 2, plug: 61016, note_length: 32, quantize: 255 });
  pr.setMuted(0, 3, true);
  pr.setMuted(0, 7, true, 2);
  op.writePages(pr, 4, Array.from({ length: 40 }, (_, s) => (s % 3 === 0 ? [{ note: 36 + (s % 12) }] : [])), 8, 4);
  pr.setChain(0, [0, 0, 1, 15]);
  return pr;
}

describe("project files", () => {
  it("builds the OS's defaults", () => {
    assert.equal(sha256(op.defaultPattern()), vector.default_pattern_sha256);
    assert.equal(sha256(op.defaultGlobal()), vector.default_global_sha256);
    const empty = new op.Project();
    assert.equal(empty.toBytes().length, op.FILE_SIZE);
    assert.deepEqual(empty.problems(), []);
  });

  it("makes the vector project byte for byte", () => {
    const pr = vectorProject();
    const data = pr.toBytes();
    assert.equal(data.length, vector.size);
    assert.equal(sha256(data), vector.sha256);
    assert.deepEqual(pr.problems(), vector.problems);
    assert.equal(sha256(op.Project.parse(data).toBytes()), vector.sha256);
  });

  it("reads like the Python tool", () => {
    const pr = vectorProject();
    const r = vector.reads;
    assert.deepEqual([[0, 0], [0, 4], [9, 4], [10, 4], [1, 15]].map(([p, t]) => pr.track(p!, t!)), r.track);
    assert.deepEqual([[0, 0, 0], [0, 0, 1], [0, 1, 2], [0, 6, 3], [9, 4, 3]].map(([p, t, s]) => pr.notes(p!, t!, s!)), r.notes);
    assert.deepEqual([pr.step(0, 4, 2), pr.step(0, 5, 0), pr.step(0, 5, 1), pr.step(3, 3, 3)], r.step);
    assert.deepEqual([pr.sound(0, 5), pr.sound(2, 0)], r.sound);
    assert.deepEqual([pr.muted(0, 3), pr.muted(0, 7), pr.muted(0, 7, 2)], r.muted);
    assert.deepEqual(pr.routing(0), r.routing);
    assert.deepEqual(pr.summary(), r.summary);
  });

  it("marks a pattern in use when it writes content into it (Q-50)", () => {
    const pr = new op.Project();
    assert.equal(pr.patternInUse(3), false);
    pr.setNotes(3, 0, 0, []);
    assert.equal(pr.patternInUse(3), false);
    pr.setNotes(3, 0, 0, [{ note: 53 }]);
    assert.equal(pr.patternInUse(3), true);
    assert.equal(pr.routing(3)[1], 0x08f0);
    pr.setNotes(3, 0, 0, []);
    assert.equal(pr.patternHasNotes(3), false);
    assert.equal(pr.patternInUse(3), true, "clearing notes never unmarks: the device keeps visited patterns marked");
    pr.copyPattern(3, 9);
    assert.equal(pr.patternInUse(9), true);
    pr.setPatternInUse(9, false);
    assert.equal(pr.patternInUse(9), false);
    assert.equal(pr.routing(9)[1], 0x00f0);
    for (const edit of [(x: op.Project) => x.setComponent(5, 4, 2, "glide", 7),
      (x: op.Project) => x.setLock(5, 5, 0, "filter", 200),
      (x: op.Project) => x.setSound(5, 5, Array.from({ length: 18 }, (_, i) => 100 + i)),
      (x: op.Project) => x.setTrack(5, 4, { plug: 61016 })]) {
      const q = new op.Project();
      edit(q);
      assert.equal(q.patternInUse(5), true);
      assert.equal(q.patternInUse(4), false);
    }
  });

  it("warns when a pattern holds notes but is not in use", () => {
    const pr = new op.Project();
    pr.setNotes(3, 0, 0, [{ note: 53 }]);
    assert.deepEqual(pr.problems(), []);
    pr.setPatternInUse(3, false);
    assert.match(pr.problems()[0]!, /^pattern 4: holds notes but is not marked in use/);
  });

  it("spreads Euclidean rhythms like the Python tool", () => {
    for (const [key, want] of Object.entries(vector.euclid as Record<string, number[]>)) {
      const [k, n, rot] = key.split(",").map(Number);
      assert.deepEqual(op.euclid(k!, n!, rot!).map(Number), want, key);
    }
  });

  it("keeps old files and refuses malformed ones", () => {
    const pr = new op.Project(undefined, undefined, 71, null);
    const data = pr.toBytes();
    assert.equal(data.length, op.OLD_FILE_SIZE);
    assert.equal(op.Project.parse(data).version, "71.0");
    assert.match(op.Project.parse(data).problems()[0]!, /^version 71\.0/);
    const good = new op.Project().toBytes();
    assert.throws(() => op.Project.parse(good.subarray(0, good.length - 4)), op.FormatError);
    const bad = good.slice();
    bad[0] = 74;
    assert.throws(() => op.Project.parse(bad), op.FormatError);
    assert.throws(() => op.Project.parse(good.subarray(1)), op.FormatError);
  });

  it("checks edits and names", () => {
    const pr = new op.Project();
    assert.throws(() => pr.setNotes(0, 8, 0, [{ note: 1 }, { note: 2 }]), op.FormatError);
    assert.throws(() => pr.setNotes(0, 0, 0, [{ note: 1, micro: 12 }]), op.FormatError);
    assert.throws(() => pr.setTrack(0, 0, { step_length: 11 }), op.FormatError);
    assert.throws(() => pr.setChain(15, [0]), op.FormatError);
    assert.equal(op.projectPath(10), "projects/project10.opz");
    assert.equal(op.projectFileId(1), 3);
    assert.throws(() => op.projectPath(0), op.FormatError);
    assert.throws(() => op.projectPath(11), op.FormatError);
    pr.bank[6] = 0;
    assert.ok(pr.problems().some((p) => p.includes("step length 0")));
  });
});
