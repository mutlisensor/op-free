/**
 * Device operations with the safety rules in code: every write starts with a backup of both settings files, refuses
 * unsafe plans, asks for confirmation, verifies by reading both files back through a fresh session, and restores the
 * slot map if verification fails. Port of the `Opz` class in tools/opz.py (same flows, same log lines).
 */

import { fromUtf8, toHex, utf8 } from "./bytes.ts";
import {
  BOOT_PATCH, BOOT_PATCH_MAX, type Doc, type Engine, MASTER, PATCH, PLUGS, SLOTS, TRACK_NAMES, USER_PLUG_MIN, assign,
  assignedTracks, bootEntries, effectPlug, FX_TRACK_MIN, instrumentError, masterError, masterPlugs, plugManifest, slotCells, buildBootPatch, buildBatchPatch, buildPatch, buildPresetsPatch, checkSound, describe, devicePresets, formatSound, orphanEngines, planInstall, presetKeysText,
  orphanErrors, presetProblems, removalBlock, sameSlots, setCommand, slotEntries, slotMapChanges, slotMapProblems, unassign, verifyInstall,
} from "./plan.ts";
import { dumps, loads, pyEqual, pyInt, pyRepr } from "./pyjson.ts";
import { describeSpace, installRoom, planRemoveOurs, removeOursLines, spaceReport } from "./space.ts";
import { type Clock, FileSession, type PushResult, StateSession, type Transport, monotonic } from "./session.ts";

/** Where backups go (the CLI writes backups/<time>-<label>/; a web app might use IndexedDB or downloads). */
export interface BackupStore {
  /** Store the files of a new backup; returns its id. */
  save(label: string, files: Map<string, Uint8Array>): Promise<string>;
  load(id: string): Promise<Map<string, Uint8Array>>;
  /** Add a file to an existing backup (install.json after an install). */
  annotate(id: string, name: string, data: Uint8Array): Promise<void>;
}

/** Backups kept in memory, named like the CLI's directories: `<UTC time>-<label>`. */
export class MemoryBackupStore implements BackupStore {
  readonly backups = new Map<string, Map<string, Uint8Array>>();
  private readonly now: () => Date;

  constructor(now: () => Date = () => new Date()) {
    this.now = now;
  }

  async save(label: string, files: Map<string, Uint8Array>): Promise<string> {
    const stamp = this.now().toISOString().replace(/[-:]/g, "").replace(/\.(\d+)Z$/, "$1000Z");
    let id = stamp + (label ? "-" + label : "");
    for (let n = 2; this.backups.has(id); n++) id = `${stamp}.${n}` + (label ? "-" + label : "");
    this.backups.set(id, new Map(files));
    return id;
  }

  async load(id: string): Promise<Map<string, Uint8Array>> {
    const files = this.backups.get(id);
    if (files === undefined) throw new Error(`no backup ${id}`);
    return files;
  }

  async annotate(id: string, name: string, data: Uint8Array): Promise<void> {
    (await this.load(id)).set(name, data);
  }
}

export interface Backup {
  id: string;
  plugs: Doc;
  slots: Doc;
}

export interface OpzOptions {
  log?: (line: string) => void;
  backups?: BackupStore;
  /** Ask the user; the default refuses, so a write needs `yes` or a real prompt. */
  confirm?: (question: string) => boolean | Promise<boolean>;
  clock?: Clock;
  /**
   * Seconds within which a backup may take the settings the last read-back returned instead of reading them again,
   * when nothing was pushed since. 0 (the default) always reads. Every session costs a ~6.5 s wait for the OP-Z's next
   * `$34` (docs/research/install-speed.md), and a flow's backup usually follows the previous flow's read-back by
   * milliseconds, so a batch saves one session per step.
   */
  reuseRead?: number;
}

export interface WriteOptions {
  dryRun?: boolean;
  yes?: boolean;
  /** install, presetsPush: the caller writes the boot file next (persistPresets), so the advice says so. */
  boot?: boolean;
}

export interface SlotWriteOptions extends WriteOptions {
  /** assign, writeSlotMap: the library's plug.json files, which say which of our plugs may go on the effect tracks and on master. */
  manifests?: Doc[];
}

async function sha256(data: Uint8Array): Promise<string> {
  return toHex(new Uint8Array(await crypto.subtle.digest("SHA-256", data as Uint8Array<ArrayBuffer>)));
}

function compact(doc: Doc): Uint8Array {
  return utf8(dumps(doc, { separators: [",", ":"] }));
}

function where(assigned: Array<[number, number]>): string {
  return assigned.map(([t, s]) => `${TRACK_NAMES[t]} slot ${s}`).join(", ");
}

export class Opz {
  readonly transport: Transport;
  readonly log: (line: string) => void;
  readonly backups: BackupStore;
  readonly confirm: (question: string) => boolean | Promise<boolean>;
  readonly clock: Clock;
  readonly reuseRead: number;
  /** The last read-back and when it finished; dropped by any push (see OpzOptions.reuseRead). */
  private recentRead: { at: number; raw: [Uint8Array, Uint8Array] } | null = null;

