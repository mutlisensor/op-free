/** The send/return model of src/mixer.ts: the send crossfades, the effect's dry level keeps the dry path. */

import assert from "node:assert/strict";
import { test } from "node:test";
import { Ramp, addBlock, gains, scaleBlock } from "../src/mixer.ts";

const one = (send: number, dry: number) => gains([{ sendByte: send, dryLevelByte: dry }]);

test("send 0: all dry, nothing into the effect", () => {
  assert.deepEqual(one(0, 0), { sends: [0], dry: 1 });
});

test("send 255 with dry level 0: heard only through the effect (crossfade)", () => {
  assert.deepEqual(one(255, 0), { sends: [1], dry: 0 });
  const half = one(128, 0);
  assert.ok(Math.abs(half.sends[0] + half.dry - 1) < 1e-12);
});

test("dry level 255 keeps the dry path at any send (send/return)", () => {
  for (const s of [0, 64, 200, 255]) assert.equal(one(s, 255).dry, 1);
});

test("two sends: each effect gets its own send; the dry path is what both leave", () => {
  const g = gains([{ sendByte: 255, dryLevelByte: 255 }, { sendByte: 102, dryLevelByte: 0 }]);
  assert.deepEqual(g.sends, [1, 0.4]);
  assert.ok(Math.abs(g.dry - 0.6) < 1e-12);
  assert.equal(gains([{ sendByte: 255, dryLevelByte: 0 }, { sendByte: 255, dryLevelByte: 0 }]).dry, 0);
  assert.deepEqual(gains([]), { sends: [], dry: 1 });
});

test("scaleBlock ramps across the block and saturates", () => {
  const src = new Int32Array([1000, -1000, 1000, -1000, 2147483647, -2147483648, 1000, 1000]);
  const out = new Int32Array(src.length);
  scaleBlock(src, 0, 1, out);
  assert.deepEqual([...out.slice(0, 2)], [0, 0]);
  assert.equal(out[2], 250);
  scaleBlock(src, 2, 2, out);
  assert.equal(out[4], 2147483647);
  assert.equal(out[5], -2147483648);
});

test("Ramp settles on its target", () => {
  const r = new Ramp(0);
  for (let i = 0; i < 200; i++) r.step(1);
  assert.equal(r.value, 1);
});

test("addBlock sums tracks into one bus, ramped and saturating", () => {
  const acc = new Int32Array([10, 10, 10, 10]);
  addBlock(new Int32Array([100, -100, 100, -100]), 1, 1, acc);
  assert.deepEqual([...acc], [110, -90, 110, -90]);
  addBlock(new Int32Array([400, 400, 400, 400]), 0, 1, acc);
  assert.deepEqual([...acc], [110, -90, 310, 110]);
  const full = new Int32Array([2147483000, -2147483000]);
  addBlock(new Int32Array([1000, -1000]), 1, 1, full);
  assert.deepEqual([...full], [2147483647, -2147483648]);
  const before = [...acc];
  addBlock(new Int32Array([5, 5, 5, 5]), 0, 0, acc);
  assert.deepEqual([...acc], before);
});
