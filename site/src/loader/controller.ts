/**
 * The loader's operations, without any DOM. A slimmed port of web/app/src/controller.ts: connect (Web MIDI or the
 * demo), one operation at a time, the core's flows, the interrupted-write record. The core does the safety work
 * (backup first, plan checks, confirm, read-back verify, slot-map rollback); this layer feeds it a transport, the
 * browser's backup store and a confirm callback that opens the loader's confirm sheet.
 *
 * Install is a batch: it backs up, plans from that backup (src/loader/plan.ts), asks once with every slot before →
 * after, then runs the core's own flows with `yes` (each one checks and verifies on its own): one `installMany` (a
 * push per new or upgraded engine with its own patch.json, one push for every preset refresh, one read-back for all;
 * no slot changes), then one `writeSlotMap` for the whole layout (refused if the OP-Z's map changed since the backup;
 * read back and rolled back if it differs). Nothing after a failure is written.
 *
 * Speed: every session costs a ~6.5 s wait for the OP-Z's next `$34` (docs/research/install-speed.md), so the opz
 * reuses a read-back made in the last few seconds as the next flow's backup (READ_REUSE), and a batch of N engines is
 * N + 4 sessions instead of 3 per engine plus 3 per preset refresh plus 3 for the slot map. All the engines in one push
 * made the OP-Z transfer for minutes without finishing (H-IS1, 2026-11-13).
 */

import type { CatalogueEngine } from "../shared/catalogue.ts";
import {
  type Clock, type Doc, type Engine, FileSession, Opz, ROOM_LOW, SLOTS, SessionError, TRACKS, type Transport, decodeFrame, loads,
  planRemoveOurs, readU16le, roomLevel, slotMapChanges, spaceReport,
} from "./core.ts";
import type { Choice } from "./model.ts";
import { type Cell, type InstallItem, cellName, planLoader, restoreTarget, toEngine } from "./plan.ts";
import { type BackupRecord, type LoaderStore, backupSlots, firstBackup } from "./store.ts";

export type ConnectionKind = "demo" | "midi";
export type OpName = "status" | "backup" | "install" | "restore" | "remove";

/** What the confirm sheet shows. */
export interface LoaderConfirm {
  op: "install" | "restore" | "remove";
  question: string;
  /** The backup this flow made before asking. */
  backupId: string | null;
  /** The settings that backup holds (for plug names). */
  device: { plugs: Doc; slots: Doc } | null;
  /** The cells that change, before → after. */
  changes: Cell[];
  /** Cells left alone because they hold the visitor's own samples (or a plug that is gone). */
  skipped: Cell[];
  installs: InstallItem[];
  /** The files sent, in words. */
  files: string[];
  effects: string[];
  restart: boolean;
  /** Room left for a content-mode restore after this write, when it gets short (docs/roadmap/restore-space.md). */
  room?: string;
  /** The core's lines so far. */
  lines: string[];
}

export interface InterruptedWrite {
  op: OpName;
  backupId: string;
  error: string;
  at: string;
}

export interface OpResult {
  op: OpName;
  ok: boolean;
  cancelled: boolean;
  /** An engine was upgraded: the OP-Z must restart before the new code runs. */
  restart: boolean;
  /** Lines worth showing: errors, warnings, verify problems, rollback, results. */
  highlights: string[];
  lines: string[];
  error?: string;
}

export interface DeviceState {
  plugs: Doc;
  slots: Doc;
  at: string;
}

export interface LoaderHooks {
  log(line: string): void;
  confirm(request: LoaderConfirm): Promise<boolean>;
  /** Connection, busy or device state changed. */
  changed(): void;
  /** Keep the screen on while a write runs; returns a release function. */
  hold?(): Promise<() => void>;
  /** The interrupted-write record changed (persist it: it must survive a reload). */
  interrupted?(record: InterruptedWrite | null): void;
}

/** What the install needs besides the device: the catalogue, the picks, and a way to get each `.engine`. */
export interface InstallRequest {
  engines: readonly CatalogueEngine[];
  choice: Choice;
  /** The built `.engine` (or the demo's placeholder); throws when it can't be had. */
  bytes(e: CatalogueEngine): Promise<Uint8Array>;
}