  constructor(transport: Transport, options: OpzOptions = {}) {
    this.transport = transport;
    this.log = options.log ?? ((line) => console.log(line));
    this.backups = options.backups ?? new MemoryBackupStore();
    this.confirm = options.confirm ?? (() => false);
    this.clock = options.clock ?? monotonic;
    this.reuseRead = options.reuseRead ?? 0;
  }

  /** Forget the last read-back: the OP-Z's settings may change from here on. A subclass's own push must call it. */
  protected forgetRead(): void {
    this.recentRead = null;
  }

  /** Open a session, run body, always close. */
  async session<T>(body: (s: FileSession) => Promise<T>): Promise<T> {
    const s = new FileSession(this.transport, { log: (m) => this.log("  " + m), clock: this.clock });
    await s.open();
    try {
      return await body(s);
    } finally {
      await s.close();
    }
  }

  /** [plugs.json, slotConfiguration.json] as stored on the OP-Z. */
  async readSettings(): Promise<[Uint8Array, Uint8Array]> {
    this.recentRead = null;
    const raw = await this.session(async (s): Promise<[Uint8Array, Uint8Array]> => [await s.pull(PLUGS), await s.pull(SLOTS)]);
    this.recentRead = { at: this.clock(), raw };
    return raw;
  }

  async backup(label = ""): Promise<Backup> {
    const recent = this.recentRead;
    let plugsRaw: Uint8Array, slotsRaw: Uint8Array;
    if (recent !== null && this.reuseRead > 0 && this.clock() - recent.at <= this.reuseRead) {
      [plugsRaw, slotsRaw] = recent.raw;
      this.log(`  settings as read back ${(this.clock() - recent.at).toFixed(1)} s ago (nothing written since)`);
    } else {
      [plugsRaw, slotsRaw] = await this.readSettings();
    }
    const meta: Record<string, string> = { [PLUGS]: await sha256(plugsRaw), [SLOTS]: await sha256(slotsRaw) };
    const files = new Map([[PLUGS, plugsRaw], [SLOTS, slotsRaw], ["backup.json", utf8(dumps({ sha256: meta }, { indent: 2 }))]]);
    const id = await this.backups.save(label, files);
    this.log(`backup: ${id}`);
    return { id, plugs: loads(plugsRaw), slots: loads(slotsRaw) };
  }

  async push(files: Array<[string, Uint8Array]>, remove: number[] | null = null): Promise<PushResult> {
    this.forgetRead();
    return this.session((s) => s.push(files, remove));
  }

  async install(
    engine: Engine, tracks: number[], slot: number | null = null, replace = false, options: WriteOptions = {},
  ): Promise<boolean> {
    const b = await this.backup("install-" + engine.name);
    const [errors, warnings, plan] = planInstall(b.plugs, b.slots, engine, tracks, slot, replace);
    for (const w of warnings) this.log("warning: " + w);
    for (const e of errors) this.log("error:   " + e);
    if (errors.length) {
      this.log("nothing written.");
      return false;
    }
    const files: Array<[string, Uint8Array]> = [[engine.devicePath, engine.data], [PATCH, buildPatch(engine, plan.tracks)]];
    if (plan.slotMap !== null) files.push([SLOTS, compact(plan.slotMap)]);
    this.log(`plan: ${engine.devicePath} (${engine.data.length} bytes) as plug ${pyInt(engine.manifest.plug.id)} -> `
      + (where(plan.assign) || "no track"));
    const room = installRoom(b.plugs, pyInt(engine.manifest.plug.id));
    if (room) this.log("warning: " + room);
    if (options.dryRun) {
      this.log("dry run: nothing written. patch.json:\n" + fromUtf8(files[1][1]));
      return true;
    }
    if (!options.yes && !(await this.confirm("Install now?"))) {
      this.log("cancelled.");
      return false;
    }
    const result = await this.push(files);
    this.log(`sync: requested ${pyRepr(result.requested)}; skipped (unchanged) ${pyRepr(result.skipped)}`);
    const [plugs2, slots2] = (await this.readSettings()).map(loads);
    const problems = verifyInstall(plugs2, slots2, engine, plan.assign);
    if (problems.length) {
      for (const p of problems) this.log("verify:  " + p);
      this.log(`restoring the slot map from ${b.id}`);
      await this.restore(b.id, { yes: true });
      return false;
    }
    await this.backups.annotate(b.id, "install.json", utf8(dumps({
      engine: engine.devicePath, sha256: await sha256(engine.data), plug: engine.manifest.plug.id, assigned: plan.assign,
    }, { indent: 2 })));
    this.log("installed and verified.\n" + describe(plugs2, slots2));
    if (plan.restart) {
      this.log("RESTART THE OP-Z: this engine was already registered, and the OP-Z keeps the code it loaded at "
        + "boot until it restarts (proven on hardware 2026-10-17).");
    }
    if (engine.presets.length) {
      if (options.boot) {
        this.log("The OP-Z keeps the presets this install sent in RAM; the boot file written next brings them back "
          + `after ${plan.restart ? "the restart" : "a restart"} (research/presets.md).`);
      } else {
        this.log((plan.restart ? "The restart also drops the presets this install sent"
          : "The OP-Z keeps the presets this install sent in RAM only: a restart or crash drops them")
          + ` (research/presets.md); ${plan.restart ? "after it," : "then"} run: opz.py presets push engines/${engine.name}`);
      }
      const keys = presetKeysText(engine.name, engine.presets, pyInt(engine.manifest.plug.trackmask ?? 0));
      if (keys) this.log(keys);
    }
    return true;
  }

