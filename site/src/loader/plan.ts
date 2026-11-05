/**
 * The picks (src/loader/model.ts) against a real (or demo) OP-Z: which slot gets which plug, which engines need an
 * install, and the slot map to write. No DOM. The checks are the core's own (`planInstall`, `slotMapProblems`), so the
 * confirm sheet shows what the core is about to accept, not a second opinion.
 *
 * Rules, cell by cell (a cell is one track's slot):
 * - a cell holding one of the visitor's own samples (flag 2) is never touched (SAFETY.md rule 5);
 * - an engine goes on the key the visitor gave it on that track, and only on a track it may go on (model.ts `canGoOn`:
 *   effects only on fx1/fx2, synths and drums only on their own tracks among 1–8, as their trackmask names); anything
 *   else is an error, so a stored or hand-made choice can't cross that line either;
 * - *original* puts back the plug the first backup of this OP-Z had in that cell (TE's own sound; it stays installed
 *   when we displace it, so this is a slot-map change only);
 * - a slot the picks leave alone keeps what it has, except one of our catalogue's plugs that is no longer picked
 *   there: that cell goes back to its original too.
 */

import type { CatalogueEngine } from "../shared/catalogue.ts";
import {
  type Doc, type Engine, TRACK_NAMES, isUserSample, makeEngine, planInstall, pyInt, slotCells, slotMapProblems, unassignedUserSamples,
} from "./core.ts";
import { type Choice, SLOTS_PER_TRACK, TRACKS, TRACK_LABELS, canGoOn, cautionOf, notForTrack } from "./model.ts";

export interface Device {
  plugs: Doc;
  slots: Doc;
}

export type CellWhy =
  /** a picked engine goes here */
  | "engine"
  /** the original plug goes (back) here */
  | "original"
  /** one of our plugs that is no longer picked here: back to the original */
  | "revert"
  /** left as it is */
  | "keep"
  /** the visitor's own sample: never touched */
  | "user-sample"
  /** the original plug is no longer installed: left as it is */
  | "original-gone";

export interface Cell {
  track: number;
  slot: number;
  /** Plug id now, null for an empty slot. */
  from: number | null;
  to: number | null;
  why: CellWhy;
  /** The engine (folder name) the slot was picked for. */
  dir?: string;
}

export interface InstallItem {
  dir: string;
  label: string;
  plugId: number;
  version: number;
  /** install: not on the OP-Z; upgrade: an older version is; ready: this version is (presets pushed again). */
  action: "install" | "upgrade" | "ready";
  /** The engine was registered before: the OP-Z keeps the old code until it restarts. */
  restart: boolean;
  presets: number;
  /** The catalogue has the built `.engine`. */
  built: boolean;
  bytes: number | null;
}

export interface LoaderPlan {
  /** Every cell of tracks 1–10, track then slot order. */
  cells: Cell[];
  /** The cells that change. */
  changes: Cell[];
  /** The slot map to write. */
  target: Doc;
  /** The engines placed somewhere, in pick order. */
  installs: InstallItem[];
  /** Any error: write nothing. */
  errors: string[];
  warnings: string[];
  /** Per track (index 0…9), the keys holding the visitor's samples: never changed. */
  blocked: number[][];
  /** Cells an engine or original would have taken but that hold the visitor's own sample. */
  skipped: Cell[];
}

export interface PlanInput {
  engines: readonly CatalogueEngine[];
  choice: Choice;
  device: Device;
  /** The slot map of the first backup this browser took of this OP-Z: what "original" puts back (null: unknown). */
  original: Doc | null;
  /** Demo mode: an engine that is not built installs a placeholder (the demo OP-Z never runs it). */
  demo: boolean;
}

const key = (t: number, s: number) => `${t}:${s}`;

/** "t:s" → plug id for every cell holding one of the visitor's own samples. */
export function userSampleCells(d: Device): Map<string, number> {
  const mine = new Set((d.plugs.plugs ?? []).filter(isUserSample).map((p: Doc) => p.id));
  return new Map([...slotCells(d.slots)].filter(([, id]) => mine.has(id)));
}

/** The keys of a track that hold one of the visitor's samples. */
export function blockedKeys(track: number, samples: Map<string, number>): number[] {
  const out: number[] = [];
  for (let s = 1; s <= SLOTS_PER_TRACK; s++) if (samples.has(key(track, s))) out.push(s);
  return out;
}

/** The catalogue's presets as presets.json has them (`default_octave`), for the core. */
export function presetDocs(e: CatalogueEngine): Doc[] {
  return e.presets.map(({ octave, ...p }) => ({ ...p, default_octave: (p as Doc).default_octave ?? octave ?? 0 }));
}

/** The core's Engine for a catalogue entry (`data`: the built `.engine`, or a placeholder in the demo). */
export function toEngine(e: CatalogueEngine, data: Uint8Array): Engine {
  return makeEngine(e.manifest, presetDocs(e), data, e.dir);
}

