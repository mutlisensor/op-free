/**
 * Room on the OP-Z, and taking every plug of ours off it (pure). Port of `space_report`, `describe_space` and
 * `plan_remove_ours` in tools/opz.py (same numbers, same lines). Why it matters: docs/roadmap/restore-space.md.
 *
 * The file service lists no file sizes and no free space, so what we can know over MIDI comes from the two settings
 * files: the plug definitions (the OS keeps at most 200, FUN_800ff9e8 "plug count full" [code]) and which engine files
 * are ours (sizes from our own builds). Content mode's own limits (the 24 MB of samplepacks/user/, the 33 MB disk) are
 * read from the content-mode disk itself (web/app src/disk.ts).
 */

import {
  type Doc, TE_ENGINES, TRACK_NAMES, USER_PLUG_MIN, assign, isUserSample, orphanEngines, orphanErrors, removalBlock,
  unassign, unassignedUserSamples,
} from "./plan.ts";
import { clone } from "./pyjson.ts";

/** Test plugs 60900–60999, released from 61000, our sample plugs 61400–61499 (sdk/docs/memory-slots.md). */
export const OUR_PLUG_MIN = 60900;
/** 60000–60899 are other people's user plugs; we never touch them. */
export const OUR_PLUG_MAX = 61999;
/** Plug definitions the OS keeps in RAM (FUN_800ff9e8: "plug count full" past 200) [code]. */
export const PLUG_LIMIT = 200;
/** Content mode's sample memory: the files in samplepacks/user/ only (FUN_8003377c, 0x1800000) [code]. */
export const SAMPLE_LIMIT = 24 * 1024 * 1024;

/** Free plug definitions below which our tools warn before an install or restore, and below which they call it full. */
export const ROOM_LOW = 60;
export const ROOM_TIGHT = 30;

export function roomLevel(free: number): "ok" | "low" | "full" {
  return free < ROOM_TIGHT ? "full" : free < ROOM_LOW ? "low" : "ok";
}

/**
 * The warning when an install leaves too few plug definitions for a content-mode restore of the user's own samples
 * (each needs one; a full table drops the sample silently, docs/roadmap/restore-space.md), or null. Port of
 * `install_room` in tools/opz.py.
 */
export function installRoom(plugs: Doc, plugId: number): string | null {
  const every: Doc[] = plugs.plugs ?? [];
  const free = PLUG_LIMIT - every.length - (every.some((p) => p.id === plugId) ? 0 : 1);
  const level = roomLevel(free);
  if (level === "ok") return null;
  return `room: ${Math.max(free, 0)} of ${PLUG_LIMIT} plug definitions free after this install; a content-mode restore of `
    + `your own samples needs one per sample (${level === "full" ? "too few for a typical backup" : `fewer than ${ROOM_LOW} left`}). `
    + "opz.py remove --all-ours frees them again";
}

/** A plug this project made: id in our range and not one of the user's samples (their ids are crc32 of the path). */
export function isOurPlug(plug: Doc): boolean {
  const id = Number(plug.id);
  return id >= OUR_PLUG_MIN && id <= OUR_PLUG_MAX && !isUserSample(plug);
}

export interface SpaceReport {
  plugs: number;
  limit: number;
  free: number;
  ours: number[];
  user_samples: number;
  te: number;
  our_engine_files: string[];
  our_engine_bytes: number;
  unsized: number;
  displaced_te: number[];
  loose_user_samples: number[];
}

function engineName(path: string): string {
  return String(path.split("/").at(-1)).replaceAll(".engine", "");
}

/**
 * What the OP-Z holds that a restore needs room for, from the two settings files. `engineSizes` maps an engine path
 * (`engines/<name>.engine`) to its size in bytes, for the engines whose build we have.
 */
export function spaceReport(plugs: Doc, slots: Doc, engineSizes: ReadonlyMap<string, number> | Record<string, number> = {}): SpaceReport {
  const every: Doc[] = plugs.plugs ?? [];
  const ours = every.filter(isOurPlug);
  const user = every.filter(isUserSample);
  // engines our plugs use and orphans (left by removals), except TE's own (our sample plugs play on TE's)
  const used = new Set<string>([...ours.map((p) => String(p.engine)), ...orphanEngines(plugs)]);
  const ourEngines = (plugs.engines ?? []).map((e: Doc) => String(e.path))
    .filter((p: string) => used.has(p) && !TE_ENGINES.has(engineName(p))).sort();
  const size = (p: string): number | undefined => engineSizes instanceof Map ? engineSizes.get(p)
    : (engineSizes as Record<string, number>)[p];
  const known = ourEngines.map(size).filter((n: number | undefined): n is number => n !== undefined);
  const inSlots = new Set<number>((slots.tracks ?? []).flatMap((t: Doc) => (t.packs ?? []).map((p: Doc) => p.id)));
  return {
    plugs: every.length, limit: PLUG_LIMIT, free: PLUG_LIMIT - every.length,
    ours: ours.map((p) => p.id), user_samples: user.length, te: every.length - ours.length - user.length,
    our_engine_files: ourEngines, our_engine_bytes: known.reduce((a: number, b: number) => a + b, 0), unsized: ourEngines.length - known.length,
    displaced_te: every.filter((p) => p.id < USER_PLUG_MIN && !inSlots.has(p.id)).map((p): number => p.id).sort((a: number, b: number) => a - b),
    loose_user_samples: unassignedUserSamples(plugs, slots).map((p) => p.id),
  };
}