  /**
   * Install several engines and replace the presets of installed plugs in one push: one backup, one sync job with
   * every engine file and one patch.json (buildBatchPatch), one read-back. No slot changes (write the slot map next);
   * the read-back must show the slot map unchanged, or it is restored. The single-engine flow (install) costs three
   * sessions per engine, each with a ~6.5 s wait for the OP-Z (docs/research/install-speed.md).
   */
  async installMany(engines: Engine[], presets: Engine[] = [], options: WriteOptions = {}): Promise<boolean> {
    const b = await this.backup("install-batch");
    const errors: string[] = [];
    const warnings: string[] = [];
    const restart: string[] = [];
    const ids = new Map<number, string>();
    const mslots = new Map<number, string>();
    for (const e of [...engines, ...presets]) {
      const pid = pyInt(e.manifest.plug.id);
      if (ids.has(pid)) errors.push(`${e.name}: plug id ${pid} is also ${ids.get(pid)} in this batch`);
      ids.set(pid, e.name);
    }
    for (const e of engines) {
      const [errs, warns, plan] = planInstall(b.plugs, b.slots, e, [], null, false);
      errors.push(...errs.map((x) => `${e.name}: ${x}`));
      warnings.push(...warns.map((x) => `${e.name}: ${x}`));
      if (plan.restart) restart.push(e.name);
      const mslot = pyInt(e.manifest.memoryslot);
      if (mslots.has(mslot)) errors.push(`${e.name}: memory slot ${mslot} is also ${mslots.get(mslot)} in this batch`);
      mslots.set(mslot, e.name);
    }
    const records: Array<[Doc, Doc[]]> = [];
    for (const e of presets) {
      const pid = pyInt(e.manifest.plug.id);
      const record = (b.plugs.plugs ?? []).find((p: Doc) => p.id === pid);
      const [perrs, pwarns] = presetProblems(e.presets, pyInt(e.manifest.plug.trackmask ?? 0));
      if (record === undefined) errors.push(`${e.name}: plug ${pid} is not installed (install it first)`);
      else if (record.engine !== e.devicePath) errors.push(`${e.name}: plug id ${pid} belongs to ${record.engine}`);
      else records.push([record, e.presets]);
      if (!e.presets.length) errors.push(`${e.name}: presets.json is empty: nothing to push`);
      errors.push(...perrs.map((x) => `${e.name}: ${x}`));
      warnings.push(...pwarns.map((x) => `${e.name}: ${x}`));
    }
    if (!engines.length && !presets.length) errors.push("nothing to install");
    for (const w of warnings) this.log("warning: " + w);
    for (const e of errors) this.log("error:   " + e);
    if (errors.length) {
      this.log("nothing written.");
      return false;
    }
    const patch = (nonce: number | null) => buildBatchPatch(engines, records, nonce);
    const files: Array<[string, Uint8Array]> = [...engines.map((e): [string, Uint8Array] => [e.devicePath, e.data]), [PATCH, patch(null)]];
    for (const e of engines) {
      this.log(`plan: ${e.devicePath} (${e.data.length} bytes) as plug ${pyInt(e.manifest.plug.id)} v${pyInt(e.manifest.plug.version)}`
        + ` with ${e.presets.length} presets`);
    }
    for (const [record, ps] of records) this.log(`plan: ${ps.length} presets -> plug ${record.id} (${record.engine} v${record.version}), no version bump`);
    this.log(`plan: one push of ${files.length} file${files.length > 1 ? "s" : ""}, no slot changes`);
    if (options.dryRun) {
      this.log("dry run: nothing written. patch.json:\n" + fromUtf8(files[files.length - 1][1]));
      return true;
    }
    if (!options.yes && !(await this.confirm("Install now?"))) {
      this.log("cancelled.");
      return false;
    }
    let result = await this.push(files);
    this.log(`sync: requested ${pyRepr(result.requested)}; skipped (unchanged) ${pyRepr(result.skipped)}`);
    if (result.skipped.includes(PATCH)) {
      this.log("the OP-Z did not fetch patch.json (it still holds an identical one); sending it with a nonce");
      result = await this.push([[PATCH, patch(Math.trunc(this.clock() * 1000))]]);
      if (result.skipped.includes(PATCH)) {
        this.log("error:   the OP-Z did not fetch patch.json; nothing was imported");
        return false;
      }
    }
    const [plugs2, slots2] = (await this.readSettings()).map(loads);
    const problems: string[] = [];
    for (const e of engines) problems.push(...verifyInstall(plugs2, slots2, e, []).map((x) => `${e.name}: ${x}`));
    for (const [record] of records) {
      const now = (plugs2.plugs ?? []).find((p: Doc) => p.id === record.id) ?? null;
      if (!pyEqual(now, record)) problems.push(`plug record changed: ${pyRepr(record)} -> ${pyRepr(now)}`);
    }
    const slotsChanged = !pyEqual(slots2, b.slots);
    if (slotsChanged) problems.push("slot map changed");
    if (problems.length) {
      for (const p of problems) this.log("verify:  " + p);
      if (slotsChanged) {
        this.log(`restoring the slot map from ${b.id}`);
        await this.restore(b.id, { yes: true });
      }
      return false;
    }
    const sums: Doc[] = [];
    for (const e of engines) sums.push({ engine: e.devicePath, sha256: await sha256(e.data), plug: e.manifest.plug.id, assigned: [] });
    await this.backups.annotate(b.id, "install.json", utf8(dumps({ batch: sums, presets: records.map(([r]) => r.id) }, { indent: 2 })));
    this.log("installed and verified" + (records.length ? " (presets cannot be read back: plug records unchanged)" : "")
      + ".\n" + describe(plugs2, slots2));
    if (restart.length) {
      this.log(`RESTART THE OP-Z: ${restart.join(", ")} ${restart.length > 1 ? "were" : "was"} already registered, and the OP-Z `
        + "keeps the code it loaded at boot until it restarts (proven on hardware 2026-10-17).");
    }
    const withPresets = [...engines, ...presets].filter((e) => e.presets.length);
    if (withPresets.length) {
      this.log(options.boot
        ? "The OP-Z keeps the presets this install sent in RAM; the boot file written next brings them back after a restart (research/presets.md)."
        : "The OP-Z keeps the presets this install sent in RAM only: a restart or crash drops them (research/presets.md); "
          + `then run: opz.py presets push engines/<name> (${withPresets.map((e) => e.name).join(", ")})`);
      for (const e of withPresets) {
        const keys = presetKeysText(e.name, e.presets, pyInt(e.manifest.plug.trackmask ?? 0));
        if (keys) this.log(keys);
      }
    }
    return true;
  }

