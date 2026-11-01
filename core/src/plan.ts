/**
 * Pure install planning (no device): patch.json, collision checks, slot-map edits, verification. Port of the pure
 * helpers in tools/opz.py; messages are the same strings.
 *
 * Settings documents are plain parsed JSON (`settings/plugs.json`, `settings/slotConfiguration.json`) and keep the
 * device's key order, so a document edited here serialises like the Python tools' output.
 */

import { utf8 } from "./bytes.ts";
import { clone, dumps, pyInt, pyRepr, pyStr } from "./pyjson.ts";

/* eslint-disable @typescript-eslint/no-explicit-any */
export type Doc = Record<string, any>;

export const PLUGS = "settings/plugs.json";
export const SLOTS = "settings/slotConfiguration.json";
export const PATCH = "settings/patch.json";
/** Imported at boot, then deleted (research/presets.md, Persistence). */
export const BOOT_PATCH = "settings/plugs_patch.json";
/** The OS reads at most 0x40000 bytes of an import file; stay well below. */
export const BOOT_PATCH_MAX = 128 * 1024;

export const TRACKS: Record<string, number> = {
  kick: 0, snare: 1, perc: 2, sample: 3, bass: 4, lead: 5, arp: 6, chord: 7, fx1: 8, fx2: 9, tape: 10, master: 11,
};
export const TRACK_NAMES: Record<number, string> = Object.fromEntries(Object.entries(TRACKS).map(([k, v]) => [v, k]));
export const TE_ENGINES = new Set([
  "analoga", "chord", "chorus", "chorus2", "cluster", "crush", "delay", "digital", "dist", "electric", "ep",
  "fdnreverb", "fm003b", "ministringshort", "organ", "reverb", "sampleplayergain", "saw013a", "synthsampler",
  "tapetwo", "volt",
]);
export const USER_PLUG_MIN = 60000;
export const MAX_SLOT = 89;
/** The 18 sound parameters, in `values` / `$0E` order. */
export const PARAM_NAMES = [
  "P1", "P2", "attack", "decay", "sustain", "release", "fx1", "fx2", "filter", "resonance", "pan", "level",
  "portamento", "lfo depth", "lfo speed", "lfo target", "lfo shape", "note style",
] as const;
/** Preset slot n is for this synth track. */
export const PRESET_SLOTS = ["bass", "lead", "arp", "chord"] as const;
export const PRESET_NAME_MAX = 11;
export const PRESET_INDEX_MAX = 13;
/** fx1, fx2, tape, master: no preset keys on the device, only preset 0 when the plug is chosen. */
export const FX_TRACK_MIN = 8;
/** trackmask bits of fx1, fx2, tape, master. */
export const FX_TRACKS_MASK = 0xf00;
export const MASTER = 11;
/** TE's master chorus: the one plug known to process master in place. */
export const MASTER_TE_PLUGS: ReadonlySet<number> = new Set([39]);

export class PlanError extends Error {
  override name = "PlanError";
}

/** An engine ready to install: plug.json (`manifest`), its presets.json, and the built .engine bytes. */
export interface Engine {
  name: string;
  manifest: Doc;
  presets: Doc[];
  data: Uint8Array;
  /** Where the engine lives on the OP-Z: `engines/<name>.engine`. */
  devicePath: string;
}

/** `load_engine` without the filesystem: `dirName` is the engine directory's name (the fallback engine name). */
export function makeEngine(manifest: Doc, presets: Doc[], data: Uint8Array, dirName?: string): Engine {
  const name = manifest.engine ?? dirName;
  if (typeof name !== "string") throw new PlanError("plug.json has no engine name");
  return { name, manifest, presets, data, devicePath: `engines/${name}.engine` };
}

/** A track name (kick, snare, ...) or 0..11. */
export function trackIndex(value: string | number): number {
  const key = String(value).toLowerCase();
  if (Object.prototype.hasOwnProperty.call(TRACKS, key)) return TRACKS[key];
  const n = pyInt(value);
  if (n < 0 || n > 11) throw new RangeError(`track must be a name (${Object.keys(TRACKS).join(", ")}) or 0..11`);
  return n;
}

export function tracksFromMask(mask: number): number[] {
  const out: number[] = [];
  for (let t = 0; t < 12; t++) if ((mask >> t) & 1) out.push(t);
  return out;
}

function trackName(t: number): string {
  return TRACK_NAMES[t];
}

