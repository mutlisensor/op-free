/**
 * The OP-Z as a MIDI controller for the page: which track a channel drives and which parameter byte a CC moves.
 * No DOM, no Web MIDI: `MidiRouter.handle` turns one MIDI message into actions the page applies (tests feed it bytes).
 *
 * What the OP-Z sends (docs/research/opz-as-controller.md has the evidence):
 * - Each of its 16 tracks has a MIDI channel; the default is track + 1 (kick 1 … lead 6 … FX1 9, FX2 10)
 *   [proven for MIDI in, H-23; out uses the same per-track channel table: code, app `Midi::AddParameterCC`].
 * - Notes from its keys and its sequencer go out on each track's channel, so a playing pattern drives every track.
 * - Turning a dial sends a 7-bit CC on that track's channel, once per sequencer tick while the byte changes, for 16 of
 *   the 18 parameters (not portamento or note style) [code, app `Sequencer::tick` → `Midi::AddParameterCC`]. The
 *   parameters are taken in the order of its four dial pages (P1 P2 filter resonance · attack decay sustain release ·
 *   LFO depth speed target shape · FX1 FX2 pan level) [code: the app's parameter → CC-slot table]; the CC number of
 *   each slot comes from the device's MIDI config, most likely CC 1–16 like its incoming map [inferred; Q-37].
 * - Incoming, the OP-Z reads CC 1–18 on a track's channel as those parameters (17 portamento, 18 note style)
 *   [code, app `IncomingCCToParameter`; manual p.59–60], so the same map works for any controller set up like it.
 * - Switching tracks on the OP-Z sends nothing [code, app: no CC or other message from the track keys].
 *
 * Bytes here are opzsim's parameter order (sim/README.md): P1, P2, attack, decay, sustain, release, FX1 send, FX2 send,
 * filter, resonance, pan, level, portamento, LFO depth, speed, target, shape, note style.
 */

/** The tracks the page plays: 0–7 instruments, 8 FX1, 9 FX2 (OP-Z track order). */
export const ROUTED_TRACKS = 10;
export const TRACK_NAMES = ["kick", "snare", "perc", "sample", "bass", "lead", "arp", "chord", "FX1", "FX2"];

/** CC number → opzsim byte index: CC 1–16 in the OP-Z's dial-page order, then 17 portamento and 18 note style. */
export const CC_TO_PARAM: Readonly<Record<number, number>> = {
  1: 0, 2: 1, 3: 8, 4: 9,          // page 1: P1, P2, filter, resonance
  5: 2, 6: 3, 7: 4, 8: 5,          // page 2: attack, decay, sustain, release
  9: 13, 10: 14, 11: 15, 12: 16,   // page 3: LFO depth, speed, target, shape
  13: 6, 14: 7, 15: 10, 16: 11,    // page 4: FX1 send, FX2 send, pan, level
  17: 12, 18: 17,                  // not sent by the OP-Z; read by it on input
};

export const PARAM_TO_CC: Readonly<Record<number, number>> =
  Object.fromEntries(Object.entries(CC_TO_PARAM).map(([cc, p]) => [p, Number(cc)]));

/** The CC the OP-Z sends for dial `dial` (0–3) of page `page` (0–3). */
export function dialCc(page: number, dial: number): number {
  return page * 4 + dial + 1;
}

/** The OS's 7-bit → byte step for a CC (value v → 2v + (v & 1), so 0 → 0 and 127 → 255) [code, app `Sequencer::cc`]. */
export function ccToByte(value: number): number {
  const v = value & 0x7f;
  return 2 * v + (v & 1);
}

/** What the OP-Z sends for a byte: its top 7 bits. */
export function byteToCc(byte: number): number {
  return (byte >> 1) & 0x7f;
}

export interface Binding {
  track: number;
  param: number;
}

export type MidiAction =
  | { type: "note"; track: number; on: boolean; note: number; velocity: number; channel: number }
  | { type: "param"; track: number; param: number; byte: number; channel: number; cc: number; learned: boolean }
  | { type: "bend"; track: number; cents: number; channel: number }
  /** A CC was bound to a knob in learn mode (a `param` action follows it). */
  | { type: "learned"; track: number; param: number; channel: number; cc: number }
  /** A message the router has no target for: shown in the monitor so a learn or a channel fix can follow. */
  | { type: "unmapped"; channel: number; status: number; data1: number; data2: number; why: string };

export interface RouterState {
  channels: number[];
  omni: boolean;
  learned: Array<{ channel: number; cc: number; track: number; param: number }>;
}

export class MidiRouter {
  /** channels[track] = the 0-based MIDI channel that drives it (the OP-Z's default: track). */
  channels: number[] = Array.from({ length: ROUTED_TRACKS }, (_, t) => t);
  /** Every channel plays the selected track (a plain keyboard on one channel); CCs still follow the channel map. */
  omni = false;
  /** The track the page has selected (omni's target). */
  selected = 5;
  private learned = new Map<string, Binding>();
  private armed: Binding | null = null;

  /** The track a channel drives, or null. */
  trackFor(channel: number): number | null {
    const t = this.channels.indexOf(channel);
    return t < 0 ? null : t;
  }