const WRITES = new Set<OpName>(["install", "restore", "remove"]);

/** How long a session open waits for the OP-Z's `$34`, then the pause before the one more try (seconds). */
export const OPEN_WAIT = 15;
export const OPEN_PAUSE = 3;
const NO_ANSWER = /no \$34 reply/;
/** A backup within this many seconds of a read-back, with nothing pushed since, takes that read-back (OpzOptions.reuseRead). */
export const READ_REUSE = 5;

/** A session open the OP-Z hasn't answered yet: the panel shows a countdown. */
export interface OpenWait {
  attempt: 1 | 2;
  seconds: number;
  /** Date.now() when this try started. */
  since: number;
}

/**
 * Opz that remembers the last settings it read (every flow ends with a read-back, which is the new status), counts
 * pushes, knows whether the last one has been read back yet, and opens each session with one more try: the OP-Z
 * sends nothing and receives no file before it answers `$34`, so trying the open again repeats no step of a flow. A
 * friend's install timed out right there, 2 ms after the backup's session closed (2026-11-05).
 */
class TrackingOpz extends Opz {
  lastRead: [Uint8Array, Uint8Array] | null = null;
  pushes = 0;
  /** A push started and no read-back has completed since: an error now may leave a write unchecked. */
  unverified = false;
  onWait: (w: OpenWait | null) => void = () => {};

  override async readSettings(): Promise<[Uint8Array, Uint8Array]> {
    const result = await super.readSettings();
    this.lastRead = result;
    this.unverified = false;
    return result;
  }

  /** As the core's push(); counted once the OP-Z has answered the open, the moment files can start to go out. */
  override push(files: Array<[string, Uint8Array]>, remove: number[] | null = null) {
    this.forgetRead();
    return this.session((s) => {
      this.pushes++;
      this.unverified = true;
      return s.push(files, remove);
    });
  }

  /** As the core's session(), with open() tried twice. */
  override async session<T>(body: (s: FileSession) => Promise<T>): Promise<T> {
    const s = await this.openSession();
    try {
      return await body(s);
    } finally {
      await s.close();
    }
  }

  private async openSession(): Promise<FileSession> {
    for (const attempt of [1, 2] as const) {
      const s = new FileSession(this.transport, { log: (m) => this.log("  " + m), clock: this.clock });
      this.onWait({ attempt, seconds: OPEN_WAIT, since: Date.now() });
      try {
        await s.open(OPEN_WAIT);
        return s;
      } catch (e) {
        if (attempt === 2 || !(e instanceof SessionError) || !NO_ANSWER.test(e.message)) throw e;
        this.log(`the OP-Z did not answer within ${OPEN_WAIT} s (nothing was sent in this session); trying once more in ${OPEN_PAUSE} s`);
        const until = this.clock() + OPEN_PAUSE;
        while (this.clock() < until) await this.transport.receive(Math.max(1, (until - this.clock()) * 1000));
      } finally {
        this.onWait(null);
      }
    }
    throw new Error("unreachable");
  }
}

/**
 * Warns when the OP-Z serves file-service frames to another client too (TE's app, another tab): the `$34` reply names
 * this session's client, `$53`/`$33` for a different one mean someone else is connected. A heuristic, from web/app.
 */
class WatchedTransport implements Transport {
  private client: number | null = null;
  private readonly seen = new Set<number>();
  private readonly inner: Transport;
  private readonly onForeign: (id: number) => void;

  constructor(inner: Transport, onForeign: (id: number) => void) {
    this.inner = inner;
    this.onForeign = onForeign;
  }

  send(sysex: Uint8Array): void | Promise<void> {
    return this.inner.send(sysex);
  }

  async receive(timeoutMs: number): Promise<Uint8Array | null> {
    const raw = await this.inner.receive(timeoutMs);
    const id = raw && raw.length > 6 ? raw[5] : -1;
    if (raw && (id === 0x34 || id === 0x33 || id === 0x53)) {
      try {
        const p = decodeFrame(raw).payload;
        if (p.length >= 2) {
          const who = readU16le(p, 0);
          if (id === 0x34) this.client = who;
          else if (this.client !== null && who !== this.client && !this.seen.has(who)) {
            this.seen.add(who);
            this.onForeign(who);
          }
        }
      } catch {
        // not a frame we read
      }
    }
    return raw;
  }
}