/** The lines `opz.py space` prints. */
export function describeSpace(r: SpaceReport): string {
  const lines = [`plug definitions: ${r.plugs} of ${r.limit} (${r.free} free): ${r.ours.length} of ours, `
    + `${r.user_samples} user samples, ${r.te} TE`];
  const size = `${Math.ceil(r.our_engine_bytes / 1024)} KB` + (r.unsized ? ` + ${r.unsized} not built here` : "");
  lines.push(`our engine files: ${r.our_engine_files.length} (${size}), on the OP-Z's 512 MB flash; content mode shows none of them`);
  if (r.displaced_te.length) {
    lines.push("TE plugs in no slot (displaced; a restore or remove --all-ours --put-back brings them back): "
      + r.displaced_te.join(", "));
  }
  if (r.loose_user_samples.length) {
    lines.push("WARNING: user samples in no slot (content mode deletes them): " + r.loose_user_samples.join(", "));
  }
  return lines.join("\n");
}

export interface RemoveOursPlan {
  /** The plugs that go, in plugs.json order. */
  ids: number[];
  /** Plugs of ours that stay, with the reason (their removal would delete a file we did not write). */
  kept: Array<[number, string]>;
  /** [track, slot, plug] for every cell one of them sits in. */
  cleared: Array<[number, number, number]>;
  /** [track, slot, plug] put back from the earlier slot map. */
  refilled: Array<[number, number, number]>;
  slot_map: Doc;
  errors: string[];
}

/**
 * Every plug of ours off the OP-Z: which plugs go, which cells empty, and (with `putBack`, the slot map of an earlier
 * backup) which installed plug that isn't ours goes back into a cell we empty.
 */
export function planRemoveOurs(plugs: Doc, slots: Doc, putBack: Doc | null = null): RemoveOursPlan {
  const ids: number[] = [];
  const kept: Array<[number, string]> = [];
  for (const p of plugs.plugs ?? []) {
    if (!isOurPlug(p)) continue;
    const why = removalBlock(p, plugs);
    if (why !== null) kept.push([p.id, why]);
    else ids.push(p.id);
  }
  const gone = new Set(ids);
  let doc: Doc = clone(slots);
  const cleared: Array<[number, number, number]> = [];
  for (const t of doc.tracks ?? []) for (const q of t.packs ?? []) if (gone.has(q.id)) cleared.push([t.index, q.slot, q.id]);
  for (const pid of ids) doc = unassign(doc, pid);
  const refilled: Array<[number, number, number]> = [];
  if (putBack !== null) {
    const installed = new Set<number>((plugs.plugs ?? []).map((p: Doc) => p.id));
    const old = new Map<string, number>();
    for (const t of putBack.tracks ?? []) for (const q of t.packs ?? []) old.set(`${t.index}:${q.slot}`, q.id);
    for (const [t, s] of cleared) {
      const q = old.get(`${t}:${s}`);
      if (q === undefined || gone.has(q) || !installed.has(q)) continue;
      doc = assign(doc, t, s, q);
      refilled.push([t, s, q]);
    }
  }
  return { ids, kept, cleared, refilled, slot_map: doc, errors: orphanErrors(plugs, slots, doc) };
}

/** The plan lines `opz.py remove --all-ours` logs before it asks. */
export function removeOursLines(plan: RemoveOursPlan, bootPatch: string): string[] {
  const lines = [`plan: remove ${plan.ids.length} plug${plan.ids.length > 1 ? "s" : ""} of ours: ${plan.ids.join(", ")}`];
  const refilled = new Set(plan.refilled.map(([t, s]) => `${t}:${s}`));
  const emptied = plan.cleared.filter(([t, s]) => !refilled.has(`${t}:${s}`)).map(([t, s]) => `${TRACK_NAMES[t] ?? t} ${s}`);
  if (emptied.length) lines.push(`plan: empty ${emptied.join(", ")}`);
  if (plan.refilled.length) {
    lines.push("plan: put back " + plan.refilled.map(([t, s, q]) => `${TRACK_NAMES[t] ?? t} ${s} = ${q}`).join(", "));
  }
  lines.push(`plan: empty boot file ${bootPatch}; the engine files stay (no known delete)`);
  return lines;
}
