/**
 * What the visitor picks, before any OP-Z is involved. No DOM, no device.
 *
 * Per track: each of the OP-Z's ten tracks (kick, snare, perc, sample, bass, lead, arp, chord, fx1, fx2) has ten sound
 * slots, the black keys 1…0 (manual p.25), and the visitor says for each key of each track what goes there: one of our
 * engines, the *original* sound, or nothing changed. Ticking an engine in a track's list puts it on that track's first
 * key left as it is; the key strip moves it anywhere. Every key starts as it is: nothing changes on the OP-Z until the
 * visitor picks a sound for a key. *Featured* fills a track with up to `FEATURED_PICKS` (8) featured engines on keys
 * 1–8, so keys 9 and 0 stay as they are, for the visitor's own samples and sounds.
 *
 * The boundary: synth and drum engines only on tracks 1–8, effects only on fx1 and fx2 (tracks 9 and 10), as the OP-Z
 * is laid out; within that, an engine goes only on the tracks of its kind (synths on bass–chord, drums on kick–sample)
 * that its trackmask names. Nothing here offers or keeps an engine on any other track, and plan.ts refuses one.
 *
 * *Original* means the plug that was in that cell before (teenage engineering's own sound, installed on every OP-Z).
 * We never upload or host TE's engines: a TE plug we displace stays installed, so putting it back is a slot-map change
 * only (src/loader/plan.ts).
 */

import type { CatalogueEngine } from "../shared/catalogue.ts";

/** Sound slots per track (the black keys 1…0). */
export const SLOTS_PER_TRACK = 10;
/** Featured engines *Featured* puts on a track: keys 9 and 0 stay as they are. */
export const FEATURED_PICKS = 8;
/** Our test plugs (opz_test_*): never offered. */
export const TEST_PLUG_MIN = 60900;
export const TEST_PLUG_MAX = 60999;

export type GroupId = "synth" | "drum" | "fx";

export interface Group {
  id: GroupId;
  title: string;
  /** Track indexes (the OP-Z's order: kick 0 … fx2 9). */
  tracks: number[];
  /** Trackmask bits of those tracks. */
  mask: number;
}

export const TRACK_LABELS = ["kick", "snare", "perc", "sample", "bass", "lead", "arp", "chord", "fx1", "fx2"] as const;
/** The tracks the loader writes: 0…9 (tape and master are left alone). */
export const TRACKS: readonly number[] = TRACK_LABELS.map((_, i) => i);

export const GROUPS: readonly Group[] = [
  { id: "drum", title: "Drums", tracks: [0, 1, 2, 3], mask: 0x0f },
  { id: "synth", title: "Synths", tracks: [4, 5, 6, 7], mask: 0xf0 },
  { id: "fx", title: "Effects", tracks: [8, 9], mask: 0x300 },
];

export function group(id: GroupId): Group {
  return GROUPS.find((g) => g.id === id)!;
}

/** The group a track belongs to. */
export function trackGroup(track: number): Group {
  return GROUPS.find((g) => g.tracks.includes(track))!;
}

/** What one key of one track gets: an engine, the original sound, or left as it is. */
export type KeyPick =
  | { kind: "engine"; dir: string }
  | { kind: "original" }
  | { kind: "keep" };

/** One track's ten keys (index 0 is key 1, index 9 is key 0). */
export type TrackChoice = KeyPick[];

/** Every track's keys, by track index 0…9. */
export type Choice = TrackChoice[];

/** Per track, the keys holding the visitor's own samples (known once connected): never given anything else. */
export type Blocked = (track: number) => readonly number[];
export const NOTHING_BLOCKED: Blocked = () => [];

const KEEP: KeyPick = { kind: "keep" };

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

/** The group of an engine's kind. */
export function kindGroup(e: CatalogueEngine): Group {
  return group(e.kind === "fx" ? "fx" : e.kind === "drum" ? "drum" : "synth");
}

/**
 * Whether an engine may go on a track: never a test plug; effects only on fx1/fx2 and nothing else there; only the
 * tracks of its kind; only the tracks its trackmask names (a mask of 0 names them all).
 */
export function canGoOn(e: CatalogueEngine, track: number): boolean {
  if (isTestPlug(e) || track < 0 || track >= TRACKS.length) return false;
  if ((e.kind === "fx") !== (trackGroup(track).id === "fx")) return false;
  if (!kindGroup(e).tracks.includes(track)) return false;
  const mask = e.manifest.plug.trackmask;
  return !mask || !!((mask >> track) & 1);
}

/** The tracks an engine may go on. */
export function engineTracks(e: CatalogueEngine): number[] {
  return TRACKS.filter((t) => canGoOn(e, t));
}

