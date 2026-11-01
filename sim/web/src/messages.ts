/** Messages between the page and the AudioWorklet (src/worklet.ts). */

import type { LiveStats } from "./opzsim.ts";

/** The synth slot (an instrument on tracks 1-8) and the two effect slots, on FX1 and FX2. */
export type SlotName = "synth" | "fx1" | "fx2";
export type FxSlot = "fx1" | "fx2";
export const FX_SLOTS: FxSlot[] = ["fx1", "fx2"];

export type ToWorklet =
  /** (Re)open one slot; bytes null empties it. Each slot is its own module instance with its own host. */
  | { type: "load"; slot: SlotName; bytes: ArrayBuffer | null; track: number }
  /** Keys: to the synth, or to an effect's onNoteOn/onNoteOff. */
  | { type: "note"; slot: SlotName; on: boolean; note: number; velocity: number }
  | { type: "param"; slot: SlotName; index: number; value: number; immediate: boolean }
  | { type: "bend"; cents: number }
  /** An effect's input: "send" (the synth through its FX send) or a generator (saw, sine, noise, sweep, impulse, silence). */
  | { type: "input"; slot: FxSlot; kind: string; f0: number; f1: number; amp: number }
  | { type: "gain"; slot: FxSlot; gain: number }
  | { type: "tempo"; bpm: number }
  | { type: "transport"; play: boolean }
  | { type: "panic" };

export type FromWorklet =
  | { type: "loaded"; slot: SlotName; ok: boolean; empty: boolean; isFx: boolean; log: string; fineClock: boolean }
  | {
    /** host counters since each slot loaded; busyMs and blocks: time in process() and blocks rendered */
    type: "stats"; synth: LiveStats | null; fx1: LiveStats | null; fx2: LiveStats | null; busyMs: number; blocks: number;
    log: string;
    /** the gains in use (0-1): the synth into FX1 and FX2, and its dry path */
    sends: [number, number]; dry: number;
  };