/** Our presets.json -> the keys PlugManager's readPresetSettings reads (defaultOctave, index, name, slot, values). */
export function devicePresets(presets: Doc[]): Doc[] {
  return presets.map((p) => {
    const values = [...p.values];
    if (values.length !== 18 || !values.every((v) => typeof v === "number" && v >= 0 && v <= 255)) {
      throw new PlanError(`preset ${pyRepr(p.name ?? null)} must have 18 values in 0..255`);
    }
    return {
      slot: pyInt(p.slot ?? 0), index: pyInt(p.index ?? 0), name: pyStr(p.name ?? ""),
      defaultOctave: pyInt("defaultOctave" in p ? p.defaultOctave : (p.default_octave ?? 0)), values,
    };
  });
}

/** The preset slot the OP-Z reads on a track: track − 4 on the synth tracks 4–7, else 0. */
export function presetSlot(track: number): number {
  return track >= 4 && track <= 7 ? track - 4 : 0;
}

function isInt(v: unknown): v is number {
  return typeof v === "number" && Number.isInteger(v);
}

/** [errors, warnings] for a presets.json against what the OP-Z accepts and which slots the plug's tracks read. */
export function presetProblems(presets: Doc[], trackmask = 0): [string[], string[]] {
  const errors: string[] = [];
  const warnings: string[] = [];
  const seen = new Map<string, unknown>();
  presets.forEach((p, n) => {
    const name = "name" in p ? p.name : `#${n}`;
    const values = p.values;
    if (!Array.isArray(values) || values.length !== 18 || !values.every((v) => isInt(v) && v >= 0 && v <= 255)) {
      errors.push(`preset ${pyRepr(name)} must have 18 values in 0..255`);
    }
    const slot = "slot" in p ? p.slot : 0;
    const index = "index" in p ? p.index : 0;
    if (!isInt(slot) || slot < 0 || slot > 3) {
      errors.push(`preset ${pyRepr(name)}: slot must be 0..3 (${PRESET_SLOTS.join(", ")})`);
      return;
    }
    if (!isInt(index) || index < 0 || index > PRESET_INDEX_MAX) {
      errors.push(`preset ${pyRepr(name)}: index must be 0..${PRESET_INDEX_MAX}`);
      return;
    }
    const key = `${slot}/${index}`;
    if (seen.has(key)) errors.push(`presets ${pyRepr(seen.get(key))} and ${pyRepr(name)} share slot ${slot} index ${index}`);
    seen.set(key, name);
    if (typeof name !== "string" || [...name].length > PRESET_NAME_MAX) {
      warnings.push(`preset ${pyRepr(name)}: the OP-Z keeps ${PRESET_NAME_MAX} characters of the name`);
    }
    if (Array.isArray(values) && values.length === 18 && values[13] === 0) {
      warnings.push(
        `preset ${pyRepr(name)}: LFO depth 0 is full negative depth (the dial is bipolar, 128 = off); for no ` +
          "LFO use depth 128, speed 100, target 255, shape 0 like TE's default sound",
      );
    }
  });
  const tracks = tracksFromMask(trackmask);
  if (tracks.length && seen.size) {
    const read = [...new Set(tracks.map(presetSlot))].sort((a, b) => a - b);
    const used = [...new Set([...seen.keys()].map((k) => Number(k.split("/")[0])))].sort((a, b) => a - b);
    for (const slot of used.filter((s) => !read.includes(s))) {
      warnings.push(`presets in slot ${slot} (${PRESET_SLOTS[slot]}) are never used: the plug's tracks `
        + `(${tracks.map(trackName).join(", ")}) read slot ${read.join(" and ")}`);
    }
    for (const slot of read) {
      if (!seen.has(`${slot}/0`)) {
        const users = tracks.filter((t) => presetSlot(t) === slot).map(trackName);
        warnings.push(`no preset at slot ${slot} index 0: ${users.join(", ")} start${users.length > 1 ? "" : "s"} `
          + "from TE's default sound when switched to this plug");
      }
    }
    const extra = [...seen.keys()].filter((k) => k.startsWith("0/") && k !== "0/0");
    if (tracks.every((t) => t >= FX_TRACK_MIN) && extra.length) {
      warnings.push(`${extra.length} preset${extra.length > 1 ? "s are" : " is"} past slot 0 index 0 never used: effect `
        + `tracks (${tracks.map(trackName).join(", ")}) have no preset keys, only the preset applied when the plug is chosen`);
    }
  }
  return [errors, warnings];
}

