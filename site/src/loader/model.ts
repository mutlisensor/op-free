/**
 * What the visitor picks, before any OP-Z is involved. No DOM, no device.
 *
 * Three track groups: the synth tracks (bass, lead, arp, chord), the drum tracks (kick, snare, perc, sample) and the
 * send effects (fx1, fx2). Each OP-Z track has ten sound slots (the black keys 1…0, manual p.25). A group's picks fill
 * slot numbers 1, 2, 3… in order, the same slot on every track of the group the engine is made for, so one engine
 * sits on the same black key everywhere. `DEFAULT_PICKS` (8) are ticked so two slots per track stay as they are, for
 * the visitor's own samples and sounds; never more than ten.
 *
 * Community or original, per group or per slot: *community* puts our engines in; *original* means the plug that was
 * in that slot before (teenage engineering's own sound, installed on every OP-Z). We never upload or host TE's
 * engines: a TE plug we displace stays installed, so putting it back is a slot-map change only (src/loader/plan.ts).
 */

import type { CatalogueEngine } from "../shared/catalogue.ts";

/** Sound slots per track (the black keys 1…0). */
export const SLOTS_PER_TRACK = 10;
/** Ticked by default per group: two slots per track stay free. */
export const DEFAULT_PICKS = 8;
/** Never more than the slots a track has. */
export const MAX_PICKS = SLOTS_PER_TRACK;
/** Our test plugs (opz_test_*): never offered. */
export const TEST_PLUG_MIN = 60900;
export const TEST_PLUG_MAX = 60999;

export type GroupId = "synth" | "drum" | "fx";
export type Source = "community" | "original";

export interface Group {
  id: GroupId;
  title: string;
  /** Track indexes (the OP-Z's order: kick 0 … fx2 9). */
  tracks: number[];
  /** Trackmask bits of those tracks. */
  mask: number;
}

export const TRACK_LABELS = ["kick", "snare", "perc", "sample", "bass", "lead", "arp", "chord", "fx1", "fx2"] as const;

export const GROUPS: readonly Group[] = [
  { id: "synth", title: "Synths", tracks: [4, 5, 6, 7], mask: 0xf0 },
  { id: "drum", title: "Drums", tracks: [0, 1, 2, 3], mask: 0x0f },
  { id: "fx", title: "Effects", tracks: [8, 9], mask: 0x300 },
];

export function group(id: GroupId): Group {
  return GROUPS.find((g) => g.id === id)!;
}

/** One group's choice: community or original, the ticked engines in slot order, slots kept original in community. */
export interface GroupChoice {
  source: Source;
  /** Engine folder names, in the order they fill the slots. */
  picks: string[];
  /** Slot numbers (1…10) that stay original while the group is community. */
  original: number[];
}

export type Choice = Record<GroupId, GroupChoice>;

export function isTestPlug(e: CatalogueEngine): boolean {
  const id = e.manifest.plug.id;
  return id >= TEST_PLUG_MIN && id <= TEST_PLUG_MAX;
}

/**
 * Why an engine must not go on a real OP-Z yet (it crashed or froze one), or null: the catalogue's `caution`. Read
 * through a cast so the loader builds with and without that field in src/shared/catalogue.ts.
 */
export function cautionOf(e: CatalogueEngine): string | null {
  return (e as CatalogueEngine & { caution?: string | null }).caution ?? null;
}

/** The group an engine belongs to: its kind, if its trackmask names a track of that group (or names none). */
export function engineGroup(e: CatalogueEngine): GroupId | null {
  const id: GroupId = e.kind === "fx" ? "fx" : e.kind === "drum" ? "drum" : "synth";
  const mask = e.manifest.plug.trackmask;
  return !mask || mask & group(id).mask ? id : null;
}

/** The tracks of `g` an engine goes on: the ones its trackmask names (all of them for a mask of 0). */
export function engineTracks(e: CatalogueEngine, g: Group): number[] {
  const mask = e.manifest.plug.trackmask;
  return g.tracks.filter((t) => !mask || (mask >> t) & 1);
}

/** The engines offered for a group: featured first, then the rest, each in catalogue order. No test plugs. */
export function groupEngines(engines: readonly CatalogueEngine[], id: GroupId): CatalogueEngine[] {
  const list = engines.filter((e) => !isTestPlug(e) && engineGroup(e) === id);
  return [...list.filter((e) => e.featured), ...list.filter((e) => !e.featured)];
}

/**
 * The default: per group the catalogue's featured engines, at most DEFAULT_PICKS. A catalogue that features none in a
 * group gets its first DEFAULT_PICKS engines there. Engines with a caution are never ticked by default.
 */
export function defaultChoice(engines: readonly CatalogueEngine[]): Choice {
  const out = {} as Choice;
  for (const g of GROUPS) {
    const list = groupEngines(engines, g.id).filter((e) => !cautionOf(e));
    const featured = list.filter((e) => e.featured);
    out[g.id] = { source: "community", picks: (featured.length ? featured : list).slice(0, DEFAULT_PICKS).map((e) => e.dir), original: [] };
  }
  return out;
}