/** Why an engine can't go on a track, for the visitor. */
export function notForTrack(e: CatalogueEngine, track: number): string {
  const where = engineTracks(e).map((t) => TRACK_LABELS[t]);
  const what = e.kind === "fx" ? "an effect" : e.kind === "drum" ? "a drum engine" : "a synth";
  return `${e.label} is ${what}: it goes on ${where.length ? where.join(", ") : "no track"} only, not on ${TRACK_LABELS[track]}.`;
}

/** The engines offered for a track: featured first, then the rest, each in catalogue order. */
export function trackEngines(engines: readonly CatalogueEngine[], track: number): CatalogueEngine[] {
  const list = engines.filter((e) => canGoOn(e, track));
  return [...list.filter((e) => e.featured), ...list.filter((e) => !e.featured)];
}

/** The key (1…10) an engine is on in a track, or null. */
export function keyOf(t: TrackChoice, dir: string): number | null {
  const i = t.findIndex((k) => k.kind === "engine" && k.dir === dir);
  return i < 0 ? null : i + 1;
}

/** The engines on a track, in key order. */
export function trackPicks(t: TrackChoice): string[] {
  return t.flatMap((k) => (k.kind === "engine" ? [k.dir] : []));
}

/** Every engine on any track (catalogue order of first appearance by track, then key). */
export function pickedDirs(c: Choice): string[] {
  return [...new Set(c.flatMap(trackPicks))];
}

/**
 * *Featured* on a track: its featured engines (no caution), at most FEATURED_PICKS, on the first keys that are not
 * `blocked`; a catalogue that features none for the track gets its first FEATURED_PICKS engines. Other keys: as they are.
 */
export function featuredTrack(engines: readonly CatalogueEngine[], track: number, blocked: readonly number[] = []): TrackChoice {
  const list = trackEngines(engines, track).filter((e) => !cautionOf(e));
  const featured = list.filter((e) => e.featured);
  const dirs = (featured.length ? featured : list).slice(0, FEATURED_PICKS).map((e) => e.dir);
  const out: TrackChoice = Array.from({ length: SLOTS_PER_TRACK }, () => KEEP);
  let k = 0;
  for (const dir of dirs) {
    while (k < SLOTS_PER_TRACK && blocked.includes(k + 1)) k++;
    if (k >= SLOTS_PER_TRACK) break;
    out[k++] = { kind: "engine", dir };
  }
  return out;
}

/** The start: every key of every track as it is, so the visitor picks each sound that replaces one. */
export function defaultChoice(): Choice {
  return TRACKS.map(() => Array.from({ length: SLOTS_PER_TRACK }, () => KEEP));
}

/** *Featured* on every track (what the loader started with until 2026-11-05). */
export function featuredChoice(engines: readonly CatalogueEngine[], blocked: Blocked = NOTHING_BLOCKED): Choice {
  return TRACKS.map((t) => featuredTrack(engines, t, blocked(t)));
}

export interface Edit {
  choice: Choice;
  /** Why the edit was refused (the choice is then unchanged). */
  refused?: string;
}

function withTrack(c: Choice, track: number, t: TrackChoice): Choice {
  return c.map((x, i) => (i === track ? t : x));
}

const same = (a: KeyPick, b: KeyPick) => a.kind === b.kind && (a.kind !== "engine" || (b.kind === "engine" && a.dir === b.dir));

/**
 * Put `pick` on one key of one track. An engine already on another key of the track moves (that key is left as it
 * is); refused for a key holding the visitor's sample and for an engine that can't go on the track.
 */
export function setKey(c: Choice, engines: readonly CatalogueEngine[], track: number, key: number, pick: KeyPick, blocked: readonly number[] = []): Edit {
  if (track < 0 || track >= TRACKS.length) return { choice: c, refused: `tracks are 1…${TRACKS.length}` };
  if (key < 1 || key > SLOTS_PER_TRACK) return { choice: c, refused: `keys are 1…${SLOTS_PER_TRACK}` };
  if (blocked.includes(key)) return { choice: c, refused: `Key ${key % 10} on ${TRACK_LABELS[track]} holds your own sample: it is never changed.` };
  if (pick.kind === "engine") {
    const e = engines.find((x) => x.dir === pick.dir);
    if (!e) return { choice: c, refused: `${pick.dir} is not in the catalogue` };
    if (!canGoOn(e, track)) return { choice: c, refused: notForTrack(e, track) };
  }
  if (same(c[track][key - 1], pick)) return { choice: c };
  const t = c[track].map((k, i) => (i === key - 1 ? pick : pick.kind === "engine" && k.kind === "engine" && k.dir === pick.dir ? KEEP : k));
  return { choice: withTrack(c, track, t) };
}