export class LoaderController {
  readonly store: LoaderStore;
  readonly hooks: LoaderHooks;
  transport: Transport | null = null;
  kind: ConnectionKind | null = null;
  deviceName = "";
  device: DeviceState | null = null;
  busy: OpName | null = null;
  interrupted: InterruptedWrite | null = null;
  /** A session open the OP-Z hasn't answered yet, or null. */
  waiting: OpenWait | null = null;
  private opz: TrackingOpz | null = null;
  private opLines: string[] | null = null;
  private lastBackupId: string | null = null;
  private lastBackup: Record<string, Uint8Array> | null = null;
  private onDisconnect: (() => void) | null = null;
  /** The restore in progress: what the sheet shows when the core asks. */
  private pendingRestore: { target: Doc; kept: Cell[]; missing: number[]; from: string } | null = null;
  /** removeAll: the first backup whose layout goes back where our plugs leave. */
  private pendingRemove: BackupRecord | null = null;

  constructor(store: LoaderStore, hooks: LoaderHooks) {
    this.store = store;
    this.hooks = hooks;
    store.onSave = (record) => {
      this.lastBackupId = record.id;
      this.lastBackup = record.files;
    };
  }

  get connected(): boolean {
    return this.transport !== null;
  }

  connect(transport: Transport, kind: ConnectionKind, deviceName: string, onDisconnect?: () => void, clock?: Clock): void {
    this.disconnect();
    this.transport = transport;
    this.kind = kind;
    this.deviceName = deviceName;
    this.onDisconnect = onDisconnect ?? null;
    this.store.source = kind === "demo" ? "demo" : deviceName;
    const watched = new WatchedTransport(transport, (id) => this.log(`warning: the OP-Z is also sending file-service frames to `
      + `client ${id}: is TE's app or another tab connected? Close it (one client at a time).`));
    this.opz = new TrackingOpz(watched, { log: (line) => this.log(line), backups: this.store, confirm: (q) => this.coreConfirm(q), clock, reuseRead: READ_REUSE });
    this.opz.onWait = (w) => {
      this.waiting = w;
      this.hooks.changed();
    };
    this.hooks.changed();
  }

  disconnect(): void {
    if (this.busy) throw new Error(`cannot disconnect while ${this.busy} is running`);
    this.drop();
  }

  /** Disconnect even while an operation runs: the running flow stops with an error, a write counts as interrupted. */
  forceDisconnect(): void {
    this.drop();
  }

  private drop(): void {
    this.onDisconnect?.();
    this.onDisconnect = null;
    this.transport = null;
    this.opz = null;
    this.kind = null;
    this.deviceName = "";
    this.device = null;
    this.hooks.changed();
  }

  setInterrupted(record: InterruptedWrite | null): void {
    this.interrupted = record;
    this.hooks.interrupted?.(record);
    this.hooks.changed();
  }

  private log(line: string): void {
    this.opLines?.push(line);
    this.hooks.log(line);
  }

  /** The core asks (restore through writeSlotMap, or removeOurs): show what changes, from the backup it just made. */
  private async coreConfirm(question: string): Promise<boolean> {
    if (this.busy === "remove") return this.removeConfirm(question);
    const r = this.pendingRestore;
    if (!r || this.busy !== "restore") return false;
    const now = this.lastBackup ? { plugs: loads(this.lastBackup["settings/plugs.json"]), slots: loads(this.lastBackup[SLOTS]) } : null;
    const changes: Cell[] = now ? slotMapChanges(now.slots, r.target).map((line) => parseChange(line)) : [];
    return this.hooks.confirm({
      op: "restore", question, backupId: this.lastBackupId, device: now, changes, skipped: r.kept, installs: [],
      files: [SLOTS],
      effects: [`the slot map of ${r.from} comes back; plugs installed since stay installed (out of their slots)`,
        ...(r.missing.length ? [`plug${r.missing.length > 1 ? "s" : ""} ${r.missing.join(", ")} from that backup ${r.missing.length > 1 ? "are" : "is"} no longer installed: those slots stay as they are`] : [])],
      restart: false, lines: [...(this.opLines ?? [])],
    });
  }