/** What to call a plug: our engine's label, else the engine or sample file name from plugs.json. */
export function plugLabel(plugs: Doc | null, id: number | null, engines: readonly CatalogueEngine[] = []): string {
  if (id === null) return "empty";
  const ours = engines.find((e) => e.manifest.plug.id === id);
  if (ours) return ours.label;
  const record = (plugs?.plugs ?? []).find((p: Doc) => p.id === id);
  if (!record) return `plug ${id}`;
  const file = (path: string) => path.split("/").at(-1)!.replace(/\.(engine|aif|aiff|wav)$/i, "");
  if (isUserSample(record)) return `your sample ${record.samples ? file(String(record.samples)) : id}`;
  return record.samples ? file(String(record.samples)) : file(String(record.engine ?? id));
}

/** A slot map with these cells (other tracks, e.g. past master, copied from `base`). */
export function cellsToDoc(cells: Map<string, number>, base: Doc): Doc {
  const tracks = new Map<number, Doc[]>();
  for (const t of base.tracks ?? []) tracks.set(t.index, []);
  for (const [k, id] of cells) {
    const [t, s] = k.split(":").map(Number);
    if (!tracks.has(t)) tracks.set(t, []);
    tracks.get(t)!.push({ id, slot: s });
  }
  return {
    ...base,
    tracks: [...tracks].sort(([a], [b]) => a - b).map(([index, packs]) => ({ index, packs: packs.sort((x, y) => x.slot - y.slot) })),
  };
}

export function planLoader(input: PlanInput): LoaderPlan {
  const { engines, choice, device } = input;
  const errors: string[] = [];
  const warnings: string[] = [];
  const now = slotCells(device.slots);
  const orig = input.original ? slotCells(input.original) : null;
  const installed = new Set<number>((device.plugs.plugs ?? []).map((p: Doc) => p.id));
  const samples = userSampleCells(device);
  const ours = new Set(engines.map((e) => e.manifest.plug.id));
  const byDir = new Map(engines.map((e) => [e.dir, e]));
  const cells: Cell[] = [];
  const skipped: Cell[] = [];
  const blocked: number[][] = [];
  const placed: string[] = [];

  for (const t of TRACKS) {
    blocked[t] = blockedKeys(t, samples);
    const keys = choice[t] ?? [];
    for (let s = 1; s <= SLOTS_PER_TRACK; s++) {
      const from = now.get(key(t, s)) ?? null;
      const intent = keys[s - 1] ?? { kind: "keep" };
      const original = (): Cell => {
        if (!orig) return { track: t, slot: s, from, to: from, why: "keep" };
        const want = orig.get(key(t, s)) ?? null;
        if (want !== null && !installed.has(want)) return { track: t, slot: s, from, to: from, why: "original-gone" };
        return { track: t, slot: s, from, to: want, why: "original" };
      };
      let cell: Cell;
      if (samples.has(key(t, s))) {
        cell = { track: t, slot: s, from, to: from, why: "user-sample" };
        if (intent.kind === "engine" || intent.kind === "original") skipped.push(cell);
      } else if (intent.kind === "engine") {
        const e = byDir.get(intent.dir);
        if (!e) errors.push(`${intent.dir} is not in the catalogue`);
        else if (!canGoOn(e, t)) errors.push(notForTrack(e, t));
        if (!e || !canGoOn(e, t)) {
          cell = { track: t, slot: s, from, to: from, why: "keep" };
        } else {
          cell = { track: t, slot: s, from, to: e.manifest.plug.id, why: "engine", dir: e.dir };
          if (!placed.includes(e.dir)) placed.push(e.dir);
        }
      } else if (intent.kind === "original") {
        cell = original();
      } else if (from !== null && ours.has(from)) {
        const back = original();
        cell = back.why === "original" ? { ...back, why: "revert" } : back;
      } else {
        cell = { track: t, slot: s, from, to: from, why: "keep" };
      }
      cells.push(cell);
    }
  }
  const loose = unassignedUserSamples(device.plugs, device.slots);
  if (loose.length) {
    warnings.push(`${loose.length === 1 ? "a sample" : `${loose.length} samples`} you made on the OP-Z ${loose.length === 1 ? "is" : "are"} in no slot `
      + `(${loose.map((p) => p.id).join(", ")}): content mode would delete ${loose.length === 1 ? "it" : "them"}, so step 4 stays locked until `
      + `${loose.length === 1 ? "it is" : "they are"} back in a slot`);
  }
  if (cells.some((c) => c.why === "original-gone")) {
    warnings.push("some original plugs are no longer installed on this OP-Z; those slots stay as they are");
  }

  // the engines: install, upgrade, or already there (checked as the core will, one after the other)
  let plugs: Doc = structuredClone(device.plugs);
  const installs: InstallItem[] = [];
  for (const dir of placed) {
    const e = byDir.get(dir)!;
    const m = e.manifest as Doc; // plug.json as written: plug.version and engine_version too
    const pid = pyInt(m.plug.id);
    const record = (plugs.plugs ?? []).find((p: Doc) => p.id === pid);
    const engine = toEngine(e, new Uint8Array(0));
    const item: InstallItem = {
      dir, label: e.label, plugId: pid, version: pyInt(m.plug.version ?? 1), action: "install", restart: false,
      presets: e.presets.length, built: e.engine !== null, bytes: null,
    };
    const caution = cautionOf(e);
    if (caution && !input.demo) errors.push(`${e.label} can't go on a real OP-Z yet: ${caution} Untick it.`);
    else if (caution) warnings.push(`${e.label}: ${caution} (the demo OP-Z never runs it)`);
    if (record && record.engine !== engine.devicePath) {
      errors.push(`plug id ${pid} on this OP-Z belongs to ${record.engine}, not ${engine.devicePath}: ${e.label} can't go on`);
    } else if (record && record.version >= item.version) {
      item.action = "ready";
      if (record.version > item.version) warnings.push(`${e.label}: the OP-Z has a newer version (${record.version}); it stays`);
    } else {
      item.action = record ? "upgrade" : "install";
      if (!item.built && !input.demo) errors.push(`${e.label} is not built yet, so it can't be installed: untick it`);
      const [errs, warns, p] = planInstall(plugs, device.slots, engine, [], null);
      errors.push(...errs.map((x) => `${e.label}: ${x}`));
      warnings.push(...warns.map((x) => `${e.label}: ${x}`));
      item.restart = p.restart;
      plugs = withInstalled(plugs, engine);
    }
    installs.push(item);
  }

  const targetCells = new Map(now);
  for (const c of cells) {
    if (c.to === null) targetCells.delete(key(c.track, c.slot));
    else targetCells.set(key(c.track, c.slot), c.to);
  }
  const target = cellsToDoc(targetCells, device.slots);
  const [slotErrors, slotWarnings] = slotMapProblems(plugs, target, device.slots);
  errors.push(...slotErrors);
  // every OP-Z's slot map lists tracks 12–15 (our own backups do): the core's note about them is no news here
  warnings.push(...slotWarnings.filter((w) => !/is not a sound track/.test(w)));
  return { cells, changes: cells.filter((c) => c.from !== c.to), target, installs, errors, warnings, blocked, skipped };
}