  /** The next CC binds to this knob (null to cancel). */
  arm(binding: Binding | null): void {
    this.armed = binding;
  }

  get armedBinding(): Binding | null {
    return this.armed;
  }

  /** The learned CC of a knob, if any. */
  learnedFor(track: number, param: number): { channel: number; cc: number } | null {
    for (const [key, b] of this.learned) {
      if (b.track === track && b.param === param) {
        const [channel, cc] = key.split(":").map(Number);
        return { channel, cc };
      }
    }
    return null;
  }

  forget(track?: number, param?: number): void {
    for (const [key, b] of this.learned) {
      if ((track === undefined || b.track === track) && (param === undefined || b.param === param)) this.learned.delete(key);
    }
  }

  get learnedCount(): number {
    return this.learned.size;
  }

  handle(data: ArrayLike<number> | null | undefined): MidiAction[] {
    if (!data || data.length < 1) return [];
    const status = data[0] & 0xf0, channel = data[0] & 0x0f;
    if (status < 0x80 || status === 0xf0) return [];             // running status never reaches Web MIDI; system: ignored
    const d1 = (data[1] ?? 0) & 0x7f, d2 = (data[2] ?? 0) & 0x7f;
    const unmapped = (why: string): MidiAction[] => [{ type: "unmapped", channel, status, data1: d1, data2: d2, why }];
    const noteTrack = this.omni ? this.selected : this.trackFor(channel);
    switch (status) {
      case 0x90:
      case 0x80: {
        if (noteTrack === null) return unmapped("no track on this channel");
        const on = status === 0x90 && d2 > 0;
        return [{ type: "note", track: noteTrack, on, note: d1, velocity: on ? d2 : 0, channel }];
      }
      case 0xe0: {
        if (noteTrack === null) return unmapped("no track on this channel");
        return [{ type: "bend", track: noteTrack, cents: Math.round(((((d2 << 7) | d1) - 8192) / 8192) * 200), channel }];
      }
      case 0xb0:
        return this.cc(channel, d1, d2);
      default:
        return unmapped("not used by the page");
    }
  }

  private cc(channel: number, cc: number, value: number): MidiAction[] {
    const key = `${channel}:${cc}`;
    const out: MidiAction[] = [];
    if (this.armed) {
      this.forget(this.armed.track, this.armed.param);
      this.learned.set(key, this.armed);
      out.push({ type: "learned", ...this.armed, channel, cc });
      this.armed = null;
    }
    const bound = this.learned.get(key);
    if (bound) return [...out, { type: "param", ...bound, byte: ccToByte(value), channel, cc, learned: true }];
    const track = this.trackFor(channel);
    const param = CC_TO_PARAM[cc];
    if (track === null) return [{ type: "unmapped", channel, status: 0xb0, data1: cc, data2: value, why: "no track on this channel" }];
    if (param === undefined) return [{ type: "unmapped", channel, status: 0xb0, data1: cc, data2: value, why: "not an OP-Z parameter CC" }];
    return [{ type: "param", track, param, byte: ccToByte(value), channel, cc, learned: false }];
  }

  save(): RouterState {
    return {
      channels: this.channels.slice(),
      omni: this.omni,
      learned: [...this.learned].map(([key, b]) => {
        const [channel, cc] = key.split(":").map(Number);
        return { channel, cc, ...b };
      }),
    };
  }

  /** Restore a saved state; anything malformed is dropped (it comes from the viewer's browser storage). */
  load(state: unknown): void {
    const s = state as Partial<RouterState> | null;
    if (!s || typeof s !== "object") return;
    const okByte = (x: unknown, max: number) => Number.isInteger(x) && (x as number) >= 0 && (x as number) <= max;
    if (Array.isArray(s.channels) && s.channels.length === ROUTED_TRACKS && s.channels.every((c) => okByte(c, 15))) {
      this.channels = s.channels.slice();
    }
    if (typeof s.omni === "boolean") this.omni = s.omni;
    if (Array.isArray(s.learned)) {
      this.learned.clear();
      for (const l of s.learned) {
        if (l && okByte(l.channel, 15) && okByte(l.cc, 127) && okByte(l.track, ROUTED_TRACKS - 1) && okByte(l.param, 17)) {
          this.learned.set(`${l.channel}:${l.cc}`, { track: l.track, param: l.param });
        }
      }
    }
  }
}

/** One MIDI message as the monitor shows it. */
export function describe(data: ArrayLike<number>): string {
  const status = data[0] & 0xf0, ch = (data[0] & 0x0f) + 1;
  const d1 = data[1] ?? 0, d2 = data[2] ?? 0;
  switch (status) {
    case 0x90: return d2 ? `ch ${ch} note on ${d1} vel ${d2}` : `ch ${ch} note off ${d1}`;
    case 0x80: return `ch ${ch} note off ${d1}`;
    case 0xb0: return `ch ${ch} CC ${d1} = ${d2}`;
    case 0xe0: return `ch ${ch} pitch bend ${((d2 << 7) | d1) - 8192}`;
    case 0xc0: return `ch ${ch} program ${d1}`;
    default: return [...Array.from(data)].map((b) => b.toString(16).padStart(2, "0")).join(" ");
  }
}