  /** Put an installed plug into a track's sound slot by writing only the slot map. */
  async assign(
    plugId: number, tracks: number[], slot: number | null = null, replace = false, options: SlotWriteOptions = {},
  ): Promise<boolean> {
    const b = await this.backup(`assign-${plugId}`);
    if (!(b.plugs.plugs ?? []).some((p: Doc) => p.id === plugId)) {
      this.log(`error:   plug ${plugId} is not installed`);
      return false;
    }
    let doc = b.slots;
    const done: Array<[number, number]> = [];
    const errors: string[] = [];
    const manifest = plugManifest(options.manifests ?? [], plugId);
    for (const t of tracks) {
      if (t >= FX_TRACK_MIN && plugId >= USER_PLUG_MIN && !(manifest && effectPlug(manifest))) {
        errors.push(instrumentError(plugId, t, manifest));
        continue;
      }
      if (t === MASTER && !masterPlugs(options.manifests ?? []).has(plugId)) {
        errors.push(masterError(plugId));
        continue;
      }
      const packs = slotEntries(doc, t);
      if (packs.some((p) => p.id === plugId)) {
        this.log(`warning: plug ${plugId} is already on track ${TRACK_NAMES[t]}`);
        continue;
      }
      let s: number | null;
      if (slot !== null) {
        const taken = packs.find((p) => p.slot === slot);
        if (taken && !replace) {
          errors.push(`track ${TRACK_NAMES[t]} slot ${slot} holds plug ${taken.id} (use --replace)`);
          continue;
        }
        s = slot;
      } else {
        const used = new Set(packs.map((p) => p.slot));
        s = null;
        for (let n = 1; n < 11; n++) if (!used.has(n)) {
          s = n;
          break;
        }
        if (s === null) {
          errors.push(`track ${TRACK_NAMES[t]} has no free slot (use --slot N --replace)`);
          continue;
        }
      }
      doc = assign(doc, t, s, plugId);
      done.push([t, s]);
    }
    errors.push(...orphanErrors(b.plugs, b.slots, doc));
    for (const e of errors) this.log("error:   " + e);
    if (errors.length || !done.length) {
      this.log("nothing written.");
      return !errors.length && !done.length;
    }
    this.log(`plan: plug ${plugId} -> ${where(done)}`);
    if (options.dryRun) {
      this.log("dry run: nothing written.");
      return true;
    }
    if (!options.yes && !(await this.confirm("Assign now?"))) {
      this.log("cancelled.");
      return false;
    }
    await this.push([[SLOTS, compact(doc)]]);
    const got = loads((await this.readSettings())[1]);
    const now = new Set(assignedTracks(got, plugId).map(([t, s]) => `${t}:${s}`));
    if (!done.every(([t, s]) => now.has(`${t}:${s}`))) {
      this.log(`verify:  assignment missing; restoring the slot map from ${b.id}`);
      await this.restore(b.id, { yes: true });
      return false;
    }
    this.log("assigned and verified.");
    return true;
  }