/** plugs.json as it will be after the core installs `engine` (for the checks of the next one). */
function withInstalled(plugs: Doc, engine: Engine): Doc {
  const m = engine.manifest;
  const doc = structuredClone(plugs);
  doc.engines = [...(doc.engines ?? []).filter((x: Doc) => x.path !== engine.devicePath),
    { memoryslot: pyInt(m.memoryslot), path: engine.devicePath, version: pyInt(m.engine_version ?? 1) }];
  doc.plugs = [...(doc.plugs ?? []).filter((p: Doc) => p.id !== pyInt(m.plug.id)),
    { engine: engine.devicePath, flags: pyInt(m.plug.flags ?? 0), id: pyInt(m.plug.id), samples: "", version: pyInt(m.plug.version) }];
  return doc;
}

/**
 * The slot map a restore writes: the backup's cells, except cells that hold one of the visitor's samples now (kept),
 * and plugs from the backup that are no longer installed (that cell is kept as it is). Written through the core's
 * writeSlotMap, which refuses anything that would leave a user sample in no slot.
 */
export function restoreTarget(backupSlots: Doc, device: Device): { target: Doc; kept: Cell[]; missing: number[] } {
  const want = slotCells(backupSlots);
  const now = slotCells(device.slots);
  const samples = userSampleCells(device);
  const installed = new Set<number>((device.plugs.plugs ?? []).map((p: Doc) => p.id));
  const cells = new Map(now);
  const kept: Cell[] = [];
  const missing = new Set<number>();
  for (const k of new Set([...want.keys(), ...now.keys()])) {
    const [t, s] = k.split(":").map(Number);
    const from = now.get(k) ?? null;
    const to = want.get(k) ?? null;
    if (from === to) continue;
    if (samples.has(k)) {
      kept.push({ track: t, slot: s, from, to: from, why: "user-sample" });
      continue;
    }
    if (to !== null && !installed.has(to)) {
      missing.add(to);
      kept.push({ track: t, slot: s, from, to: from, why: "original-gone" });
      continue;
    }
    if (to === null) cells.delete(k);
    else cells.set(k, to);
  }
  return { target: cellsToDoc(cells, device.slots), kept, missing: [...missing].sort((a, b) => a - b) };
}

/** "lead slot 3" */
export function cellName(c: { track: number; slot: number }): string {
  return `${TRACK_LABELS[c.track] ?? TRACK_NAMES[c.track] ?? c.track} slot ${c.slot}`;
}
