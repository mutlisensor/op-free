/// <reference types="node" />
/** The picking model: per track, what goes on each black key; the default eight; effects only on fx1/fx2. */

import assert from "node:assert/strict";
import { test } from "node:test";
import {
  FEATURED_PICKS, TRACKS, allOriginal, canGoOn, cautionOf, copyTrack, defaultChoice, engineTracks, featuredChoice, featuredTrack, keyOf, pickEverywhere,
  pickedDirs, sanitizeChoice, setKey, tickEngine, trackEngines, trackPicks,
} from "../model.ts";
import { catalogue, engine } from "./fixtures.ts";

const all = catalogue();
const SYNTH = [4, 5, 6, 7];
const DRUM = [0, 1, 2, 3];

test("the start: every key of every track left as it is, nothing picked", () => {
  const c = defaultChoice();
  assert.equal(c.length, 10);
  for (const t of TRACKS) assert.deepEqual(c[t], Array(10).fill({ kind: "keep" }));
  assert.deepEqual(pickedDirs(c), []);
  assert.equal(keyOf(tickEngine(c, all, 4, "syn5", true).choice[4], "syn5"), 1, "the first pick goes on key 1");
});

test("Featured: per track its featured engines on keys 1–8, keys 9 and 0 as they are", () => {
  assert.equal(FEATURED_PICKS, 8);
  const c = featuredChoice(all);
  assert.equal(c.length, 10);
  for (const t of SYNTH) assert.deepEqual(trackPicks(c[t]), ["syn1", "syn2", "syn3", "syn4", "syn5", "syn6", "syn7", "syn8"], "nine featured: the first eight");
  for (const t of DRUM) assert.deepEqual(trackPicks(c[t]), ["drm1", "drm2", "drm3"], "three featured: only those");
  assert.deepEqual(trackPicks(c[8]), ["fxa", "fxb", "fxonly1", "fxd"], "none featured: the first eight there are");
  assert.deepEqual(trackPicks(c[9]), ["fxa", "fxb", "fxd"], "fxonly1 is made for fx1 only");
  for (const t of TRACKS) {
    assert.deepEqual(c[t][8], { kind: "keep" });
    assert.deepEqual(c[t][9], { kind: "keep" });
  }
  assert.deepEqual(featuredTrack(all, 3, [2]).slice(0, 4).map((k) => k.kind), ["engine", "keep", "engine", "engine"], "past the visitor's sample on key 2");
});

test("effects only on fx1 and fx2; synths and drums only on their own tracks among 1–8", () => {
  const wideSynth = engine("wide", "synth", 61090, { mask: 0x3ff });
  const wideFx = engine("widefx", "fx", 61091, { mask: 0x3ff });
  const lost = engine("lost", "synth", 61092, { mask: 0x300 });
  assert.deepEqual(engineTracks(wideSynth), SYNTH, "a trackmask naming every track still puts a synth on bass–chord only");
  assert.deepEqual(engineTracks(wideFx), [8, 9], "an effect never goes on tracks 1–8");
  assert.deepEqual(engineTracks(lost), [], "a synth whose mask names only effect tracks goes nowhere");
  assert.deepEqual(engineTracks(engine("d", "drum", 61093, { mask: 0 })), DRUM);
  assert.deepEqual(engineTracks(all.find((e) => e.dir === "fxonly1")!), [8]);
  for (const t of TRACKS) {
    for (const e of trackEngines(all, t)) assert.equal(e.kind === "fx", t >= 8, `${e.dir} offered on track ${t}`);
  }
  const c = featuredChoice(all);
  assert.match(setKey(c, all, 5, 9, { kind: "engine", dir: "fxa" }).refused!, /an effect: it goes on fx1, fx2 only, not on lead/);
  assert.match(setKey(c, all, 8, 9, { kind: "engine", dir: "syn1" }).refused!, /a synth: it goes on bass, lead, arp, chord only/);
  assert.match(setKey(c, all, 0, 9, { kind: "engine", dir: "syn1" }).refused!, /not on kick/);
  assert.match(tickEngine(c, all, 9, "fxonly1", true).refused!, /fx1 only|fx1 only, not on fx2/);
});

test("test plugs are never offered", () => {
  const sine = all.find((e) => e.dir === "opz_test_sine")!;
  assert.ok(!canGoOn(sine, 4));
  assert.ok(!pickedDirs(featuredChoice(all)).includes("opz_test_sine"));
});

test("each track picks its own sounds, key by key", () => {
  let c = featuredChoice(all);
  // bass: syn10 on key 3, lead untouched
  c = setKey(c, all, 4, 3, { kind: "engine", dir: "syn10" }).choice;
  assert.deepEqual(c[4][2], { kind: "engine", dir: "syn10" });
  assert.equal(keyOf(c[5], "syn3"), 3, "the other synth tracks keep their own keys");
  assert.equal(keyOf(c[4], "syn3"), null, "syn3 left bass");
  // an engine already on the track moves to the key it is given
  c = setKey(c, all, 4, 10, { kind: "engine", dir: "syn1" }).choice;
  assert.equal(keyOf(c[4], "syn1"), 10);
  assert.deepEqual(c[4][0], { kind: "keep" }, "its old key is left as it is");
  // original and keep per key
  c = setKey(c, all, 4, 2, { kind: "original" }).choice;
  assert.deepEqual(c[4][1], { kind: "original" });
  assert.equal(setKey(c, all, 4, 2, { kind: "original" }).choice, c, "no change, same choice");
  assert.match(setKey(c, all, 4, 11, { kind: "keep" }).refused!, /keys are 1…10/);
});

