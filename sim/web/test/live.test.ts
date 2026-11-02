/**
 * The live path (LiveEngine: what the keyboard and knobs drive) is the scenario path: replaying a scenario's notes,
 * preset and parameters through ws_note_on/ws_param block by block gives the same samples as opzsim_main rendering
 * the `.scn`. One engine per kind (synth, drum, FX) and the three SDK templates. Skips without the wasm build.
 */

import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { BLOCK, type InputKind, LiveEngine, SAMPLE_RATE, TRACKS, runScenario, wavSamples } from "../src/opzsim.ts";

const REPO = join(import.meta.dirname, "../../..");
const WASM = join(REPO, "sim/build/wasm");
const skip = existsSync(WASM) ? false : "no sim/build/wasm: run make -C sim wasm";

const PARAMS = ["p1", "p2", "attack", "decay", "sustain", "release", "fx1", "fx2", "filter", "resonance", "pan", "level",
  "portamento", "lfo_depth", "lfo_speed", "lfo_target", "lfo_shape", "note_style"];

function noteNumber(s: string): number {
  if (/^\d+$/.test(s)) return Number(s);
  const m = /^([A-Ga-g])([#b]?)(-?\d+)$/.exec(s)!;
  const base = { A: 9, B: 11, C: 0, D: 2, E: 4, F: 5, G: 7 }[m[1].toUpperCase() as "A"]!;
  return base + (m[2] === "#" ? 1 : m[2] === "b" ? -1 : 0) + (Number(m[3]) + 1) * 12;
}

interface Ev { time: number; order: number; seq: number; run: (e: LiveEngine) => void }

/** Replay the subset of the scenario format these tests use (no sweeps, packs or transport) through LiveEngine. */
function replay(module: WebAssembly.Module, text: string, defaultTrack: number): Int32Array {
  let tempo = 120, steps = 16, tail = 1, track = defaultTrack;
  for (const l of text.split("\n")) {
    const w = l.replace(/#.*/, "").trim().split(/\s+/);
    if (w[0] === "tempo") tempo = Number(w[1]);
  }
  const stepSamples = SAMPLE_RATE * 15 / tempo;
  const at = (step: number) => Math.floor(step * stepSamples + 0.5);
  const init: Array<(e: LiveEngine) => void> = [];
  const events: Ev[] = [];
  let input: [InputKind, number, number] | null = null;
  let gain = 1;
  for (const l of text.split("\n")) {
    const w = l.replace(/#.*/, "").trim().split(/\s+/);
    const t = track;
    if (w[0] === "steps") steps = Number(w[1]);
    else if (w[0] === "tail") tail = Number(w[1]);
    else if (w[0] === "track") track = TRACKS.indexOf(w[1] as never);
    else if (w[0] === "preset") w.slice(1).forEach((v, i) => init.push((e) => e.param(i, Number(v), true, t)));
    else if (w[0] === "param") {
      const index = PARAMS.indexOf(w[1]), step = Number(w[3] ?? 0);
      if (step === 0) init.push((e) => e.param(index, Number(w[2]), true, t));
      else events.push({ time: at(step), order: 1, seq: events.length, run: (e) => e.param(index, Number(w[2]), false, t) });
    } else if (w[0] === "note") {
      const note = noteNumber(w[2]), step = Number(w[1]), vel = Number(w[4] ?? 100);
      events.push({ time: at(step), order: 1, seq: events.length, run: (e) => e.noteOn(note, vel, t) });
      events.push({ time: at(step + Number(w[3])), order: 0, seq: events.length, run: (e) => e.noteOff(note, t) });
    } else if (w[0] === "input") input = [w[1] as InputKind, Number(w[2] ?? 0), Number(w[w[1] === "noise" ? 2 : 3] ?? 0)];
    else if (w[0] === "gain") gain = Number(w[1]);
  }
  events.sort((a, b) => a.time - b.time || a.order - b.order || a.seq - b.seq);
  const live = new LiveEngine(module, track);
  assert.ok(live.ok, live.log);
  live.tempo(tempo);
  for (const f of init) f(live);
  if (input) live.input(input[0], input[0] === "noise" ? 0 : input[1], 0, input[0] === "noise" ? input[1] : input[2]);
  live.gain(gain);
  const frames = Math.ceil(Math.floor(steps * stepSamples + tail * SAMPLE_RATE + 0.5) / BLOCK) * BLOCK;
  const out = new Int32Array(frames * 2);
  let ev = 0;
  for (let t0 = 0; t0 < frames; t0 += BLOCK) {
    for (; ev < events.length && events[ev].time <= t0; ev++) events[ev].run(live);
    out.set(live.render(), t0 * 2);
  }
  return out;
}

function compare(wasm: string, name: string, scenario: string, defaultTrack: number): void {
  const module = new WebAssembly.Module(readFileSync(join(WASM, wasm)));
  const text = readFileSync(join(REPO, scenario), "utf8");
  const r = runScenario(module, { name, scenario: "/" + scenario, files: new Map([["/" + scenario, new TextEncoder().encode(text)]]) });
  assert.ok(r.exitCode === 0 || r.exitCode === 3, r.log);
  const want = wavSamples(r.wav!);
  const got = replay(module, text, defaultTrack);
  assert.equal(got.length, want.length);
  const first = got.findIndex((v, i) => v !== want[i]);
  assert.equal(first, -1, `first difference at sample ${first >> 1} (${got[first]} vs ${want[first]})`);
  assert.ok(want.some((v) => v !== 0), "silent render");
}

const LEAD = 5, FX1 = 8;
test("live = scenario: synth template", { skip }, () => compare("templates/synth.wasm", "template_synth", "sdk/templates/synth/tests/basic.scn", LEAD));
test("live = scenario: drum template", { skip }, () => compare("templates/drum.wasm", "template_drum", "sdk/templates/drum/tests/keys.scn", LEAD));
test("live = scenario: fm2 (chord track, voice steal)", { skip }, () => compare("engines/fm2.wasm", "fm2", "engines/fm2/tests/epiano.scn", LEAD));
test("live = scenario: kit (drum track)", { skip }, () => compare("engines/kit.wasm", "kit", "engines/kit/tests/steal.scn", LEAD));
test("live = scenario: swirl (FX1, noise input)", { skip }, () => compare("engines/swirl.wasm", "swirl", "engines/swirl/tests/phaser.scn", FX1));
test("live = scenario: filter (FX1, notes into the effect)", { skip }, () => compare("engines/filter.wasm", "filter", "engines/filter/tests/notes.scn", FX1));
