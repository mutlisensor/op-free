/// <reference types="node" />
/** The picking model: the default eight, the cap of ten, slots kept original or skipped, community vs original. */

import assert from "node:assert/strict";
import { test } from "node:test";
import {
  DEFAULT_PICKS, MAX_PICKS, capacity, cautionOf, defaultChoice, engineGroup, engineTracks, freeSlots, group, groupEngines, sanitizeChoice,
  setSource, slotIntents, toggleOriginalSlot, togglePick,
} from "../model.ts";
import { catalogue, engine } from "./fixtures.ts";

test("the defaults: eight per group, featured first, two slots left", () => {
  assert.equal(DEFAULT_PICKS, 8);
  assert.equal(MAX_PICKS, 10);
  const c = defaultChoice(catalogue());
  assert.deepEqual(c.synth.picks, ["syn1", "syn2", "syn3", "syn4", "syn5", "syn6", "syn7", "syn8"], "nine featured: the first eight");
  assert.deepEqual(c.drum.picks, ["drm1", "drm2", "drm3"], "three featured: only those");
  assert.deepEqual(c.fx.picks, ["fxa", "fxb", "fxonly1", "fxd"], "none featured: the first eight there are");
  for (const g of Object.values(c)) {
    assert.equal(g.source, "community");
    assert.ok(g.picks.length <= DEFAULT_PICKS);
  }
  assert.deepEqual([...slotIntents(c.synth)].filter(([, i]) => i.kind === "keep").map(([s]) => s), [9, 10]);
});

test("test plugs are never offered, and engines sort into their group", () => {
  const all = catalogue();
  assert.ok(!groupEngines(all, "synth").some((e) => e.dir === "opz_test_sine"));
  assert.ok(!defaultChoice(all).synth.picks.includes("opz_test_sine"));
  assert.equal(engineGroup(engine("x", "fx", 61099, { mask: 0x100 })), "fx");
  assert.equal(engineGroup(engine("y", "synth", 61098, { mask: 0x300 })), null, "a synth whose mask names only effect tracks");
  assert.deepEqual(engineTracks(engine("z", "fx", 61097, { mask: 0x100 }), group("fx")), [8]);
  assert.deepEqual(engineTracks(engine("w", "synth", 61096, { mask: 0 }), group("synth")), [4, 5, 6, 7]);
});

test("never more than ten per group", () => {
  let c = defaultChoice(catalogue()).synth;
  for (const dir of ["syn9", "syn10"]) {
    const r = togglePick(c, dir, true);
    assert.equal(r.refused, undefined);
    c = r.choice;
  }
  assert.equal(c.picks.length, 10);
  const r = togglePick(c, "syn11", true);
  assert.match(r.refused!, /10 sound slots/);
  assert.equal(r.choice, c, "a refused edit changes nothing");
  assert.deepEqual(togglePick(c, "syn1", false).choice.picks.length, 9);
  assert.equal(togglePick(c, "syn1", true).choice, c, "ticking a ticked engine is a no-op");
});

test("slots kept original or holding the visitor's samples lower the cap", () => {
  const c = { source: "community" as const, picks: ["a", "b", "c"], original: [2] };
  assert.deepEqual(freeSlots(c, [5]), [1, 3, 4, 6, 7, 8, 9, 10]);
  assert.equal(capacity(c, [5]), 8);
  const full = { ...c, picks: ["a", "b", "c", "d", "e", "f", "g", "h"] };
  assert.match(togglePick(full, "i", true, [5]).refused!, /8 slots are free/);
  const intents = slotIntents(full, [5]);
  assert.deepEqual(intents.get(1), { kind: "engine", dir: "a" });
  assert.deepEqual(intents.get(2), { kind: "original" });
  assert.deepEqual(intents.get(3), { kind: "engine", dir: "b" });
  assert.deepEqual(intents.get(5), { kind: "blocked" });
  assert.deepEqual(intents.get(6), { kind: "engine", dir: "d" });
  assert.deepEqual(intents.get(10), { kind: "engine", dir: "h" });
});

test("keeping a slot original: refused when the picks would no longer fit", () => {
  const ten = { source: "community" as const, picks: Array.from({ length: 10 }, (_, i) => `e${i}`), original: [] };
  assert.match(toggleOriginalSlot(ten, 3).refused!, /untick one first/);
  const eight = { ...ten, picks: ten.picks.slice(0, 8) };
  const r = toggleOriginalSlot(eight, 3);
  assert.deepEqual(r.choice.original, [3]);
  assert.deepEqual(toggleOriginalSlot(r.choice, 3).choice.original, [], "toggling again gives it back");
  assert.ok(toggleOriginalSlot(eight, 11).refused);
});

test("original for the whole group, and back to community with the picks intact", () => {
  const c = defaultChoice(catalogue()).synth;
  const o = setSource(c, "original");
  assert.ok([...slotIntents(o).values()].every((i) => i.kind === "original"));
  assert.deepEqual([...slotIntents(o, [4]).entries()].find(([s]) => s === 4)?.[1], { kind: "blocked" }, "samples are skipped in original too");
  assert.deepEqual(setSource(o, "community").picks, c.picks);
});

test("a stored choice is checked against the catalogue", () => {
  const all = catalogue();
  assert.deepEqual(sanitizeChoice(null, all), defaultChoice(all));
  const c = sanitizeChoice({
    synth: { source: "original", picks: ["syn1", "nope", "syn1", "drm1", 4], original: [3, 3, 0, 11, 2.5] },
    drum: { picks: Array.from({ length: 5 }, (_, i) => `drm${i + 1}`) },
  }, all);
  assert.deepEqual(c.synth, { source: "original", picks: ["syn1"], original: [3] });
  assert.equal(c.drum.source, "community");
  assert.equal(c.drum.picks.length, 5);
  assert.deepEqual(c.fx, defaultChoice(all).fx, "a missing group gets the default");
  const crowded = sanitizeChoice({ synth: { picks: groupEngines(all, "synth").map((e) => e.dir), original: [1, 2, 3] } }, all);
  assert.equal(crowded.synth.picks.length, 7, "capped by the free slots");
});

test("an engine with a caution is never ticked by default", () => {
  const all = [...catalogue(), engine("risky", "synth", 61099, { featured: true, caution: "Froze the OP-Z." })];
  const risky = all.at(-1)!;
  assert.equal(cautionOf(risky), "Froze the OP-Z.");
  assert.ok(!defaultChoice(all).synth.picks.includes("risky"));
  assert.ok(groupEngines(all, "synth").includes(risky), "still offered, to tick by hand (and to try in the demo)");
});