/**
 * [tracks, indices] pairs: the preset indices each track can recall, grouped by equal sets. Preset index n is white
 * key n+1 (TrackContext::onNotePressed's key table); FX tracks have no preset keys. With no trackmask, the synth
 * tracks of the slots the presets fill.
 */
export function presetKeys(presets: Doc[], trackmask = 0): Array<[number[], number[]]> {
  const slots = new Map<number, Set<number>>();
  for (const p of presets) {
    const slot = pyInt(p.slot ?? 0);
    if (!slots.has(slot)) slots.set(slot, new Set());
    slots.get(slot)!.add(pyInt(p.index ?? 0));
  }
  const tracks = trackmask
    ? tracksFromMask(trackmask).filter((t) => t < FX_TRACK_MIN)
    : [...slots.keys()].filter((s) => s >= 0 && s <= 3).map((s) => 4 + s).sort((a, b) => a - b);
  const groups = new Map<string, [number[], number[]]>();
  for (const t of tracks) {
    const ix = [...(slots.get(presetSlot(t)) ?? [])].sort((a, b) => a - b);
    const key = ix.join(",");
    if (!groups.has(key)) groups.set(key, [[], ix]);
    groups.get(key)![0].push(t);
  }
  return [...groups.values()];
}

/** Preset indices as 1-based white keys, runs joined: [0, 1, 2, 4] -> "1-3, 5". */
function keyList(indices: number[]): string {
  const runs: Array<[number, number]> = [];
  for (const i of [...indices].sort((a, b) => a - b)) {
    if (runs.length && i === runs[runs.length - 1][1] + 1) runs[runs.length - 1][1] = i;
    else runs.push([i, i]);
  }
  return runs.map(([a, b]) => (a === b ? `${a + 1}` : `${a + 1}-${b + 1}`)).join(", ");
}

/** What the user should see: the white keys that light (solid) while holding a track with the plug chosen. */
export function presetKeysText(name: string, presets: Doc[], trackmask = 0): string {
  const parts = presetKeys(presets, trackmask).map(([ts, ix]) => (ix.length
    ? `${ts.map(trackName).join(", ")}: white key${ix.length > 1 ? "s" : ""} ${keyList(ix)}`
    : `${ts.map(trackName).join(", ")}: none`));
  if (!parts.length) return "";
  return `check on the OP-Z: hold the track with ${name} chosen; lit white keys = presets (${parts.join("; ")}). `
    + "Only white key 1 lit (it blinks after every sound-slot change) = no presets there: "
    + `opz.py presets push engines/${name}`;
}

/** A preset by name (case-insensitive), or by slot and index. */
export function findPreset(presets: Doc[], name: string | null = null, slot: number | null = null,
  index: number | null = null): Doc {
  if (name !== null) {
    const found = presets.filter((p) => String(p.name ?? "").toLowerCase() === name.toLowerCase());
    if (found.length !== 1) throw new PlanError(`${found.length ? "more than one" : "no"} preset named ${pyRepr(name)}`);
    return found[0];
  }
  const found = presets.find((p) => (p.slot ?? 0) === (slot ?? 0) && (p.index ?? 0) === (index ?? 0));
  if (found === undefined) throw new PlanError(`no preset at slot ${slot ?? 0} index ${index ?? 0}`);
  return found;
}

/** "P1=60 P2=70 attack=0 …", with the plug's knob names for P1/P2 when given. */
export function formatSound(values: readonly number[], knobs: readonly unknown[] | null = null): string {
  const names: string[] = [...PARAM_NAMES];
  (knobs ?? []).slice(0, 2).forEach((k, i) => (names[i] = String(k)));
  return values.slice(0, names.length).map((v, i) => `${names[i]}=${v}`).join(" ");
}

/** The CLI line that puts a track's sound back. */
export function setCommand(track: number, values: readonly number[]): string {
  return `opz.py presets set --track ${TRACK_NAMES[track] ?? track} --values ${values.join(",")}`;
}

export function checkSound(track: number, values: readonly number[]): number[] {
  if (!(track >= 0 && track <= 11)) throw new PlanError("track must be 0..11");
  if (values.length !== 18 || !values.every((v) => isInt(v) && v >= 0 && v <= 255)) {
    throw new PlanError("a sound needs 18 values in 0..255");
  }
  return [...values];
}

