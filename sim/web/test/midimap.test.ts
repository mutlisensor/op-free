/** The OP-Z controller map of src/midimap.ts: channels → tracks, CCs → parameter bytes, MIDI learn. */

import assert from "node:assert/strict";
import { test } from "node:test";
import { CC_TO_PARAM, MidiRouter, PARAM_TO_CC, byteToCc, ccToByte, describe, dialCc } from "../src/midimap.ts";

const NAMES = ["P1", "P2", "attack", "decay", "sustain", "release", "FX1 send", "FX2 send", "filter", "resonance", "pan",
  "level", "portamento", "lfo depth", "lfo speed", "lfo target", "lfo shape", "note style"];

test("CC 1–18 are the OP-Z's parameters in its dial-page order (manual p.59–60)", () => {
  const order = Array.from({ length: 18 }, (_, i) => NAMES[CC_TO_PARAM[i + 1]]);
  assert.deepEqual(order, ["P1", "P2", "filter", "resonance", "attack", "decay", "sustain", "release", "lfo depth",
    "lfo speed", "lfo target", "lfo shape", "FX1 send", "FX2 send", "pan", "level", "portamento", "note style"]);
  // every byte has one CC, and the two tables are inverse
  assert.equal(new Set(Object.values(CC_TO_PARAM)).size, 18);
  for (const [cc, p] of Object.entries(CC_TO_PARAM)) assert.equal(PARAM_TO_CC[p], Number(cc));
  // the four dials of the four pages are CC 1–16
  assert.deepEqual([dialCc(0, 0), dialCc(0, 3), dialCc(1, 0), dialCc(3, 3)], [1, 4, 5, 16]);
});

test("7-bit values map onto bytes like the OS does: 0 → 0, 64 → 128, 127 → 255; out takes the top 7 bits", () => {
  assert.deepEqual([0, 1, 63, 64, 127].map(ccToByte), [0, 3, 127, 128, 255]);
  for (let v = 0; v < 128; v++) assert.equal(byteToCc(ccToByte(v)), v);
  assert.equal(byteToCc(255), 127);
});

test("notes go to the track on their channel (OP-Z default: track + 1); bend too", () => {
  const r = new MidiRouter();
  assert.deepEqual(r.handle([0x90, 36, 100]), [{ type: "note", track: 0, on: true, note: 36, velocity: 100, channel: 0 }]);
  assert.deepEqual(r.handle([0x95, 60, 90]), [{ type: "note", track: 5, on: true, note: 60, velocity: 90, channel: 5 }]);
  // note on with velocity 0 and note off are both offs
  assert.equal((r.handle([0x95, 60, 0])[0] as { on: boolean }).on, false);
  assert.equal((r.handle([0x85, 60, 64])[0] as { on: boolean }).on, false);
  assert.deepEqual(r.handle([0xe6, 0x7f, 0x7f]), [{ type: "bend", track: 6, cents: 200, channel: 6 }]);
  assert.equal((r.handle([0xe6, 0, 0x40])[0] as { cents: number }).cents, 0);
  // FX1/FX2 are channels 9 and 10
  assert.equal((r.handle([0x98, 60, 90])[0] as { track: number }).track, 8);
  assert.equal((r.handle([0x99, 60, 90])[0] as { track: number }).track, 9);
  // tape, master … (channels 11–16) drive nothing here
  assert.equal(r.handle([0x9c, 60, 90])[0].type, "unmapped");
  // clock and other system messages are ignored
  assert.deepEqual(r.handle([0xf8]), []);
  assert.deepEqual(r.handle([0xf0, 0x00, 0x20, 0x76, 0xf7]), []);
});