  async remove(
    plugId: number, unassignFirst = false, options: WriteOptions & { allowTe?: boolean } = {},
  ): Promise<boolean> {
    const b = await this.backup(`remove-${plugId}`);
    const record = (b.plugs.plugs ?? []).find((p: Doc) => p.id === plugId);
    if (record === undefined) {
      this.log(`error:   plug ${plugId} is not installed`);
      return false;
    }
    if (plugId < USER_PLUG_MIN && !options.allowTe) {
      this.log(`error:   plug ${plugId} is in TE's range; refusing`);
      return false;
    }
    const blocked = removalBlock(record, b.plugs);
    if (blocked !== null) {
      this.log(`error:   ${blocked}; refusing`);
      return false;
    }
    const at = assignedTracks(b.slots, plugId);
    if (at.length && !unassignFirst) {
      this.log(`error:   plug ${plugId} is assigned to `
        + at.map(([t, s]) => `${TRACK_NAMES[t] ?? t} slot ${s}`).join(", ") + "; use --unassign");
      return false;
    }
    if (options.dryRun) {
      const what = at.length ? "[" + at.map(([t, s]) => `(${t}, ${s})`).join(", ") + "]" : "nothing";
      this.log(`dry run: would unassign ${what} and remove plug ${plugId}`);
      return true;
    }
    if (!options.yes && !(await this.confirm(`Remove plug ${plugId}?`))) {
      this.log("cancelled.");
      return false;
    }
    if (at.length) {
      await this.push([[SLOTS, compact(unassign(b.slots, plugId))]]);
      if (assignedTracks(loads((await this.readSettings())[1]), plugId).length) {
        this.log("error:   unassigning failed; plug not removed");
        return false;
      }
    }
    await this.push([], [plugId]);
    const plugs2 = loads((await this.readSettings())[0]);
    if ((plugs2.plugs ?? []).some((p: Doc) => p.id === plugId)) {
      this.log(`verify:  plug ${plugId} still present`);
      return false;
    }
    const orphans = orphanEngines(plugs2);
    this.log(`removed plug ${plugId}. orphan engine files (no known delete): ${orphans.length ? pyRepr(orphans) : "none"}`);
    return true;
  }

  /**
   * Take every plug of ours off the OP-Z in three verified writes, one confirm: an empty boot file first (a stale one
   * would bring removed plugs back at the next start, research/presets.md), the slot map without them (with
   * `putBack`, a backup id: the plugs that backup had in those cells go back where they are still installed), then one
   * sync job of remove operations. The user's samples are never touched. Engine files stay (no normal-mode delete is
   * known); they are a few KB each and content mode never shows them. Port of `Opz.remove_ours` in tools/opz.py.
   */
  async removeOurs(options: WriteOptions & { putBack?: string | null } = {}): Promise<boolean> {
    const b = await this.backup("remove-ours");
    let putBack: Doc | null = null;
    if (options.putBack) {
      const raw = (await this.backups.load(options.putBack)).get(SLOTS);
      if (raw === undefined) throw new Error(`backup ${options.putBack} has no ${SLOTS}`);
      putBack = loads(raw);
    }
    const plan = planRemoveOurs(b.plugs, b.slots, putBack);
    for (const [pid, why] of plan.kept) this.log(`warning: plug ${pid} stays: ${why}`);
    for (const e of plan.errors) this.log("error:   " + e);
    if (plan.errors.length) {
      this.log("nothing written.");
      return false;
    }
    if (!plan.ids.length) {
      this.log("no plugs of ours on the OP-Z: nothing to remove.");
      return true;
    }
    for (const line of removeOursLines(plan, BOOT_PATCH)) this.log(line);
    if (options.dryRun) {
      this.log("dry run: nothing written.");
      return true;
    }
    if (!options.yes && !(await this.confirm(`Remove ${plan.ids.length > 1 ? `all ${plan.ids.length} plugs` : "the 1 plug"} of ours?`))) {
      this.log("cancelled.");
      return false;
    }
    await this.push([[BOOT_PATCH, buildBootPatch([])]]);
    if (!pyEqual(plan.slot_map, b.slots)) {
      await this.push([[SLOTS, compact(plan.slot_map)]]);
      if (!pyEqual(loads((await this.readSettings())[1]), plan.slot_map)) {
        this.log(`verify:  slot map differs; restoring the slot map from ${b.id}; no plug removed`);
        await this.restore(b.id, { yes: true });
        return false;
      }
    }
    await this.push([], plan.ids);
    const [rawPlugs, rawSlots] = await this.readSettings();
    const plugs2 = loads(rawPlugs);
    const gone = new Set(plan.ids);
    const left = (plugs2.plugs ?? []).filter((p: Doc) => gone.has(p.id)).map((p: Doc) => p.id);
    if (left.length) {
      this.log(`verify:  still present: ${left.join(", ")} (run it again)`);
      return false;
    }
    this.log(`removed ${plan.ids.length} plug${plan.ids.length > 1 ? "s" : ""} of ours and verified. The user's samples and TE's plugs are untouched.\n`
      + describeSpace(spaceReport(plugs2, loads(rawSlots))));
    return true;
  }