/**
 * A top-level plugs[] entry (imported without a version check) that only replaces presets: it repeats the installed
 * engine, samples, flags and version, which the import overwrites.
 */
export function plugEntry(record: Doc, presets: Doc[]): Doc {
  return {
    id: pyInt(record.id), version: pyInt(record.version), engine: record.engine, samples: record.samples ?? "",
    flags: pyInt(record.flags ?? 0), presets: devicePresets(presets),
  };
}

/** patch.json that only replaces one plug's presets (see plugEntry). */
export function buildPresetsPatch(record: Doc, presets: Doc[], nonce: number | null = null): Uint8Array {
  const doc: Doc = { plugs: [plugEntry(record, presets)] };
  if (nonce !== null) doc.nonce = nonce;
  return utf8(dumps(doc, { separators: [",", ":"], sortKeys: true }));
}

/**
 * The boot file's plugs[]: every installed plug in the user range that one of `engines` describes (same id and engine
 * path) and whose presets pass presetProblems. The first engine with an id wins. Returns [entries, one label per
 * entry, warnings]. Plugs we have no engine for (TE's, the app's sample plugs) are never in it.
 */
export function bootEntries(plugs: Doc, engines: Engine[]): [Doc[], string[], string[]] {
  const byId = new Map<number, Engine>();
  for (const e of engines) {
    const id = pyInt(e.manifest.plug.id);
    if (!byId.has(id)) byId.set(id, e);
  }
  const entries: Doc[] = [];
  const labels: string[] = [];
  const warnings: string[] = [];
  for (const record of [...(plugs.plugs ?? [])].sort((a: Doc, b: Doc) => a.id - b.id)) {
    const e = byId.get(record.id);
    if (record.id < USER_PLUG_MIN || e === undefined || !e.presets.length) continue;
    if (record.engine !== e.devicePath) {
      warnings.push(`plug ${record.id} is ${record.engine} on the OP-Z, not ${e.devicePath}: left out of the boot file`);
      continue;
    }
    if (presetProblems(e.presets, pyInt(e.manifest.plug.trackmask ?? 0))[0].length) {
      warnings.push(`${e.name}: presets.json has errors (opz.py presets check): left out of the boot file`);
      continue;
    }
    entries.push(plugEntry(record, e.presets));
    labels.push(`${e.name} ${record.id} (${e.presets.length})`);
  }
  return [entries, labels, warnings];
}

/** settings/plugs_patch.json: plugs[] only (no auto_import, no engines[], so no engine or track changes). */
export function buildBootPatch(entries: Doc[]): Uint8Array {
  return utf8(dumps({ plugs: entries }, { separators: [",", ":"], sortKeys: true }));
}

/** settings/patch.json for PlugManager::importPatch: engine definition, plug record, tracks to assign. */
export function buildPatch(engine: Engine, tracks: number[]): Uint8Array {
  const m = engine.manifest;
  const plug = m.plug;
  const doc = {
    auto_import: [{
      engine: { path: engine.devicePath, memoryslot: pyInt(m.memoryslot), version: pyInt(m.engine_version ?? 1) },
      plug: {
        id: pyInt(plug.id), version: pyInt(plug.version), engine: engine.devicePath,
        samples: plug.samples ?? "", flags: pyInt(plug.flags ?? 0), name: plug.name ?? engine.name,
        trackmask: pyInt(plug.trackmask ?? 0), parameter_names: plug.parameter_names ?? [],
        presets: devicePresets(engine.presets),
      },
      tracks,
    }],
  };
  return utf8(dumps(doc, { separators: [",", ":"], sortKeys: true }));
}

export function slotEntries(slots: Doc, track: number): Doc[] {
  const tr = (slots.tracks ?? []).find((t: Doc) => t.index === track);
  return tr ? tr.packs : [];
}

/** A copy of the slot map with plugId in track/slot (replacing what was there). */
export function assign(slots: Doc, track: number, slot: number, plugId: number): Doc {
  const doc = clone(slots);
  let tr = doc.tracks.find((t: Doc) => t.index === track);
  if (tr === undefined) {
    tr = { index: track, packs: [] };
    doc.tracks.push(tr);
    doc.tracks.sort((a: Doc, b: Doc) => a.index - b.index);
  }
  tr.packs = [...tr.packs.filter((p: Doc) => p.slot !== slot), { id: plugId, slot }];
  tr.packs.sort((a: Doc, b: Doc) => a.slot - b.slot);
  return doc;
}