/**
 * Slot numbers the picks can fill: 1…10 without the slots kept original and the `blocked` ones (slots that hold the
 * visitor's own samples on every track of the group: never touched).
 */
export function freeSlots(c: GroupChoice, blocked: readonly number[] = []): number[] {
  const out: number[] = [];
  for (let s = 1; s <= SLOTS_PER_TRACK; s++) if (!c.original.includes(s) && !blocked.includes(s)) out.push(s);
  return out;
}

/** How many engines the group can take now (≤ MAX_PICKS). */
export function capacity(c: GroupChoice, blocked: readonly number[] = []): number {
  return Math.min(MAX_PICKS, freeSlots(c, blocked).length);
}

export interface Edit {
  choice: GroupChoice;
  /** Why the edit was refused (the choice is then unchanged). */
  refused?: string;
}

/** Tick or untick an engine. Ticking past the group's capacity is refused. */
export function togglePick(c: GroupChoice, dir: string, on: boolean, blocked: readonly number[] = []): Edit {
  const has = c.picks.includes(dir);
  if (on === has) return { choice: c };
  if (!on) return { choice: { ...c, picks: c.picks.filter((d) => d !== dir) } };
  const cap = capacity(c, blocked);
  if (c.picks.length >= cap) {
    return {
      choice: c,
      refused: cap === MAX_PICKS
        ? `An OP-Z track has ${MAX_PICKS} sound slots: untick one first.`
        : `${cap} slot${cap === 1 ? " is" : "s are"} free for engines here (the others stay original or hold your samples): untick one first.`,
    };
  }
  return { choice: { ...c, picks: [...c.picks, dir] } };
}

/** Community or original for the whole group. Original keeps the picks, so switching back restores them. */
export function setSource(c: GroupChoice, source: Source): GroupChoice {
  return { ...c, source };
}

/** Keep one slot original (or give it back to the picks) while the group is community. */
export function toggleOriginalSlot(c: GroupChoice, slot: number, blocked: readonly number[] = []): Edit {
  if (slot < 1 || slot > SLOTS_PER_TRACK) return { choice: c, refused: `slots are 1…${SLOTS_PER_TRACK}` };
  if (c.original.includes(slot)) return { choice: { ...c, original: c.original.filter((s) => s !== slot) } };
  const next = { ...c, original: [...c.original, slot].sort((a, b) => a - b) };
  if (c.picks.length > capacity(next, blocked)) {
    return { choice: c, refused: "Every other slot is taken by a ticked engine: untick one first." };
  }
  return { choice: next };
}

/** What each slot of a group gets: an engine, the original sound, left as it is, or skipped (your samples). */
export type SlotIntent =
  | { kind: "engine"; dir: string }
  | { kind: "original" }
  | { kind: "keep" }
  | { kind: "blocked" };

export function slotIntents(c: GroupChoice, blocked: readonly number[] = []): Map<number, SlotIntent> {
  const out = new Map<number, SlotIntent>();
  const free = freeSlots(c, blocked);
  for (let s = 1; s <= SLOTS_PER_TRACK; s++) {
    if (blocked.includes(s)) out.set(s, { kind: "blocked" });
    else if (c.source === "original" || c.original.includes(s)) out.set(s, { kind: "original" });
    else {
      const i = free.indexOf(s);
      out.set(s, i >= 0 && i < c.picks.length ? { kind: "engine", dir: c.picks[i] } : { kind: "keep" });
    }
  }
  return out;
}

/** A stored choice checked against the catalogue: unknown engines and bad slots dropped, the cap kept. */
export function sanitizeChoice(raw: unknown, engines: readonly CatalogueEngine[]): Choice {
  const fallback = defaultChoice(engines);
  if (!raw || typeof raw !== "object") return fallback;
  const out = {} as Choice;
  for (const g of GROUPS) {
    const r = (raw as Record<string, Partial<GroupChoice> | undefined>)[g.id];
    if (!r || typeof r !== "object") {
      out[g.id] = fallback[g.id];
      continue;
    }
    const known = new Set(groupEngines(engines, g.id).map((e) => e.dir));
    const original = [...new Set((Array.isArray(r.original) ? r.original : []).filter((s) => Number.isInteger(s) && s >= 1 && s <= SLOTS_PER_TRACK))]
      .sort((a, b) => a - b);
    const base: GroupChoice = { source: r.source === "original" ? "original" : "community", picks: [], original };
    const picks = [...new Set((Array.isArray(r.picks) ? r.picks : []).filter((d): d is string => typeof d === "string" && known.has(d)))];
    out[g.id] = { ...base, picks: picks.slice(0, capacity(base)) };
  }
  return out;
}