test("ticking fills the first key left as it is; never more than ten per track", () => {
  let c = featuredChoice(all);
  let r = tickEngine(c, all, 6, "syn9", true);
  assert.equal(keyOf(r.choice[6], "syn9"), 9);
  assert.equal(keyOf(r.choice[7], "syn9"), null, "on arp only");
  c = tickEngine(r.choice, all, 6, "syn10", true).choice;
  r = tickEngine(c, all, 6, "syn11", true);
  assert.match(r.refused!, /Every key on arp has a sound picked/);
  assert.equal(r.choice, c, "a refused edit changes nothing");
  c = tickEngine(c, all, 6, "syn2", false).choice;
  assert.deepEqual(c[6][1], { kind: "keep" });
  assert.equal(keyOf(tickEngine(c, all, 6, "syn11", true).choice[6], "syn11"), 2, "the freed key");
});

test("keys holding the visitor's samples are never given anything", () => {
  const c = featuredChoice(all);
  assert.match(setKey(c, all, 3, 4, { kind: "engine", dir: "drm4" }, [4]).refused!, /holds your own sample/);
  assert.match(setKey(c, all, 3, 4, { kind: "original" }, [4]).refused!, /never changed/);
  const t = tickEngine(c, all, 3, "drm4", true, [4]).choice;
  assert.equal(keyOf(t[3], "drm4"), 5, "ticking skips key 4");
  const o = allOriginal(c, 3, [4, 9]);
  assert.deepEqual(o[3].map((k) => k.kind), ["original", "original", "original", "keep", "original", "original", "original", "original", "keep", "original"]);
});

test("copy a track to another of its kind", () => {
  let c = featuredChoice(all);
  c = setKey(c, all, 4, 9, { kind: "engine", dir: "syn12" }).choice;
  const r = copyTrack(c, all, 4, 5, [9]);
  assert.deepEqual(r.choice[5].slice(0, 8), c[4].slice(0, 8));
  assert.deepEqual(r.choice[5][8], c[5][8], "lead's key 9 holds a sample there: kept");
  assert.deepEqual(r.dropped, ["key 9 (your sample)"]);
  const fx = copyTrack(c, all, 8, 9);
  assert.equal(keyOf(fx.choice[9], "fxonly1"), null, "made for fx1 only");
  assert.deepEqual(fx.dropped, ["fxonly1"]);
  assert.match(copyTrack(c, all, 4, 0).refused!, /different kinds of track/);
});

test("the lab's tick: on every track the engine is made for, or off all of them", () => {
  let c = featuredChoice(all);
  c = pickEverywhere(c, all, "syn9", true).choice;
  for (const t of SYNTH) assert.equal(keyOf(c[t], "syn9"), 9);
  for (const t of [...DRUM, 8, 9]) assert.equal(keyOf(c[t], "syn9"), null);
  c = pickEverywhere(c, all, "syn1", false).choice;
  for (const t of SYNTH) assert.equal(keyOf(c[t], "syn1"), null);
  assert.ok(!pickedDirs(c).includes("syn1"));
  // every synth track full: refused
  let full = featuredChoice(all);
  for (const d of ["syn9", "syn10"]) full = pickEverywhere(full, all, d, true).choice;
  assert.match(pickEverywhere(full, all, "syn11", true).refused!, /Every key on bass, lead, arp, chord/);
});

test("a stored choice is checked against the catalogue and the track boundary", () => {
  assert.deepEqual(sanitizeChoice(null, all), defaultChoice());
  assert.deepEqual(sanitizeChoice({ synth: { picks: ["syn1"] } }, all), defaultChoice(), "the old per-group format: the default");
  const raw = featuredChoice(all).map((t) => t.map((k) => ({ ...k }))) as unknown[][];
  raw[4][8] = { kind: "engine", dir: "nope" };
  raw[4][9] = { kind: "engine", dir: "syn1" };
  raw[0][8] = { kind: "engine", dir: "syn1" };
  raw[5][8] = { kind: "engine", dir: "fxa" };
  raw[9][8] = { kind: "engine", dir: "fxonly1" };
  raw[6] = [{ kind: "original" }, "junk"];
  const c = sanitizeChoice(raw, all);
  assert.deepEqual(c[4][8], { kind: "keep" }, "unknown engine");
  assert.deepEqual(c[4][9], { kind: "keep" }, "syn1 twice on bass: its first key only");
  assert.deepEqual(c[0][8], { kind: "keep" }, "a synth on kick");
  assert.deepEqual(c[5][8], { kind: "keep" }, "an effect on lead");
  assert.deepEqual(c[9][8], { kind: "keep" }, "fx1-only engine on fx2");
  assert.deepEqual(c[6].map((k) => k.kind), ["original", ...Array(9).fill("keep")]);
});

test("an engine with a caution is never put anywhere by Featured", () => {
  const withRisky = [...all, engine("risky", "synth", 61099, { featured: true, caution: "Froze the OP-Z." })];
  const risky = withRisky.at(-1)!;
  assert.equal(cautionOf(risky), "Froze the OP-Z.");
  assert.ok(!pickedDirs(featuredChoice(withRisky)).includes("risky"));
  assert.ok(trackEngines(withRisky, 4).includes(risky), "still offered, to pick by hand (and to try in the demo)");
});