/** A copy of the slot map without plugId anywhere. */
export function unassign(slots: Doc, plugId: number): Doc {
  const doc = clone(slots);
  for (const tr of doc.tracks) tr.packs = tr.packs.filter((p: Doc) => p.id !== plugId);
  return doc;
}

/** [track, slot] pairs that hold plugId. */
export function assignedTracks(slots: Doc, plugId: number): Array<[number, number]> {
  const out: Array<[number, number]> = [];
  for (const t of slots.tracks ?? []) for (const p of t.packs ?? []) if (p.id === plugId) out.push([t.index, p.slot]);
  return out;
}

// -- the user's own samples (tools/opz_samples.py; research/samples.md) -------------------------------------

/** Plug flag of samples made on the OP-Z, by TE's app or through content mode. */
export const USER_SAMPLE_FLAG = 0x02;
export const SAMPLES_USER_DIR = "samplepacks/user/";
/** Every pack our tools write lives here, and only there. */
export const OUR_SAMPLE_PREFIX = SAMPLES_USER_DIR + "opzk_";
/** Plug ids for sample plugs made by our tools (sdk/docs/memory-slots.md). */
export const OUR_SAMPLE_PLUG_MIN = 61400;
export const OUR_SAMPLE_PLUG_MAX = 61499;

/** A sample made on the OP-Z, by TE's app or through content mode: flag 2, any id. */
export function isUserSample(plug: Doc): boolean {
  return (Number(plug.flags ?? 0) & USER_SAMPLE_FLAG) !== 0;
}

export function isOurSample(plug: Doc): boolean {
  const id = Number(plug.id);
  return id >= OUR_SAMPLE_PLUG_MIN && id <= OUR_SAMPLE_PLUG_MAX && String(plug.samples ?? "").startsWith(OUR_SAMPLE_PREFIX)
    && !isUserSample(plug);
}

/**
 * Why removing this plug would harm content we don't own, or null. The OS deletes a removed plug's sample file
 * (FUN_800fe700), so a plug with a sample may only go if the sample is ours and no other plug uses it.
 */
export function removalBlock(plug: Doc, plugs: Doc): string | null {
  const samples = String(plug.samples ?? "");
  if (isUserSample(plug)) return `plug ${plug.id} is a user sample (${samples}); removing it deletes the sample file from the OP-Z`;
  if (samples && !isOurSample(plug)) return `plug ${plug.id} uses ${samples}, which our tools did not install; removing it deletes that file`;
  if (samples) {
    const others = (plugs.plugs ?? []).filter((p: Doc) => p.samples === samples && p.id !== plug.id).map((p: Doc) => p.id);
    if (others.length) return `plugs ${pyRepr(others)} also use ${samples}; removing plug ${plug.id} would delete it under them`;
  }
  return null;
}

/** plug id -> [track, slot] pairs, for every filled slot. */
export function sampleAssignments(slots: Doc): Map<number, Array<[number, number]>> {
  const out = new Map<number, Array<[number, number]>>();
  for (const t of slots.tracks ?? []) {
    for (const p of t.packs ?? []) {
      if (!out.has(p.id)) out.set(p.id, []);
      out.get(p.id)!.push([t.index, p.slot]);
    }
  }
  return out;
}

/**
 * User samples assigned in `before` and nowhere in `after`. Content mode removes unassigned user-sample plugs and then
 * deletes every file in samplepacks/user/ that no plug names (FUN_80038cd8 -> FUN_800ffc88, FUN_800364c4), so leaving
 * one unassigned puts the user's sample at risk.
 */
export function orphanedUserSamples(plugs: Doc, before: Doc, after: Doc): Doc[] {
  const a = sampleAssignments(before);
  const b = sampleAssignments(after);
  return (plugs.plugs ?? []).filter((p: Doc) => isUserSample(p) && a.has(p.id) && !b.has(p.id));
}

export function unassignedUserSamples(plugs: Doc, slots: Doc): Doc[] {
  const a = sampleAssignments(slots);
  return (plugs.plugs ?? []).filter((p: Doc) => isUserSample(p) && !a.has(p.id));
}