  /** The sheet for "take OP-FREE off": the core's own plan on the backup it just made, with the first backup's layout. */
  private async removeConfirm(question: string): Promise<boolean> {
    if (!this.lastBackup) return false;
    const now = { plugs: loads(this.lastBackup["settings/plugs.json"]), slots: loads(this.lastBackup[SLOTS]) };
    const first = this.pendingRemove;
    const plan = planRemoveOurs(now.plugs, now.slots, first ? backupSlots(first) : null);
    const r = spaceReport(now.plugs, now.slots);
    return this.hooks.confirm({
      op: "remove", question, backupId: this.lastBackupId, device: now,
      changes: slotMapChanges(now.slots, plan.slot_map).map((line) => parseChange(line)), skipped: [], installs: [],
      files: ["settings/plugs_patch.json (empty)", ...(plan.cleared.length ? [SLOTS] : []), `the removal of plug${plan.ids.length === 1 ? "" : "s"} ${plan.ids.join(", ")}`],
      effects: [
        `every OP-FREE sound leaves your OP-Z: ${plan.ids.length} plug${plan.ids.length === 1 ? "" : "s"}, which frees as many of its ${r.limit} sound definitions (${r.plugs} in use now)`,
        first ? `the slots they leave get back the sound they had in your first backup (${first.id}) where it's still installed; the others are left empty`
          : "the slots they leave are left empty (no backup from before OP-FREE in this browser)",
        "your samples, teenage engineering's sounds, your projects and your presets are not touched",
        "the engine files stay on the OP-Z's storage, out of any slot, where they do nothing (a few KB each; content mode never shows them); installing again reuses them",
        ...plan.kept.map(([pid, why]) => `plug ${pid} stays: ${why}`),
      ],
      restart: false, lines: [...(this.opLines ?? [])],
    });
  }

  /** One operation: busy guard, line capture, wake lock, error capture, status from the read-back. */
  private async run(op: OpName, body: (opz: TrackingOpz) => Promise<boolean>): Promise<OpResult> {
    if (!this.opz) throw new Error("not connected");
    if (this.busy) throw new Error(`${this.busy} is still running`);
    const opz = this.opz;
    this.busy = op;
    this.opLines = [];
    this.lastBackupId = null;
    this.lastBackup = null;
    opz.lastRead = null;
    opz.unverified = false;
    const pushes = opz.pushes;
    this.hooks.changed();
    const release = op === "status" ? null : await this.hooks.hold?.().catch(() => null);
    let ok = false;
    let error: string | undefined;
    try {
      ok = await body(opz);
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
      this.log(`error: ${error}`);
    } finally {
      release?.();
    }
    if (opz.lastRead && this.opz === opz) {
      this.device = { plugs: loads(opz.lastRead[0]), slots: loads(opz.lastRead[1]), at: new Date().toISOString() };
    }
    if (error !== undefined && WRITES.has(op)) {
      // only a push that wasn't read back can have left the OP-Z in a state nobody checked
      if (opz.unverified && this.lastBackupId !== null) {
        this.setInterrupted({ op, backupId: this.lastBackupId, error, at: new Date().toISOString() });
      } else if (opz.pushes === pushes) {
        this.log("nothing written: the OP-Z didn't answer, so nothing was sent.");
      } else {
        this.log("nothing was left half-written: everything sent before this was read back and checked.");
      }
    }
    const lines = this.opLines;
    this.opLines = null;
    this.busy = null;
    this.pendingRestore = null;
    this.pendingRemove = null;
    this.hooks.changed();
    return {
      op, ok, error, lines,
      cancelled: lines.includes("cancelled."),
      restart: ok && lines.some((l) => l.startsWith("RESTART THE OP-Z")),
      highlights: lines.filter((l) => /^(error|warning|verify|restoring|slot map restore|slot map written|nothing to change|removed|no plugs of ours|installed|nothing written|nothing was left|RESTART|presets pushed|batch:)/.test(l))
        .map((l) => l.split("\n")[0]),
    };
  }

  refresh(): Promise<OpResult> {
    return this.run("status", async (opz) => {
      await opz.readSettings();
      return true;
    });
  }