test("a dial on the OP-Z moves that track's byte: lead page 1 dial 1 = CC 1 on channel 6 = P1", () => {
  const r = new MidiRouter();
  assert.deepEqual(r.handle([0xb5, 1, 127]), [{ type: "param", track: 5, param: 0, byte: 255, channel: 5, cc: 1, learned: false }]);
  // kick, page 4 dial 4 (level)
  assert.deepEqual(r.handle([0xb0, 16, 64])[0], { type: "param", track: 0, param: 11, byte: 128, channel: 0, cc: 16, learned: false });
  // FX1's P2
  assert.equal((r.handle([0xb8, 2, 10])[0] as { track: number }).track, 8);
  assert.equal(r.handle([0xb5, 74, 10])[0].type, "unmapped");
});

test("a channel map change follows the OP-Z's own setting; omni sends every note to the selected track", () => {
  const r = new MidiRouter();
  r.channels[5] = 0;                    // lead set to channel 1 while kick is still on it: the first track wins
  assert.equal(r.trackFor(0), 0);
  r.channels[0] = 15;                   // kick moved away: channel 1 drives lead
  assert.equal((r.handle([0x90, 60, 1])[0] as { track: number }).track, 5);
  r.omni = true;
  r.selected = 7;
  assert.equal((r.handle([0x93, 60, 1])[0] as { track: number }).track, 7);
  // CCs keep following the channel map in omni
  assert.equal((r.handle([0xb0, 1, 1])[0] as { track: number }).track, 5);
});

test("MIDI learn: the next CC binds to the armed knob and wins over the default map", () => {
  const r = new MidiRouter();
  r.arm({ track: 6, param: 8 });
  const first = r.handle([0xb2, 74, 100]);
  assert.deepEqual(first[0], { type: "learned", track: 6, param: 8, channel: 2, cc: 74 });
  assert.deepEqual(first[1], { type: "param", track: 6, param: 8, byte: 200, channel: 2, cc: 74, learned: true });
  assert.equal(r.armedBinding, null);
  assert.deepEqual(r.learnedFor(6, 8), { channel: 2, cc: 74 });
  // the learned CC keeps working, on its own channel only
  assert.equal((r.handle([0xb2, 74, 0])[0] as { byte: number }).byte, 0);
  assert.equal(r.handle([0xb3, 74, 0])[0].type, "unmapped");
  // learn over a default CC: ch 6 CC 1 now drives arp's filter, not lead's P1
  r.arm({ track: 6, param: 8 });
  r.handle([0xb5, 1, 5]);
  assert.deepEqual(r.learnedFor(6, 8), { channel: 5, cc: 1 });
  assert.equal(r.learnedCount, 1);
  assert.deepEqual(r.handle([0xb5, 1, 5])[0], { type: "param", track: 6, param: 8, byte: 11, channel: 5, cc: 1, learned: true });
  r.forget(6, 8);
  assert.equal((r.handle([0xb5, 1, 5])[0] as { track: number }).track, 5);
});

test("save/load round-trips and drops anything malformed", () => {
  const r = new MidiRouter();
  r.channels[2] = 11;
  r.omni = true;
  r.arm({ track: 1, param: 0 });
  r.handle([0xb0, 20, 1]);
  const saved = JSON.parse(JSON.stringify(r.save()));
  const r2 = new MidiRouter();
  r2.load(saved);
  assert.deepEqual(r2.save(), r.save());
  const r3 = new MidiRouter();
  r3.load({ channels: [1, 2], omni: "yes", learned: [{ channel: 99, cc: 1, track: 0, param: 0 }, null] });
  assert.deepEqual(r3.save(), new MidiRouter().save());
  r3.load(null);
  r3.load("junk");
  assert.deepEqual(r3.save(), new MidiRouter().save());
});

test("the monitor's text", () => {
  assert.equal(describe([0xb5, 1, 127]), "ch 6 CC 1 = 127");
  assert.equal(describe([0x90, 60, 0]), "ch 1 note off 60");
  assert.equal(describe([0xe0, 0, 0x40]), "ch 1 pitch bend 0");
  assert.equal(describe([0xf8]), "f8");
});