/**
 * Refuse a slot change that leaves one of the user's own samples in no slot: content mode would drop it and delete its
 * file (research/samples.md).
 */
export function orphanErrors(plugs: Doc, before: Doc, after: Doc): string[] {
  return orphanedUserSamples(plugs, before, after).map((p) => `slot change would leave user sample ${p.id} `
    + `(${p.samples ?? ""}) in no slot; the OP-Z deletes unassigned user samples in content mode. Pick another slot`);
}

export interface InstallPlan {
  /** Tracks patch.json assigns (first free slot); empty when a specific slot is written through the slot map. */
  tracks: number[];
  /** The slot map to upload when --slot was given, else null. */
  slotMap: Doc | null;
  /** Where the plug will be: [track, slot]. */
  assign: Array<[number, number]>;
  /** The engine was already registered: the OP-Z keeps the old code until it restarts. */
  restart: boolean;
}

/**
 * The trackmask names an effect track. Only such a plug may go on fx1, fx2, tape or master: what a synth or drum
 * engine does there is untested.
 */
export function effectPlug(manifest: Doc): boolean {
  return (pyInt(manifest.plug?.trackmask ?? 0) & FX_TRACKS_MASK) !== 0;
}

export function plugManifest(manifests: Doc[], plugId: number): Doc | null {
  return manifests.find((m) => pyInt(m.plug?.id) === plugId) ?? null;
}

export function instrumentError(plugId: number, track: number, manifest: Doc | null): string {
  const why = manifest ? `its trackmask 0x${pyInt(manifest.plug?.trackmask ?? 0).toString(16)} names no effect track`
    : "no local plug.json with that id names an effect track in its trackmask";
  return `plug ${plugId} cannot go on track ${trackName(track)}: ${why}, so it is a synth or drum engine, and what one `
    + "does on fx1, fx2, tape or master is untested (it may crash the OP-Z). Use tracks kick..chord (1-8)";
}

/** plug.json says the engine handles master's call: renderFX with out = NULL, result written back into in. */
export function masterInPlace(manifest: Doc): boolean {
  return manifest.master_in_place === true;
}

/** Plug ids that may go on master: TE's chorus and every manifest (plug.json) that declares it. */
export function masterPlugs(manifests: Doc[]): Set<number> {
  return new Set([...MASTER_TE_PLUGS, ...manifests.filter(masterInPlace).map((m) => pyInt(m.plug.id))]);
}

export function masterError(plugId: number): string {
  return `plug ${plugId} cannot go on track master: master runs its plug in place (renderFX gets out = NULL), so an `
    + "engine that writes out crashes the OP-Z, probably at every start of that project "
    + "(research/routing.md section 3). Only an engine whose plug.json declares \"master_in_place\": true "
    + "may go there";
}

