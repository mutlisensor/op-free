/** Messages between the lab and its AudioWorklet (worklet.ts). Trimmed from sim/web/src/messages.ts. */

/** The synth slot (an instrument on tracks 1-8) and the two effect slots, on FX1 and FX2. */
export type SlotName = "synth" | "fx1" | "fx2";
export type FxSlot = "fx1" | "fx2";
export const FX_SLOTS: FxSlot[] = ["fx1", "fx2"];

export type ToWorklet =
  /** (Re)open one slot; bytes null empties it. Each slot is its own module instance with its own host. */
  | { type: "load"; slot: SlotName; bytes: ArrayBuffer | null; track: number }
  | { type: "note"; slot: SlotName; on: boolean; note: number; velocity: number }
  | { type: "param"; slot: SlotName; index: number; value: number; immediate: boolean }
  | { type: "bend"; cents: number }
  | { type: "tempo"; bpm: number }
  | { type: "panic" };

export type FromWorklet =
  | { type: "loaded"; slot: SlotName; ok: boolean; empty: boolean; isFx: boolean; log: string }
  | { type: "error"; log: string };