/** Tick an engine on a track (its first key left as it is) or untick it (its key is then left as it is). */
export function tickEngine(c: Choice, engines: readonly CatalogueEngine[], track: number, dir: string, on: boolean, blocked: readonly number[] = []): Edit {
  const at = keyOf(c[track], dir);
  if (!on) return at === null ? { choice: c } : setKey(c, engines, track, at, KEEP, blocked);
  if (at !== null) return { choice: c };
  const free = c[track].findIndex((k, i) => k.kind === "keep" && !blocked.includes(i + 1));
  if (free < 0) {
    return { choice: c, refused: `Every key on ${TRACK_LABELS[track]} has a sound picked: set one to “leave as it is” (or untick an engine) first.` };
  }
  return setKey(c, engines, track, free + 1, { kind: "engine", dir }, blocked);
}

/** Every key of a track (but the visitor's samples) to the original sound. */
export function allOriginal(c: Choice, track: number, blocked: readonly number[] = []): Choice {
  return withTrack(c, track, c[track].map((k, i) => (blocked.includes(i + 1) ? k : { kind: "original" as const })));
}

/**
 * Copy one track's keys to another track of the same group: engines that can't go on the target, and keys holding the
 * visitor's samples there, are left as they are (`dropped` names them).
 */
export function copyTrack(c: Choice, engines: readonly CatalogueEngine[], from: number, to: number, blocked: readonly number[] = []): Edit & { dropped: string[] } {
  if (trackGroup(from) !== trackGroup(to)) {
    return { choice: c, dropped: [], refused: `${TRACK_LABELS[from]} and ${TRACK_LABELS[to]} are different kinds of track` };
  }
  const dropped: string[] = [];
  const t = c[from].map((k, i): KeyPick => {
    if (blocked.includes(i + 1)) {
      if (k.kind !== "keep") dropped.push(`key ${(i + 1) % 10} (your sample)`);
      return c[to][i];
    }
    if (k.kind !== "engine") return k;
    const e = engines.find((x) => x.dir === k.dir);
    if (e && canGoOn(e, to)) return k;
    dropped.push(e?.label ?? k.dir);
    return KEEP;
  });
  return { choice: withTrack(c, to, t), dropped };
}

/**
 * The lab's "add to my OP-Z" for one engine: on puts it on every track it can go on that doesn't have it (first key
 * left as it is), off takes it off every track. Refused when it fit on no track.
 */
export function pickEverywhere(c: Choice, engines: readonly CatalogueEngine[], dir: string, on: boolean, blocked: Blocked = NOTHING_BLOCKED): Edit {
  const e = engines.find((x) => x.dir === dir);
  if (!e) return { choice: c, refused: `${dir} is not in the catalogue` };
  let next = c;
  const full: string[] = [];
  for (const t of TRACKS) {
    if (on ? !canGoOn(e, t) : keyOf(next[t], dir) === null) continue;
    const edit = tickEngine(next, engines, t, dir, on, blocked(t));
    if (edit.refused) full.push(TRACK_LABELS[t]);
    else next = edit.choice;
  }
  if (on && full.length && full.length === engineTracks(e).length) {
    return { choice: c, refused: `Every key on ${full.join(", ")} has a sound picked: free one before adding ${e.label}.` };
  }
  return { choice: next };
}

function keyPick(raw: unknown, engines: readonly CatalogueEngine[], track: number): KeyPick {
  if (!raw || typeof raw !== "object") return KEEP;
  const r = raw as { kind?: unknown; dir?: unknown };
  if (r.kind === "original") return { kind: "original" };
  if (r.kind === "engine" && typeof r.dir === "string") {
    const e = engines.find((x) => x.dir === r.dir);
    if (e && canGoOn(e, track)) return { kind: "engine", dir: e.dir };
  }
  return KEEP;
}

/**
 * A stored choice checked against the catalogue: ten tracks of ten keys, engines unknown or not allowed on their track
 * left as they are, an engine twice on one track kept on its first key only. Anything else: the default.
 */
export function sanitizeChoice(raw: unknown, engines: readonly CatalogueEngine[]): Choice {
  if (!Array.isArray(raw) || raw.length !== TRACKS.length) return defaultChoice();
  return TRACKS.map((t) => {
    const keys = Array.isArray(raw[t]) ? raw[t] : [];
    const seen = new Set<string>();
    return Array.from({ length: SLOTS_PER_TRACK }, (_, i) => {
      const k = keyPick(keys[i], engines, t);
      if (k.kind !== "engine") return k;
      if (seen.has(k.dir)) return KEEP;
      seen.add(k.dir);
      return k;
    });
  });
}