/** Return [errors, warnings, plan]. Any error means: write nothing. */
export function planInstall(
  plugs: Doc, slots: Doc, engine: Engine, tracks: number[], slot: number | null, replace = false,
): [string[], string[], InstallPlan] {
  const m = engine.manifest;
  const errors: string[] = [];
  const warnings: string[] = [];
  const plug = m.plug;
  const pid = pyInt(plug.id);
  const pver = pyInt(plug.version);
  const mslot = pyInt(m.memoryslot);
  const ever = pyInt(m.engine_version ?? 1);
  if (TE_ENGINES.has(engine.name)) errors.push(`engine name ${pyRepr(engine.name)} is a TE engine; pick another name`);
  if (pid < USER_PLUG_MIN) errors.push(`plug id ${pid} is below ${USER_PLUG_MIN} (TE's range)`);
  if (!(mslot > 0 && mslot <= MAX_SLOT)) errors.push(`memory slot ${mslot} is outside 1..${MAX_SLOT}`);
  for (const e of plugs.engines ?? []) {
    if (e.memoryslot === mslot && e.path !== engine.devicePath) {
      errors.push(`memory slot ${mslot} is already used by ${pyStr(e.path)}`);
    }
    if (e.path === engine.devicePath && e.version === ever) {
      warnings.push(`engine version ${ever} is already registered: bump engine_version in plug.json`);
    }
  }
  const old = (plugs.plugs ?? []).find((p: Doc) => p.id === pid);
  if (old !== undefined) {
    if (old.engine !== engine.devicePath) errors.push(`plug id ${pid} already belongs to ${pyStr(old.engine)}`);
    else if (old.version >= pver) {
      errors.push(`plug ${pid} version ${old.version} is installed; the OP-Z ignores an import unless the version is higher `
        + "(bump plug.version in plug.json)");
    }
  }
  const mask = pyInt(plug.trackmask ?? 0);
  const [perrors, pwarnings] = presetProblems(engine.presets, mask);
  errors.push(...perrors);
  warnings.push(...pwarnings);
  const plan: InstallPlan = {
    tracks: [], slotMap: null, assign: [],
    restart: (plugs.engines ?? []).some((e: Doc) => e.path === engine.devicePath),
  };
  for (const t of tracks) {
    if (t >= FX_TRACK_MIN && !effectPlug(m)) {
      errors.push(instrumentError(pid, t, m));
      continue;
    }
    if (t === MASTER && !masterInPlace(m)) {
      errors.push(masterError(pid));
      continue;
    }
    if (mask && !((mask >> t) & 1)) {
      warnings.push(`track ${trackName(t)} is outside the plug's trackmask 0x${mask.toString(16)} (the device does not enforce it)`);
    }
    const packs = slotEntries(slots, t);
    if (packs.some((p) => p.id === pid)) {
      warnings.push(`plug ${pid} is already on track ${trackName(t)}`);
      continue;
    }
    if (slot !== null) {
      const taken = packs.find((p) => p.slot === slot);
      if (taken && !replace) {
        errors.push(`track ${trackName(t)} slot ${slot} holds plug ${pyStr(taken.id)} (use --replace to overwrite)`);
      }
      plan.assign.push([t, slot]);
    } else {
      const used = new Set(packs.map((p) => p.slot));
      let free: number | null = null;
      for (let s = 1; s < 11; s++) if (!used.has(s)) {
        free = s;
        break;
      }
      if (free === null) errors.push(`track ${trackName(t)} has no free slot (use --slot N --replace)`);
      else {
        plan.tracks.push(t);
        plan.assign.push([t, free]);
      }
    }
  }
  if (slot !== null) {
    let doc = slots;
    for (const [t, s] of plan.assign) doc = assign(doc, t, s, pid);
    plan.slotMap = doc;
    errors.push(...orphanErrors(plugs, slots, doc));
  }
  return [errors, warnings, plan];
}

/** Problems found when reading back after an install (empty: verified). */
export function verifyInstall(plugs: Doc, slots: Doc, engine: Engine, expected: Array<[number, number]>): string[] {
  const m = engine.manifest;
  const problems: string[] = [];
  const pid = pyInt(m.plug.id);
  const e = (plugs.engines ?? []).find((x: Doc) => x.path === engine.devicePath);
  if (e === undefined) problems.push(`engine ${engine.devicePath} missing from plugs.json`);
  else if (e.memoryslot !== pyInt(m.memoryslot) || e.version !== pyInt(m.engine_version ?? 1)) {
    problems.push(`engine definition differs: ${pyRepr(e)}`);
  }
  const p = (plugs.plugs ?? []).find((x: Doc) => x.id === pid);
  if (p === undefined) problems.push(`plug ${pid} missing from plugs.json`);
  else if (p.engine !== engine.devicePath || p.version !== pyInt(m.plug.version)) {
    problems.push(`plug record differs: ${pyRepr(p)}`);
  }
  const got = new Set(assignedTracks(slots, pid).map(([t, s]) => `${t}:${s}`));
  for (const [t, s] of expected) {
    if (!got.has(`${t}:${s}`)) problems.push(`plug ${pid} not in track ${trackName(t)} slot ${s}`);
  }
  return problems;
}

/** Engine files no plug uses any more (the OP-Z has no known way to delete them). */
export function orphanEngines(plugs: Doc): string[] {
  const used = new Set((plugs.plugs ?? []).map((p: Doc) => p.engine));
  return (plugs.engines ?? []).filter((e: Doc) => !used.has(e.path)).map((e: Doc) => e.path).sort();
}