  /**
   * Upload the backed-up slot map. touch re-serialises it (same content, different bytes) so the OP-Z must really
   * download it: a zero-change test of the upload path.
   */
  async restore(backupId: string, options: { yes?: boolean; touch?: boolean } = {}): Promise<boolean> {
    let raw = (await this.backups.load(backupId)).get(SLOTS);
    if (raw === undefined) throw new Error(`backup ${backupId} has no ${SLOTS}`);
    const want = loads(raw);
    if (options.touch) raw = utf8(dumps(want, { indent: 1 }) + "\n");
    if (!options.yes && !(await this.confirm(`Restore the slot map from ${backupId}?`))) {
      this.log("cancelled.");
      return false;
    }
    const result = await this.push([[SLOTS, raw]]);
    if (options.touch && result.skipped.includes(SLOTS)) {
      this.log("warning: the OP-Z did not request the touched slot map (upload path not exercised)");
    }
    const got = loads((await this.readSettings())[1]);
    const ok = pyEqual(got, want);
    this.log(`slot map restore ${ok ? "verified" : "FAILED: readback differs"}`);
    return ok;
  }

  /**
   * Write a whole slot map (move, swap, replace or clear slots; the plugs must be installed): backup, checks, confirm,
   * push, read back, roll back if the read-back differs. `base` is the slot map the edit started from: if the OP-Z's
   * map differs from it now (changed on the device, or by another tool), nothing is written. One of our plugs newly
   * put on fx1, fx2, tape or master needs a manifest in `options.manifests` whose trackmask names an effect track; on
   * master it must also declare `master_in_place` (or be TE's chorus). TypeScript only (the web
   * app's plug librarian); opz.py has no such command yet.
   */
  async writeSlotMap(slots: Doc, base: Doc | null = null, label = "slots", options: SlotWriteOptions = {}): Promise<boolean> {
    const b = await this.backup(label);
    const [errors, warnings] = slotMapProblems(b.plugs, slots, b.slots);
    const before = slotCells(b.slots);
    const allowed = masterPlugs(options.manifests ?? []);
    for (const [cell, id] of slotCells(slots)) {
      const track = Number(cell.split(":")[0]);
      if (track < FX_TRACK_MIN || track > MASTER || before.get(cell) === id) continue;
      const manifest = plugManifest(options.manifests ?? [], id);
      if (id >= USER_PLUG_MIN && !(manifest && effectPlug(manifest))) errors.push(instrumentError(id, track, manifest));
      else if (track === MASTER && !allowed.has(id)) errors.push(masterError(id));
    }
    if (base !== null && !sameSlots(base, b.slots)) {
      errors.unshift("the slot map on the OP-Z changed since it was read: "
        + (slotMapChanges(base, b.slots).join("; ") || "(order only)") + "; read the status again");
    }
    for (const w of warnings) this.log("warning: " + w);
    for (const e of errors) this.log("error:   " + e);
    if (errors.length) {
      this.log("nothing written.");
      return false;
    }
    const changes = slotMapChanges(b.slots, slots);
    if (!changes.length) {
      this.log("nothing to change: the OP-Z's slot map is already like this.");
      return true;
    }
    for (const c of changes) this.log("plan: " + c);
    if (options.dryRun) {
      this.log("dry run: nothing written.");
      return true;
    }
    if (!options.yes && !(await this.confirm(`Write the slot map (${changes.length} slot${changes.length > 1 ? "s" : ""})?`))) {
      this.log("cancelled.");
      return false;
    }
    await this.push([[SLOTS, compact(slots)]]);
    const got = loads((await this.readSettings())[1]);
    if (!sameSlots(got, slots)) {
      for (const c of slotMapChanges(slots, got)) this.log("verify:  " + c.replace(" -> ", " expected, read back "));
      this.log(`restoring the slot map from ${b.id}`);
      await this.restore(b.id, { yes: true });
      return false;
    }
    this.log("slot map written and verified.");
    return true;
  }

  // -- presets --------------------------------------------------------------------------------------------------

