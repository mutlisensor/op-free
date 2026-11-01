/**
 * Ported from web/app/src/samplegate.ts; the refusal texts point at the loader's steps instead of the configurator's views.
 *
 * The user-sample check before content mode (SAFETY.md rule 5, docs/research/samples.md). When the OP-Z leaves
 * content mode it drops every user-sample plug (flag 2: made on the OP-Z, by TE's app or through content mode) that
 * sits in no sound slot, then deletes every file in samplepacks/user/ that no plug names. So before the user enters
 * content mode for a bake, the app reads plugs.json and the slot map over Web MIDI and refuses to go on while any user
 * sample is in no slot (web/core's unassignedUserSamples, the port of opz_samples.unassigned_user_samples). Without a
 * MIDI connection it cannot know, so the user must confirm that every sample is in a slot.
 *
 * No DOM: the bake card shows the state, main.ts and the Backups view ask whether a disk flow may start.
 */

import { type Doc, unassignedUserSamples, isUserSample } from "./core.ts";

/** A read of the OP-Z over Web MIDI (not the demo), as the check saw it. */
export interface SampleCheck {
  /** ISO time of the read. */
  at: string;
  /** User samples in no slot: what content mode would delete. */
  loose: Array<{ id: number; samples: string }>;
  /** User samples on the OP-Z. */
  total: number;
}

/** A passed check counts this long once MIDI is gone (the user went into content mode after it). */
export const CHECK_VALID_MS = 30 * 60 * 1000;

export function sampleCheck(device: { plugs: Doc; slots: Doc; at?: string }, now = new Date()): SampleCheck {
  return {
    at: device.at ?? now.toISOString(),
    loose: unassignedUserSamples(device.plugs, device.slots).map((p) => ({ id: Number(p.id), samples: String(p.samples ?? "") })),
    total: (device.plugs.plugs ?? []).filter(isUserSample).length,
  };
}

export type SampleGate =
  /** Connected over MIDI and a user sample is in no slot: no disk flow until it is back in a slot. */
  | { kind: "blocked"; check: SampleCheck }
  /** Connected over MIDI, or a passed check from the last half hour: every user sample is in a slot. */
  | { kind: "checked"; check: SampleCheck; live: boolean }
  /** Connected over MIDI, but the status has not been read yet. */
  | { kind: "needs-read" }
  /** Not connected (or only the demo): the user must confirm; `last` is the last check, if any (stale or blocked). */
  | { kind: "needs-confirm"; last: SampleCheck | null }
  /** Not connected, and the user confirmed that every sample is in a slot. */
  | { kind: "confirmed"; last: SampleCheck | null };

export interface GateInput {
  /** Connected to an OP-Z over Web MIDI (the demo does not count). */
  midi: boolean;
  /** What the controller read from it, if anything. */
  device: { plugs: Doc; slots: Doc; at?: string } | null;
  /** The last check over Web MIDI in this page. */
  last: SampleCheck | null;
  /** The user ticked "every sample I made on the OP-Z is in a sound slot". */
  confirmed: boolean;
  now?: Date;
}

export function sampleGate(i: GateInput): SampleGate {
  const now = i.now ?? new Date();
  if (i.midi) {
    if (!i.device) return { kind: "needs-read" };
    const check = sampleCheck(i.device, now);
    return check.loose.length ? { kind: "blocked", check } : { kind: "checked", check, live: true };
  }
  const last = i.last;
  if (last && !last.loose.length && now.getTime() - Date.parse(last.at) <= CHECK_VALID_MS) return { kind: "checked", check: last, live: false };
  return i.confirmed ? { kind: "confirmed", last } : { kind: "needs-confirm", last };
}

/** A disk flow (bake, look, put back) may start. */
export function gateOpen(g: SampleGate): boolean {
  return g.kind === "checked" || g.kind === "confirmed";
}

/** One line per loose sample: "plug 1513459487 (samplepacks/user/….aif)". */
export function looseText(check: SampleCheck): string[] {
  return check.loose.map((p) => `plug ${p.id}${p.samples ? ` (${p.samples})` : ""}`);
}

/** Why a disk flow may not start, for the refusal sheet; null when it may. */
export function gateRefusal(g: SampleGate): string | null {
  switch (g.kind) {
    case "blocked":
      return `Content mode would delete ${g.check.loose.length === 1 ? "a sample" : `${g.check.loose.length} samples`} you made on the OP-Z: `
        + `${looseText(g.check).join(", ")} ${g.check.loose.length === 1 ? "is" : "are"} in no sound slot. Put each one back in a slot first `
        + "(step 5's Restore puts back the slot map of a backup that had it in a slot), then check again.";
    case "needs-read":
      return "Read the OP-Z's status first: the app checks that every sample you made on it is in a sound slot before content mode.";
    case "needs-confirm":
      return "Confirm that every sample you made on the OP-Z is in a sound slot, or connect over USB in normal mode and let the "
        + "page check: content mode deletes user samples that are in no slot.";
    default:
      return null;
  }
}