  backup(label = "opfree"): Promise<OpResult> {
    return this.run("backup", async (opz) => {
      await opz.backup(label);
      return true;
    });
  }

  /** The first settings backup this browser took of the connected OP-Z: what "original" puts back. */
  async firstBackup(): Promise<BackupRecord | null> {
    return firstBackup(await this.store.list(), this.store.source);
  }

  /** Install the picks: one confirm for the whole batch, then the core's flows (see the file comment). */
  install(request: InstallRequest): Promise<OpResult> {
    return this.run("install", async (opz) => {
      const b = await opz.backup("opfree-install");
      const first = await this.firstBackup();
      const device = { plugs: b.plugs, slots: b.slots };
      const plan = planLoader({
        engines: request.engines, choice: request.choice, device, original: first ? backupSlots(first) : null, demo: this.kind === "demo",
      });
      for (const w of plan.warnings) this.log("warning: " + w);
      for (const e of plan.errors) this.log("error:   " + e);
      if (plan.errors.length) {
        this.log("nothing written.");
        return false;
      }
      const work = plan.installs.filter((i) => i.action !== "ready");
      const repush = plan.installs.filter((i) => i.action === "ready" && i.presets > 0);
      if (!plan.changes.length && !work.length && !repush.length) {
        this.log("nothing to change: the OP-Z already has these sounds in these slots.");
        return true;
      }
      // every engine file is fetched before asking, so a missing one stops the batch before any write
      const byDir = new Map(request.engines.map((e) => [e.dir, e]));
      const engines = new Map<string, Engine>();
      for (const i of work) {
        const data = await request.bytes(byDir.get(i.dir)!);
        i.bytes = data.length;
        engines.set(i.dir, toEngine(byDir.get(i.dir)!, data));
      }
      for (const i of work) this.log(`plan: ${i.action} ${i.label} (plug ${i.plugId} v${i.version}, ${i.bytes} bytes, ${i.presets} presets)`);
      for (const i of repush) this.log(`plan: push the presets of ${i.label} again (plug ${i.plugId}, already installed)`);
      for (const c of plan.changes) this.log(`plan: ${cellName(c)}: ${c.from ?? "empty"} -> ${c.to ?? "empty"}`);
      const files = [
        ...work.map((i) => `${engines.get(i.dir)!.devicePath} (${kb(i.bytes ?? 0)})`),
        ...work.map((i) => `settings/patch.json: plug ${i.plugId} v${i.version} with ${i.presets} preset${i.presets === 1 ? "" : "s"}`),
        ...repush.map((i) => `settings/patch.json: ${i.presets} presets for plug ${i.plugId} (no version bump)`),
        ...(repush.length > 1 ? ["(the preset refreshes go in one patch.json)"] : []),
        ...(plan.changes.length ? [SLOTS] : []),
      ];
      const restart = work.some((i) => i.restart);
      const effects = [
        ...work.filter((i) => i.action === "install").map((i) => `${i.label} is added to the OP-Z (plug ${i.plugId})`),
        ...work.filter((i) => i.action === "upgrade").map((i) => `${i.label} is upgraded to v${i.version}`),
        "a plug taken out of its slot stays installed, so the original sound can go back at any time",
        "pushed presets live in the OP-Z's memory until it restarts; step 4 (content mode) keeps them for good",
      ];
      const added = work.filter((i) => i.action === "install").length;
      const room = installRoom(spaceReport(b.plugs, b.slots).free, added);
      if (room) this.log("warning: " + room);
      const yes = await this.hooks.confirm({
        op: "install", room: room ?? undefined,
        question: [
          work.length ? `Install ${work.length} engine${work.length === 1 ? "" : "s"}` : "",
          plan.changes.length ? `write ${plan.changes.length} slot${plan.changes.length === 1 ? "" : "s"}` : "",
          repush.length ? `push the presets of ${repush.length} installed engine${repush.length === 1 ? "" : "s"} again` : "",
        ].filter(Boolean).join(", ").replace(/^./, (x) => x.toUpperCase()).replace(/, ([^,]*)$/, " and $1") + "?",
        backupId: b.id, device, changes: plan.changes, skipped: plan.skipped, installs: plan.installs, files, effects, restart,
        lines: [...(this.opLines ?? [])],
      });
      if (!yes) {
        this.log("cancelled.");
        return false;
      }
      if (work.length || repush.length) {
        const ok = await opz.installMany(
          work.map((i) => engines.get(i.dir)!), repush.map((i) => toEngine(byDir.get(i.dir)!, new Uint8Array(0))), { yes: true },
        );
        const what = [...work.map((i) => i.label), ...repush.map((i) => `presets of ${i.label}`)].join(", ");
        this.log(`batch: ${what} ${ok ? (work.length ? "installed and verified" : "pushed and verified") : "FAILED"}`);
        if (!ok) {
          this.log("batch: stopped. The slot map was not changed.");
          return false;
        }
      }
      if (plan.changes.length) {
        const ok = await opz.writeSlotMap(plan.target, b.slots, "opfree-slots", { yes: true, manifests: request.engines.map((e) => e.manifest) });
        this.log(`batch: slot map ${ok ? "written and verified" : "FAILED (rolled back to the backup)"}`);
        if (!ok) return false;
      }
      this.log("batch: done.");
      return true;
    });
  }

