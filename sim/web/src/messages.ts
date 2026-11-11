/** Messages between the page and the AudioWorklet (src/worklet.ts). */

import type { LiveStats } from "./opzsim.ts";

/** The eight instrument tracks (OP-Z tracks 1–8), each its own engine slot. */
export const INST_SLOTS = ["kick", "snare", "perc", "sample", "bass", "lead", "arp", "chord"] as const;
export type InstSlot = (typeof INST_SLOTS)[number];
/** The two effect slots, on FX1 and FX2. */
export type FxSlot = "fx1" | "fx2";
export const FX_SLOTS: FxSlot[] = ["fx1", "fx2"];
export type SlotName = InstSlot | FxSlot;
export const ALL_SLOTS: SlotName[] = [...INST_SLOTS, ...FX_SLOTS];

/** The OP-Z track of a slot: 0–7 the instruments, 8 FX1, 9 FX2. */
export function trackOf(slot: SlotName): number {
  return slot === "fx1" ? 8 : slot === "fx2" ? 9 : INST_SLOTS.indexOf(slot);
}

export function slotOf(track: number): SlotName | null {
  return track === 8 ? "fx1" : track === 9 ? "fx2" : INST_SLOTS[track] ?? null;
}

export function isInst(slot: SlotName): slot is InstSlot {
  return slot !== "fx1" && slot !== "fx2";
}

export type ToWorklet =
  /** (Re)open one slot on its track; bytes null empties it. Each slot is its own module instance with its own host. */
  | { type: "load"; slot: SlotName; bytes: ArrayBuffer | null; track: number }
  /** Keys: to an instrument track, or to an effect's onNoteOn/onNoteOff. */
  | { type: "note"; slot: SlotName; on: boolean; note: number; velocity: number }
  | { type: "param"; slot: SlotName; index: number; value: number; immediate: boolean }
  /** Pitch bend on one track (the OP-Z sends it on the track's channel). */
  | { type: "bend"; slot: SlotName; cents: number }
  /** An effect's input: "send" (the tracks through their FX sends) or a generator (saw, sine, noise, sweep, impulse, silence). */
  | { type: "input"; slot: FxSlot; kind: string; f0: number; f1: number; amp: number }
  | { type: "gain"; slot: FxSlot; gain: number }
  | { type: "tempo"; bpm: number }
  | { type: "transport"; play: boolean }
  | { type: "panic" };

export interface TrackMix {
  /** this track into FX1 and FX2, and its dry path (0-1) */
  sends: [number, number];
  dry: number;
}

export type FromWorklet =
  | { type: "loaded"; slot: SlotName; ok: boolean; empty: boolean; isFx: boolean; log: string; fineClock: boolean }
  | {
    /** host counters since each slot loaded (null: empty slot); busyMs and blocks: time in process() and blocks rendered */
    type: "stats"; slots: Partial<Record<SlotName, LiveStats | null>>; busyMs: number; blocks: number;
    log: string;
    /** each instrument track's gains in use */
    mix: Partial<Record<InstSlot, TrackMix>>;
    /** each slot's loudest sample since the last report (0-1, linear) */
    peaks: Partial<Record<SlotName, number>>;
  };