/** The status table `opz.py status` prints. */
export function describe(plugs: Doc, slots: Doc): string {
  const engines = new Map<unknown, string>();
  for (const p of plugs.plugs ?? []) engines.set(p.id, String(p.engine.split("/").at(-1)).replaceAll(".engine", ""));
  const lines: string[] = [];
  const tracks = [...(slots.tracks ?? [])].sort((a: Doc, b: Doc) => a.index - b.index);
  for (const t of tracks) {
    const cells = t.packs.map((p: Doc) => `${p.slot}:${pyStr(p.id)}(${engines.get(p.id) ?? "?"})`);
    lines.push(`  ${(TRACK_NAMES[t.index] ?? String(t.index)).padEnd(7)} ${cells.join("  ")}`);
  }
  const user = (plugs.plugs ?? []).filter((p: Doc) => p.id >= USER_PLUG_MIN);
  lines.push("user plugs: " + (user.map((p: Doc) => `${p.id} ${p.engine} v${p.version}`).join(", ") || "none"));
  lines.push("orphan engines: " + (orphanEngines(plugs).join(", ") || "none"));
  const loose = unassignedUserSamples(plugs, slots);
  if (loose.length) {
    lines.push("WARNING: user samples in no slot (content mode deletes them; assign them again first): "
      + loose.map((p) => `${p.id} ${p.samples ?? ""}`).join(", "));
  }
  return lines.join("\n");
}

// -- slot map edits (the web app's plug librarian; TypeScript only, no opz.py command yet) ------------------------

/** "track:slot" -> plug id for every filled slot. Two maps with the same cells are the same arrangement. */
export function slotCells(slots: Doc): Map<string, number> {
  const out = new Map<string, number>();
  for (const t of slots.tracks ?? []) for (const p of t.packs ?? []) out.set(`${t.index}:${p.slot}`, p.id);
  return out;
}

/** Same plugs in the same slots (track and pack order, and empty tracks, do not matter). */
export function sameSlots(a: Doc, b: Doc): boolean {
  const x = slotCells(a);
  const y = slotCells(b);
  return x.size === y.size && [...x].every(([k, v]) => y.get(k) === v);
}

function cellName(key: string): string {
  const [t, s] = key.split(":").map(Number);
  return `${TRACK_NAMES[t] ?? t} slot ${s}`;
}

/** One line per slot that differs: "kick slot 3: 121 -> 61020", "lead slot 9: 10 -> empty". Track, then slot order. */
export function slotMapChanges(before: Doc, after: Doc): string[] {
  const x = slotCells(before);
  const y = slotCells(after);
  const keys = [...new Set([...x.keys(), ...y.keys()])].filter((k) => x.get(k) !== y.get(k));
  keys.sort((a, b) => {
    const [ta, sa] = a.split(":").map(Number);
    const [tb, sb] = b.split(":").map(Number);
    return ta - tb || sa - sb;
  });
  return keys.map((k) => `${cellName(k)}: ${x.get(k) ?? "empty"} -> ${y.get(k) ?? "empty"}`);
}

/**
 * [errors, warnings] for a slot map about to be written: slots 1..10, one plug per slot, installed plugs only; with
 * `before` (the map it replaces), no user sample assigned there may end up in no slot (orphanErrors).
 */
export function slotMapProblems(plugs: Doc, slots: Doc, before: Doc | null = null): [string[], string[]] {
  const errors: string[] = [];
  const warnings: string[] = [];
  const installed = new Set((plugs.plugs ?? []).map((p: Doc) => p.id));
  const seenTracks = new Set<number>();
  for (const t of slots.tracks ?? []) {
    if (!isInt(t.index) || t.index < 0 || t.index > 15) {
      errors.push(`track index ${pyRepr(t.index ?? null)} is outside 0..15`);
      continue;
    }
    if (seenTracks.has(t.index)) errors.push(`track ${trackName(t.index) ?? t.index} appears twice`);
    seenTracks.add(t.index);
    const used = new Set<number>();
    for (const p of t.packs ?? []) {
      const where = `track ${trackName(t.index) ?? t.index} slot ${pyRepr(p.slot ?? null)}`;
      if (!isInt(p.slot) || p.slot < 1 || p.slot > 10) errors.push(`${where}: slots are 1..10`);
      else if (used.has(p.slot)) errors.push(`${where} holds two plugs`);
      used.add(p.slot);
      if (!isInt(p.id)) errors.push(`${where}: plug id ${pyRepr(p.id ?? null)} is not a number`);
      else if (!installed.has(p.id)) errors.push(`${where}: plug ${p.id} is not installed`);
    }
    if (t.index > 11) warnings.push(`track ${t.index} is not a sound track (0..11)`);
  }
  if (before !== null) errors.push(...orphanErrors(plugs, before, slots));
  return [errors, warnings];
}