  /**
   * Take every OP-FREE plug off the OP-Z (the core's removeOurs: one confirm, then an empty boot file, the slot map
   * and the removals, each read back). The slots go back to the first backup's sounds where those are still
   * installed. For a content-mode restore of an old OP-Z backup, or to go back to stock (docs/roadmap/restore-space.md).
   */
  removeAll(): Promise<OpResult> {
    return this.run("remove", async (opz) => {
      const first = await this.firstBackup();
      this.pendingRemove = first;
      return opz.removeOurs({ putBack: first?.id ?? null });
    });
  }

  /**
   * Put a backup's slot map back, through the core's writeSlotMap (backup, checks, confirm, push, read back,
   * rollback). Cells holding the visitor's samples now, and plugs no longer installed, stay as they are.
   */
  restore(backupId: string, manifests: Doc[] = []): Promise<OpResult> {
    return this.run("restore", async (opz) => {
      const record = await this.store.get(backupId);
      if (!record || !(SLOTS in record.files)) throw new Error(`backup ${backupId} has no ${SLOTS}`);
      const [plugsRaw, slotsRaw] = await opz.readSettings();
      const device = { plugs: loads(plugsRaw), slots: loads(slotsRaw) };
      const { target, kept, missing } = restoreTarget(backupSlots(record), device);
      for (const c of kept) {
        this.log(`warning: ${cellName(c)} stays as it is (${c.why === "user-sample" ? "your own sample" : `plug ${c.to ?? "?"} is no longer installed`})`);
      }
      this.pendingRestore = { target, kept, missing, from: backupId };
      return opz.writeSlotMap(target, device.slots, "restore", { manifests });
    });
  }
}

/**
 * What an install leaves for a later content-mode restore of the visitor's own backup: every sample it brings back
 * needs one of the OS's 200 plug definitions, and a full table drops the sample silently (docs/roadmap/restore-space.md).
 * Null when there is room.
 */
export function installRoom(freeNow: number, added: number): string | null {
  const after = freeNow - added;
  const level = roomLevel(after);
  if (level === "ok") return null;
  return `after this install ${Math.max(after, 0)} of the OP-Z's 200 sound definitions are free`
    + (level === "full" ? ": too few to restore a typical backup of your samples in content mode. Pick fewer engines, or take OP-FREE off (step 5) before such a restore"
      : `; a content-mode restore needs one per sample it brings back (fewer than ${ROOM_LOW} left). Before such a restore, take OP-FREE off (step 5)`);
}

/** "lead slot 3: 10 -> 61010" (the core's slotMapChanges) as a cell. */
function parseChange(line: string): Cell {
  const m = /^(\w+) slot (\d+): (\S+) -> (\S+)$/.exec(line);
  const id = (v: string) => (v === "empty" ? null : Number(v));
  return m ? { track: TRACKS[m[1]] ?? -1, slot: Number(m[2]), from: id(m[3]), to: id(m[4]), why: "original" }
    : { track: -1, slot: 0, from: null, to: null, why: "keep" };
}

function kb(n: number): string {
  return n < 1024 ? `${n} B` : `${(n / 1024).toFixed(1)} KB`;
}