  /**
   * Replace an installed plug's presets: patch.json with only a plugs[] entry, so no version bump, no engine import
   * and no slot change. Verified by reading back an unchanged plug record and slot map (the presets themselves are not
   * readable). options.boot: the caller writes the boot file next.
   */
  async presetsPush(engine: Engine, options: WriteOptions = {}): Promise<boolean> {
    const b = await this.backup("presets-" + engine.name);
    const plug = engine.manifest.plug;
    const pid = pyInt(plug.id);
    const record = (b.plugs.plugs ?? []).find((p: Doc) => p.id === pid);
    const [errors, warnings] = presetProblems(engine.presets, pyInt(plug.trackmask ?? 0));
    if (record === undefined) errors.unshift(`plug ${pid} is not installed (install it first)`);
    else if (record.engine !== engine.devicePath) errors.unshift(`plug id ${pid} belongs to ${record.engine}`);
    if (!engine.presets.length) errors.push("presets.json is empty: nothing to push");
    for (const w of warnings) this.log("warning: " + w);
    for (const e of errors) this.log("error:   " + e);
    if (errors.length) {
      this.log("nothing written.");
      return false;
    }
    const patch = buildPresetsPatch(record, engine.presets);
    this.log(`plan: ${engine.presets.length} presets -> plug ${pid} (${record.engine} v${record.version}) through `
      + "patch.json plugs[] (no version bump)");
    if (options.dryRun) {
      this.log("dry run: nothing written. patch.json:\n" + fromUtf8(patch));
      return true;
    }
    if (!options.yes && !(await this.confirm("Push presets now?"))) {
      this.log("cancelled.");
      return false;
    }
    let result = await this.push([[PATCH, patch]]);
    if (result.skipped.includes(PATCH)) {
      this.log("the OP-Z did not fetch patch.json (it still holds an identical one); sending it with a nonce");
      result = await this.push([[PATCH, buildPresetsPatch(record, engine.presets, Math.trunc(this.clock() * 1000))]]);
      if (result.skipped.includes(PATCH)) {
        this.log("error:   the OP-Z did not fetch patch.json; presets not pushed");
        return false;
      }
    }
    const [plugs2, slots2] = (await this.readSettings()).map(loads);
    const problems: string[] = [];
    const record2 = (plugs2.plugs ?? []).find((p: Doc) => p.id === pid) ?? null;
    if (!pyEqual(record2, record)) problems.push(`plug record changed: ${pyRepr(record)} -> ${pyRepr(record2)}`);
    const slotsChanged = !pyEqual(slots2, b.slots);
    if (slotsChanged) problems.push("slot map changed");
    if (problems.length) {
      for (const p of problems) this.log("verify:  " + p);
      if (slotsChanged) {
        this.log(`restoring the slot map from ${b.id}`);
        await this.restore(b.id, { yes: true });
      }
      return false;
    }
    await this.backups.annotate(b.id, "presets.json", utf8(dumps(devicePresets(engine.presets), { indent: 2 })));
    this.log("presets pushed and verified (plug record and slot map unchanged; presets cannot be read back).");
    this.log(options.boot
      ? "The OP-Z keeps them in RAM; the boot file written next brings them back after a restart (research/presets.md)."
      : "The OP-Z keeps them until it restarts: push again after a restart (research/presets.md).");
    const keys = presetKeysText(engine.name, engine.presets, pyInt(plug.trackmask ?? 0));
    if (keys) this.log(keys);
    return true;
  }

  /**
   * Write the boot file settings/plugs_patch.json: a plugs[] entry for every installed plug of ours that `engines`
   * describes (bootEntries), or none with `off`. The OP-Z imports it at once (a file whose path contains "patch.json"),
   * and again at its next start, before loading the preset caches; that start deletes it whether or not the import
   * worked. So it brings the presets back after one restart and must be written again after each. It cannot be read
   * back: verified by unchanged plug records and slot map; a copy goes into the backup.
   */
  async persistPresets(engines: Engine[], options: WriteOptions & { off?: boolean } = {}): Promise<boolean> {
    const b = await this.backup("boot-presets");
    const [entries, labels, warnings] = options.off ? [[], [], []] : bootEntries(b.plugs, engines);
    for (const w of warnings) this.log("warning: " + w);
    const data = buildBootPatch(entries);
    if (data.length > BOOT_PATCH_MAX) {
      this.log(`error:   the boot file would be ${data.length} bytes (limit ${BOOT_PATCH_MAX}); nothing written.`);
      return false;
    }
    if (entries.length) {
      this.log(`plan: boot file ${BOOT_PATCH} (${data.length} bytes), ${entries.length} plug${entries.length > 1 ? "s" : ""} `
        + `(presets): ${labels.join(", ")}. The OP-Z imports it now (the same presets, no version bump) and again at its `
        + "next start, which deletes it");
    } else {
      this.log(`plan: empty boot file ${BOOT_PATCH}: the OP-Z imports nothing now or at its next start, which deletes it`);
    }
    if (options.dryRun) {
      this.log("dry run: nothing written. plugs_patch.json:\n" + fromUtf8(data));
      return true;
    }
    if (!options.yes && !(await this.confirm("Write the boot file now?"))) {
      this.log("cancelled.");
      return false;
    }
    const result = await this.push([[BOOT_PATCH, data]]);
    this.log(`sync: requested ${pyRepr(result.requested)}; skipped (unchanged) ${pyRepr(result.skipped)}`);
    if (result.skipped.includes(BOOT_PATCH)) {
      this.log("the OP-Z already holds this boot file (no restart since it was written): unchanged");
    }
    const [plugs2, slots2] = (await this.readSettings()).map(loads);
    const before = new Map<number, Doc>((b.plugs.plugs ?? []).map((p: Doc) => [p.id, p]));
    const after = new Map<number, Doc>((plugs2.plugs ?? []).map((p: Doc) => [p.id, p]));
    const problems: string[] = [];
    for (const pid of [...new Set([...before.keys(), ...after.keys()])].sort((x, y) => x - y)) {
      if (!pyEqual(before.get(pid) ?? null, after.get(pid) ?? null)) {
        problems.push(`plug ${pid} changed: ${pyRepr(before.get(pid))} -> ${pyRepr(after.get(pid))}`);
      }
    }
    const slotsChanged = !pyEqual(slots2, b.slots);
    if (slotsChanged) problems.push("slot map changed");
    if (problems.length) {
      for (const p of problems) this.log("verify:  " + p);
      if (slotsChanged) {
        this.log(`restoring the slot map from ${b.id}`);
        await this.restore(b.id, { yes: true });
      }
      this.log("to empty the boot file before the next start: opz.py presets persist --off");
      return false;
    }
    await this.backups.annotate(b.id, "plugs_patch.json", data);
    this.log(entries.length
      ? "boot file written and verified (plug records and slot map unchanged; the file cannot be read back). At its "
        + "next start the OP-Z restores these presets without a push and deletes the file: after a restart, run opz.py "
        + "presets persist (or any install or presets push) to cover the next."
      : "boot file emptied and verified.");
    return true;
  }

  /** Open a StateSync session that has reported `tracks`, run body, always close. */
  async stateSession<T>(tracks: readonly number[], body: (s: StateSession) => Promise<T>): Promise<T> {
    const s = new StateSession(this.transport, { log: (m) => this.log("  " + m), clock: this.clock });
    await s.open(tracks);
    try {
      return await body(s);
    } finally {
      s.close();
    }
  }

  /** Play `values` on `track` through `$0E` while hold(session) runs, then put the track's sound back. */
  async audition(
    track: number, values: readonly number[], hold: (s: StateSession) => Promise<unknown>, label = "sound",
    options: { yes?: boolean } = {},
  ): Promise<boolean> {
    const sound = checkSound(track, values);
    return this.stateSession([track], async (s) => {
      const original = s.sounds.get(track)!;
      this.log(`track ${TRACK_NAMES[track]} now: ${formatSound(original)}`);
      this.log("if this is interrupted, put it back with: " + setCommand(track, original));
      if (!options.yes && !(await this.confirm(`Audition ${label} on track ${TRACK_NAMES[track]} (its sound comes back afterwards)?`))) {
        this.log("cancelled.");
        return false;
      }
      await s.set(track, sound);
      this.log(`auditioning ${label} on track ${TRACK_NAMES[track]}`);
      try {
        await hold(s);
      } finally {
        await s.set(track, original);
        this.log(`track ${TRACK_NAMES[track]} restored`);
      }
      return true;
    });
  }

  /** Set a track's sound in the active pattern through `$0E` and leave it (saved with the project). */
  async setSound(track: number, values: readonly number[], options: WriteOptions = {}): Promise<boolean> {
    const sound = checkSound(track, values);
    return this.stateSession([track], async (s) => {
      const original = s.sounds.get(track)!;
      this.log(`track ${TRACK_NAMES[track]} now: ${formatSound(original)}`);
      this.log(`plan:    track ${TRACK_NAMES[track]} -> ${formatSound(sound)}`);
      if (options.dryRun) {
        this.log("dry run: nothing written.");
        return true;
      }
      if (!options.yes && !(await this.confirm(`Set track ${TRACK_NAMES[track]}'s sound (changes the active pattern)?`))) {
        this.log("cancelled.");
        return false;
      }
      await s.set(track, sound);
      await s.pump(0.5);
      this.log("set. to undo: " + setCommand(track, original));
      return true;
    });
  }

  /** Log the sounds the OP-Z reports (`$0E`): the current ones, then every change. Read-only. */
  async watch(tracks: readonly number[] = [], seconds: number | null = null, knobs: readonly unknown[] | null = null,
    stop: () => boolean = () => false): Promise<boolean> {
    const line = (t: number, v: number[]) => `${String(TRACK_NAMES[t] ?? t).padEnd(7)} ${formatSound(v, knobs)}`;
    return this.stateSession(tracks, async (s) => {
      for (const t of [...s.sounds.keys()].sort((a, b) => a - b)) {
        if (!tracks.length || tracks.includes(t)) this.log(line(t, s.sounds.get(t)!));
      }
      let waited = 0;
      while ((seconds === null || waited < seconds) && !stop()) {
        for (const [t, v] of await s.pump(1.0)) if (!tracks.length || tracks.includes(t)) this.log(line(t, v));
        waited += 1.0;
      }
      return true;
    });
  }
}
