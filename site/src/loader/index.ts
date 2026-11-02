/**
 * The loader (workstream W2): a guided panel next to the synth lab. Five steps, the current one highlighted:
 * 1 pick (per track group, community or original), 2 connect and back up (Web MIDI or the demo OP-Z), 3 install
 * (one confirm, the core's flows), 4 content mode (the user-sample check, then the presets baked into the OP-Z's own
 * store), 5 eject and play, and restore from any backup.
 *
 * The device work is in controller.ts (the core's flows), the picks in model.ts, the slots in plan.ts; this file is
 * the DOM. It talks to the lab only through DOM events (src/shared/mount.ts): it sends `opfree:audition` and listens
 * for `opfree:pick`.
 */

import "./loader.css";
import { type CatalogueEngine, loadCatalogue } from "../shared/catalogue.ts";
import type { AuditionEvent, Mounted, PickEvent } from "../shared/mount.ts";
import { type LoaderConfirm, LoaderController, type OpResult } from "./controller.ts";
import { DemoDisk, demoFiles } from "./demo.ts";
import { type BakeEngine, FakeOpz, isUserSample } from "./core.ts";
import { type BakeRequest, type DiskDir, backupMeta, diskPicker, runBake } from "./disk.ts";
import { type Child, clear, download, formatTime, h } from "./dom.ts";
import {
  type Choice, GROUPS, type GroupId, MAX_PICKS, SLOTS_PER_TRACK, capacity, engineTracks, group, groupEngines, sanitizeChoice,
  cautionOf, setSource, slotIntents, toggleOriginalSlot, togglePick, TRACK_LABELS,
} from "./model.ts";
import { type LoaderPlan, blockedSlots, planLoader, plugLabel, presetDocs, userSampleCells } from "./plan.ts";
import { type SampleCheck, type SampleGate, gateOpen, gateRefusal, looseText, sampleCheck, sampleGate } from "./samplegate.ts";
import { bakeSheet, confirmSheet, failureSheet, openSheet } from "./sheet.ts";
import {
  type BackupRecord, type LoaderStore, MemoryLoaderStore, backupSlots, firstBackup, isSettingsBackup, isUserPresetsBackup, openStore,
} from "./store.ts";
import { DemoTransport, loadDemoFiles, loadDemoState, resetDemoFiles, saveDemoFiles, saveDemoState } from "./transport/demo.ts";
import { workerSleep } from "./transport/timer.ts";
import { type MidiAccessLike, PORT_LOST, WebMidiTransport, pickOpz, ports, requestMidi } from "./transport/webmidi.ts";

const KEYS = { choice: "op-free.loader.choice", group: "op-free.loader.group", step: "op-free.loader.step", interrupted: "op-free.loader.interrupted" };
const STEPS = ["Pick", "Connect & back up", "Install", "Content mode", "Eject & play"] as const;
/** Backups the loader makes itself (the core's own, per step, are folded away in step 5). */
const MAIN_BACKUPS = new Set(["opfree-connect", "opfree", "opfree-install", "restore"]);

function read<T>(key: string): T | null {
  try {
    const raw = localStorage.getItem(key);
    return raw ? (JSON.parse(raw) as T) : null;
  } catch {
    return null;
  }
}

function write(key: string, value: unknown): void {
  try {
    if (value === null) localStorage.removeItem(key);
    else localStorage.setItem(key, JSON.stringify(value));
  } catch {
    // private window or blocked storage: the panel just forgets
  }
}

/** Why Web MIDI can't be used here, or null. */
function midiUnavailable(): string | null {
  if (typeof (navigator as Navigator & { requestMIDIAccess?: unknown }).requestMIDIAccess !== "function") {
    return "This browser has no Web MIDI. Use Chrome or Edge on a computer (Safari and every iOS browser have none). The demo OP-Z works everywhere.";
  }
  if (!globalThis.isSecureContext) return "Web MIDI needs a secure page: open this site over https (or localhost).";
  return null;
}

class LoaderPanel {
  private readonly el: HTMLElement;
  private engines: CatalogueEngine[] = [];
  private catalogueError: string | null = null;
  private choice: Choice | null = null;
  private groupId: GroupId = read<GroupId>(KEYS.group) ?? "synth";
  private open: number = read<number>(KEYS.step) ?? 1;
  private store: LoaderStore = new MemoryLoaderStore();
  private controller!: LoaderController;
  private backups: BackupRecord[] = [];
  private first: BackupRecord | null = null;
  private demo: DemoTransport | null = null;
  private demoDisk: DemoDisk | null = null;
  private midi: { access: MidiAccessLike; transport: WebMidiTransport; listener: () => void } | null = null;
  private connectError: string | null = null;
  private notice: { text: string; kind: "ok" | "warn" | "error" } | null = null;
  private lastInstall: OpResult | null = null;
  private lastBake: { ok: boolean; text: string } | null = null;
  private sampleLast: SampleCheck | null = null;
  private sampleConfirmed = false;
  private overwriteUser = false;
  private log: string[] = [];
  private destroyed = false;
  private readonly onPick = (e: Event) => this.pickFromLab(e as PickEvent);
  /** Set while the loader sends its own opfree:pick, so it doesn't handle its own event. */
  private telling = false;

  constructor(el: HTMLElement) {
    this.el = el;
    el.classList.add("ld");
    document.addEventListener("opfree:pick", this.onPick);
    void this.start();
  }

  destroy(): void {
    this.destroyed = true;
    document.removeEventListener("opfree:pick", this.onPick);
    if (this.controller?.connected) this.controller.forceDisconnect();
    this.el.classList.remove("ld");
    this.el.replaceChildren();
  }

  private async start(): Promise<void> {
    this.render();
    this.store = await openStore();
    this.controller = new LoaderController(this.store, {
      log: (line) => {
        this.log.push(line);
        if (this.log.length > 400) this.log.splice(0, this.log.length - 400);
      },
      confirm: (r) => this.confirm(r),
      changed: () => this.render(),
      hold: async () => {
        try {
          const lock = await (navigator as Navigator & { wakeLock?: { request(t: "screen"): Promise<{ release(): Promise<void> }> } }).wakeLock?.request("screen");
          return () => void lock?.release();
        } catch {
          return () => {};
        }
      },
      interrupted: (r) => write(KEYS.interrupted, r),
    });
    this.controller.interrupted = read(KEYS.interrupted);
    try {
      this.engines = (await loadCatalogue()).engines;
    } catch (e) {
      this.catalogueError = (e as Error).message;
    }
    this.choice = sanitizeChoice(read(KEYS.choice), this.engines);
    // the lab starts with the featured engines ticked: tell it where the loader's (stored) picks differ
    const picked = new Set(GROUPS.flatMap((g) => this.choice![g.id].picks));
    for (const e of this.engines) if (!!e.featured !== picked.has(e.dir) && GROUPS.some((g) => groupEngines(this.engines, g.id).includes(e))) this.tellLab(e.dir, picked.has(e.dir));
    await this.reloadBackups();
    this.render();
  }

  // -- state ----------------------------------------------------------------------------------------------------

  private setChoice(next: Choice): void {
    this.choice = next;
    write(KEYS.choice, next);
    this.render();
  }

  private setOpen(step: number): void {
    this.open = step;
    write(KEYS.step, step);
    this.render();
  }

  private async reloadBackups(): Promise<void> {
    this.backups = await this.store.list();
    this.first = this.controller?.connected ? firstBackup(this.backups, this.store.source) : null;
  }

  /** Slots each group must skip (the visitor's samples on every track), once connected. */
  private blocked(id: GroupId): number[] {
    const d = this.controller?.device;
    return d ? blockedSlots(id, userSampleCells(d)) : [];
  }

  private plan(): LoaderPlan | null {
    const d = this.controller?.device;
    if (!d || !this.choice) return null;
    return planLoader({
      engines: this.engines, choice: this.choice, device: d, original: this.first ? backupSlots(this.first) : null,
      demo: this.controller.kind === "demo",
    });
  }

  private gate(): SampleGate {
    const c = this.controller;
    return sampleGate({ midi: !!c?.connected, device: c?.device ?? null, last: this.sampleLast, confirmed: this.sampleConfirmed });
  }

  /** The step to highlight: the first one not done. */
  private current(): number {
    if (this.lastBake?.ok) return 5;
    if (this.lastInstall?.ok) return 4;
    if (this.controller?.connected && this.first) return 3;
    return this.open <= 1 ? 1 : 2;
  }

  /** Keep the lab's "add to my OP-Z" in step with the ticks here. */
  private tellLab(dir: string, picked: boolean): void {
    this.telling = true;
    try {
      document.dispatchEvent(new CustomEvent("opfree:pick", { detail: { dir, picked } }) as PickEvent);
    } finally {
      this.telling = false;
    }
  }

  private pickFromLab(e: PickEvent): void {
    if (!this.choice || this.telling) return;
    const engine = this.engines.find((x) => x.dir === e.detail?.dir);
    const g = engine && GROUPS.find((x) => groupEngines(this.engines, x.id).includes(engine));
    if (!engine || !g) return;
    const edit = togglePick(this.choice[g.id], engine.dir, e.detail.picked, this.blocked(g.id));
    this.groupId = g.id;
    this.notice = edit.refused ? { text: edit.refused, kind: "warn" } : null;
    if (edit.refused) this.tellLab(engine.dir, false);
    this.setChoice({ ...this.choice, [g.id]: edit.choice });
  }

  // -- connection -----------------------------------------------------------------------------------------------

  private async connectDemo(): Promise<void> {
    this.connectError = null;
    const fake = new FakeOpz(loadDemoFiles(undefined, demoFiles));
    loadDemoState(fake);
    this.demo = new DemoTransport(fake, workerSleep());
    this.demoDisk ??= new DemoDisk();
    this.controller.connect(this.demo, "demo", "Demo OP-Z", () => {
      this.demo = null;
    }, this.demo.clock);
    await this.afterConnect();
  }

  private async connectMidi(): Promise<void> {
    this.connectError = null;
    this.render();
    try {
      const access = await requestMidi();
      const input = pickOpz(ports(access.inputs));
      const output = pickOpz(ports(access.outputs));
      if (!input || !output) {
        const seen = ports(access.inputs).map((p) => p.name).filter(Boolean);
        throw new Error("No OP-Z MIDI port found. Plug the OP-Z in over USB, switch it on in normal mode, then try again."
          + (seen.length ? ` Ports seen: ${seen.join(", ")}.` : ""));
      }
      const transport = new WebMidiTransport(input, output, workerSleep());
      await transport.open();
      const listener = () => {
        if (transport.checkPorts() && this.controller.transport === transport) {
          if (this.controller.busy) this.controller.forceDisconnect();
          else this.controller.disconnect();
          this.notice = { text: PORT_LOST, kind: "warn" };
          this.render();
        }
      };
      access.addEventListener?.("statechange", listener);
      this.midi = { access, transport, listener };
      this.controller.connect(transport, "midi", input.name ?? "OP-Z", () => {
        access.removeEventListener?.("statechange", listener);
        transport.close();
        this.midi = null;
      });
      await this.afterConnect();
    } catch (e) {
      this.connectError = (e as Error).message;
      this.render();
    }
  }

  /** Read the status and back up straight away: the first backup is what "original" goes back to. */
  private async afterConnect(): Promise<void> {
    const r = await this.controller.backup("opfree-connect");
    this.saveDemo();
    await this.reloadBackups();
    if (!r.ok) {
      await failureSheet(r, "Back up");
      return;
    }
    const d = this.controller.device;
    if (d && this.controller.kind === "midi") this.sampleLast = sampleCheck(d);
    if (this.open < 3) this.setOpen(3);
    else this.render();
  }

  private disconnect(): void {
    if (this.controller.busy) return;
    if (this.controller.device && this.controller.kind === "midi") this.sampleLast = sampleCheck(this.controller.device);
    this.controller.disconnect();
    this.first = null;
    this.render();
  }

  private saveDemo(): void {
    if (this.demo) {
      saveDemoFiles(this.demo.fake.files);
      saveDemoState(this.demo.fake);
    }
  }

  // -- writes ---------------------------------------------------------------------------------------------------

  private confirm(r: LoaderConfirm): Promise<boolean> {
    return confirmSheet(r, this.engines, this.controller.kind === "midi");
  }

  private async engineBytes(e: CatalogueEngine): Promise<Uint8Array> {
    if (e.engine === null) {
      if (this.controller.kind !== "demo") throw new Error(`${e.label} is not built: it can't be installed`);
      return demoPlaceholder(e.dir);
    }
    const r = await fetch(new URL(e.engine, document.baseURI));
    if (!r.ok) throw new Error(`${e.engine}: HTTP ${r.status}`);
    const data = new Uint8Array(await r.arrayBuffer());
    if (!data.length) throw new Error(`${e.engine} is empty`);
    return data;
  }

  private async install(): Promise<void> {
    if (!this.choice) return;
    const r = await this.controller.install({ engines: this.engines, choice: this.choice, bytes: (e) => this.engineBytes(e) });
    this.saveDemo();
    await this.reloadBackups();
    if (r.cancelled) return this.render();
    this.lastInstall = r;
    if (r.ok) {
      if (this.controller.device && this.controller.kind === "midi") this.sampleLast = sampleCheck(this.controller.device);
      this.notice = { kind: r.restart ? "warn" : "ok", text: r.lines.some((l) => l.startsWith("nothing to change"))
        ? "Nothing to change: your OP-Z already has these sounds in these slots."
        : "Installed and verified by reading back." + (r.restart ? " Restart the OP-Z before you play: it keeps the old engine code until then." : "") };
      this.setOpen(4);
    } else {
      this.render();
      await failureSheet(r, "Install");
    }
  }

  private async restore(id: string): Promise<void> {
    const r = await this.controller.restore(id, this.engines.map((e) => e.manifest));
    this.saveDemo();
    await this.reloadBackups();
    this.render();
    if (r.ok && !r.cancelled) this.notice = { text: r.lines.includes("slot map written and verified.") ? "Restored and verified." : "Nothing to change: the OP-Z already has that layout.", kind: "ok" };
    else if (!r.cancelled) await failureSheet(r, "Restore");
    if (r.ok && this.controller.interrupted?.backupId === id) this.controller.setInterrupted(null);
    this.render();
  }

  private async refresh(): Promise<void> {
    const r = await this.controller.refresh();
    if (r.ok && this.controller.device && this.controller.kind === "midi") this.sampleLast = sampleCheck(this.controller.device);
    if (!r.ok) await failureSheet(r, "Reading the OP-Z");
    this.render();
  }

  private bakeEngines(): BakeEngine[] {
    if (!this.choice) return [];
    const dirs = GROUPS.flatMap((g) => (this.choice![g.id].source === "community" ? this.choice![g.id].picks : []));
    return dirs.map((d) => this.engines.find((e) => e.dir === d)!).filter(Boolean)
      .map((e) => ({ name: e.dir, manifest: e.manifest, presets: presetDocs(e) }));
  }

  /** The bake or a put-back on the content-mode disk (the demo disk in demo mode). */
  private async disk(request: BakeRequest): Promise<void> {
    const refusal = gateRefusal(this.gate());
    if (refusal) {
      await openSheet("Not yet: your samples", h("p", null, refusal), [{ value: "ok", label: "OK", kind: "primary" }], "gate-refused");
      return;
    }
    let dir: DiskDir;
    if (this.controller.kind === "demo" || (!this.controller.connected && this.demoDisk && !diskPicker())) {
      dir = (this.demoDisk ??= new DemoDisk());
    } else {
      const pick = diskPicker();
      if (!pick) return;
      try {
        dir = await pick({ id: "opz-disk", mode: "readwrite" });
      } catch (e) {
        if ((e as Error).name !== "AbortError") this.lastBake = { ok: false, text: (e as Error).message };
        return this.render();
      }
    }
    const demo = dir === this.demoDisk;
    const result = await runBake(dir, request, {
      log: (line) => this.log.push(line),
      backup: async (data, source, sha) => {
        const meta = new TextEncoder().encode(backupMeta(source, sha, data.length));
        const id = await this.store.saveUserPresets(data, meta, demo ? "demo disk" : source);
        if (!demo) download(`${id}-userPresets.dat`, data);
        return id;
      },
      confirm: (c) => bakeSheet(c),
    });
    await this.reloadBackups();
    if (result.cancelled) return this.render();
    if (!result.ok) {
      this.lastBake = { ok: false, text: result.error ?? "the bake stopped" };
    } else if (!result.written) {
      this.lastBake = { ok: true, text: result.lines.find((l) => l.startsWith("nothing to change")) ?? "Nothing to change." };
    } else {
      this.lastBake = { ok: true, text: `Written and read back${result.backupId ? `; the original is backup ${result.backupId}` : ""}. Now eject the disk.` };
    }
    if (result.warnings.length) this.lastBake.text += " " + result.warnings.join(" ");
    if (result.written) this.notice = { kind: "ok", text: this.lastBake.text };
    if (this.lastBake.ok && request.kind === "bake") this.setOpen(5);
    else this.render();
  }

  // -- rendering ------------------------------------------------------------------------------------------------

  private render(): void {
    if (this.destroyed) return;
    const focusKey = (document.activeElement as HTMLElement | null)?.dataset?.key;
    const scroll = this.el.scrollTop;
    clear(this.el, this.view());
    this.el.scrollTop = scroll;
    if (focusKey) this.el.querySelector<HTMLElement>(`[data-key="${CSS.escape(focusKey)}"]`)?.focus();
  }

  private view(): Child[] {
    const c = this.controller;
    const busy = c?.busy ?? null;
    const head = h("header", { class: "ld-head" },
      h("p", { class: "label" }, "Your OP-Z"),
      h("p", { class: `ld-conn ${c?.connected ? "is-on" : ""}`, "data-testid": "connection" },
        h("i", { class: "ld-led", "aria-hidden": "true" }),
        c?.connected ? (c.kind === "demo" ? "Demo OP-Z" : `USB · ${c.deviceName}`) : "Not connected"));
    const out: Child[] = [head];
    if (busy) {
      out.push(h("p", { class: "ld-busy", role: "status", "data-testid": "busy" }, h("i", { class: "ld-spin", "aria-hidden": "true" }),
        busy === "install" ? "Installing… keep this tab in front and the OP-Z connected." : busy === "restore" ? "Restoring…" : "Reading the OP-Z…"));
    }
    if (c?.interrupted) {
      const r = c.interrupted;
      out.push(h("div", { class: "ld-callout ld-callout--warn", "data-testid": "interrupted" },
        h("p", null, h("b", null, "A write was interrupted. "), `${r.op} stopped (${r.error}) before it could check or roll back. `
          + "The OP-Z keeps a file only when it arrived whole."),
        h("div", { class: "ld-row" },
          h("button", { type: "button", class: "btn btn--small", disabled: !c.connected || !!busy, onclick: () => void this.restore(r.backupId) },
            "Check and put the backup back…"),
          h("button", { type: "button", class: "btn btn--small btn--ghost", onclick: () => c.setInterrupted(null) }, "Dismiss"))));
    }
    if (this.notice) {
      out.push(h("p", { class: `ld-callout ld-callout--${this.notice.kind}`, role: "status" }, this.notice.text, " ",
        h("button", { type: "button", class: "ld-x", "aria-label": "Dismiss", onclick: () => { this.notice = null; this.render(); } }, "×")));
    }
    if (this.catalogueError) {
      out.push(h("p", { class: "ld-callout ld-callout--error" }, `The engine catalogue didn't load (${this.catalogueError}). Reload the page.`));
    }
    const current = c ? this.current() : 1;
    const bodies = [() => this.stepPick(), () => this.stepConnect(), () => this.stepInstall(), () => this.stepContent(), () => this.stepEject()];
    out.push(h("ol", { class: "ld-steps" }, STEPS.map((title, i) => {
      const n = i + 1;
      const state = n < current ? "done" : n === current ? "current" : "todo";
      const isOpen = this.open === n;
      return h("li", { class: `ld-step is-${state} ${isOpen ? "is-open" : ""}`, "data-step": String(n), "aria-current": state === "current" ? "step" : null },
        h("button", { type: "button", class: "ld-step__head", "aria-expanded": String(isOpen), "data-key": `step-${n}`, onclick: () => this.setOpen(isOpen ? 0 : n) },
          h("span", { class: "ld-step__n" }, state === "done" ? "✓" : String(n)),
          h("span", { class: "ld-step__t" }, title),
          h("span", { class: "ld-step__s" }, this.stepSummary(n))),
        isOpen ? h("div", { class: "ld-step__body" }, bodies[i]()) : null);
    })));
    out.push(h("details", { class: "ld-log" }, h("summary", null, "Log"), h("pre", { class: "ld-pre" }, this.log.slice(-200).join("\n") || "Nothing yet.")));
    if (this.store.volatile) out.push(h("p", { class: "dim ld-small" }, "This browser blocks storage: backups last only while the page is open."));
    return out;
  }

  private stepSummary(n: number): string {
    const c = this.controller;
    if (!this.choice) return "";
    switch (n) {
      case 1: return GROUPS.map((g) => `${this.choice![g.id].source === "original" ? "orig." : this.choice![g.id].picks.length} ${g.title.toLowerCase()}`).join(" · ");
      case 2: return c?.connected ? (this.first ? "backed up" : "connected") : "";
      case 3: return this.lastInstall?.ok ? "verified" : "";
      case 4: return this.lastBake?.ok ? "presets baked" : "";
      default: return "";
    }
  }

  // step 1 -------------------------------------------------------------------------------------------------------

  private stepPick(): Child[] {
    if (!this.choice) return [h("p", { class: "dim" }, "Loading the engines…")];
    const g = group(this.groupId);
    const c = this.choice[g.id];
    const blocked = this.blocked(g.id);
    const cap = capacity(c, blocked);
    const list = groupEngines(this.engines, g.id);
    const midi = this.controller?.kind === "midi";
    const tabs = h("div", { class: "ld-tabs", role: "tablist", "aria-label": "Track group" }, GROUPS.map((x) => h("button", {
      type: "button", role: "tab", class: "ld-tab", "aria-selected": String(x.id === g.id), "data-key": `tab-${x.id}`,
      onclick: () => { this.groupId = x.id; write(KEYS.group, x.id); this.render(); },
    }, x.title, h("small", null, this.choice![x.id].source === "original" ? "orig." : `${this.choice![x.id].picks.length}/${MAX_PICKS}`))));
    const tracks = g.tracks.map((t) => TRACK_LABELS[t]).join(" · ");
    const source = h("div", { class: "ld-seg", role: "radiogroup", "aria-label": "Sounds for this group" }, (["community", "original"] as const).map((s) => h("button", {
      type: "button", role: "radio", class: "ld-seg__b", "aria-checked": String(c.source === s), "data-key": `src-${s}`,
      onclick: () => this.setChoice({ ...this.choice!, [g.id]: setSource(c, s) }),
    }, s === "community" ? "Community" : "Original")));
    const out: Child[] = [tabs, h("p", { class: "ld-small dim" }, tracks), source, this.slotStrip()];
    if (c.source === "original") {
      out.push(h("p", { class: "ld-note" }, `Every slot on the ${tracks} tracks gets ${this.controller?.connected ? "the plug it had before (from your first backup)" : "your OP-Z's own sound back"}: `
        + "teenage engineering's sounds are already on your OP-Z, so nothing is uploaded. Your ticks are kept for when you switch back."));
    } else {
      const left = SLOTS_PER_TRACK - c.picks.length - c.original.length - blocked.length;
      out.push(h("p", { class: "ld-count", "data-testid": "pick-count" }, `${c.picks.length} of ${cap} ticked`,
        h("span", { class: "dim" }, ` · ${left > 1 ? `${left} slots stay as they are` : left === 1 ? "1 slot stays as it is" : "no slot left as it is"}`)));
    }
    out.push(h("ul", { class: `ld-engines ${c.source === "original" ? "is-muted" : ""}`, "data-testid": "engine-list" }, list.map((e) => {
      const on = c.picks.includes(e.dir);
      const slotNo = on ? [...slotIntents(c, blocked)].find(([, i]) => i.kind === "engine" && i.dir === e.dir)?.[0] : undefined;
      const unbuilt = e.engine === null;
      return h("li", { class: `ld-engine ${on ? "is-on" : ""}` },
        h("label", { class: "ld-engine__pick" },
          h("input", {
            type: "checkbox", checked: on, disabled: c.source === "original", "data-key": `pick-${e.dir}`,
            onchange: (ev: Event) => {
              const box = ev.target as HTMLInputElement;
              const edit = togglePick(c, e.dir, box.checked, blocked);
              this.notice = edit.refused ? { text: edit.refused, kind: "warn" } : null;
              if (!edit.refused) this.tellLab(e.dir, box.checked);
              this.setChoice({ ...this.choice!, [g.id]: edit.choice });
            },
          }),
          h("span", { class: "ld-engine__name" }, e.label,
            slotNo ? h("small", { class: "ld-engine__slot", title: `slot ${slotNo} (black key ${slotNo % 10})` }, `slot ${slotNo}`) : null,
            unbuilt ? h("small", { class: "ld-tag", title: "The catalogue has no built .engine for it" }, midi ? "not built" : "not built · demo only") : null,
            engineTracks(e, g).length < g.tracks.length ? h("small", { class: "ld-tag" }, engineTracks(e, g).map((t) => TRACK_LABELS[t]).join("+") + " only") : null),
          e.summary ? h("span", { class: "ld-engine__sum" }, e.summary) : null,
          cautionOf(e) ? h("span", { class: "ld-engine__caution", "data-testid": `caution-${e.dir}` },
            `${cautionOf(e)} ${midi ? "It can't be installed on your OP-Z." : "Demo only."}`) : null),
        h("button", {
          type: "button", class: "ld-listen", "data-key": `listen-${e.dir}`, "aria-label": `Listen to ${e.label}`,
          onclick: () => document.dispatchEvent(new CustomEvent("opfree:audition", { detail: { dir: e.dir } }) as AuditionEvent),
        }, "Listen"));
    })));
    if (!list.length) out.push(h("p", { class: "dim" }, "No engines for this group in the catalogue yet."));
    out.push(h("div", { class: "ld-row ld-row--end" }, h("button", { type: "button", class: "btn btn--primary btn--small", onclick: () => this.setOpen(this.controller?.connected ? 3 : 2) },
      this.controller?.connected ? "Next: install →" : "Next: connect →")));
    return out;
  }

  /** Slots 1…10 of the group: what each gets. Click a slot to keep it original (community only). */
  private slotStrip(): Child {
    const g = group(this.groupId);
    const c = this.choice![g.id];
    const blocked = this.blocked(g.id);
    const intents = slotIntents(c, blocked);
    const d = this.controller?.device;
    const orig = this.first ? backupSlots(this.first) : null;
    const plan = this.plan();
    const originalName = (s: number) => {
      if (!orig || !d) return "your OP-Z's own sound in this slot";
      const names = [...new Set(g.tracks.map((t) => {
        const id = (orig.tracks ?? []).find((x: { index: number }) => x.index === t)?.packs.find((p: { slot: number }) => p.slot === s)?.id ?? null;
        return plugLabel(d.plugs, id, this.engines);
      }))];
      return names.join(", ");
    };
    const skippedHere = plan?.skipped.filter((x) => g.tracks.includes(x.track)) ?? [];
    return h("div", { class: "ld-strip-wrap" },
      h("ol", { class: "ld-strip", "aria-label": "Slots 1 to 10 (black keys)" }, [...intents].map(([s, i]) => {
        const e = i.kind === "engine" ? this.engines.find((x) => x.dir === i.dir) : null;
        const title = i.kind === "engine" ? `slot ${s}: ${e?.label ?? i.dir}`
          : i.kind === "original" ? `slot ${s}: original — ${originalName(s)}`
            : i.kind === "blocked" ? `slot ${s}: your samples — never touched`
              : `slot ${s}: stays as it is`;
        const clickable = c.source === "community" && i.kind !== "blocked";
        return h("li", null, h("button", {
          type: "button", class: `ld-slot is-${i.kind}`, title, "aria-label": title + (clickable ? (i.kind === "original" ? ". Click to give it to the picks." : ". Click to keep it original.") : ""),
          disabled: !clickable, "data-key": `slot-${s}`,
          onclick: () => {
            const edit = toggleOriginalSlot(c, s, blocked);
            this.notice = edit.refused ? { text: edit.refused, kind: "warn" } : null;
            this.setChoice({ ...this.choice!, [g.id]: edit.choice });
          },
        }, h("span", { class: "ld-slot__n" }, String(s % 10)),
        h("span", { class: "ld-slot__v" }, i.kind === "engine" ? (e?.label ?? i.dir).slice(0, 6) : i.kind === "original" ? "orig" : i.kind === "blocked" ? "yours" : "—")));
      })),
      h("p", { class: "ld-small dim" }, "Black keys 1…0. ", c.source === "community" ? "Tap a slot to keep it original. " : "",
        d ? (skippedHere.length
          ? `Skipped, your samples: ${skippedHere.map((x) => `${TRACK_LABELS[x.track]} ${x.slot}`).join(", ")}.`
          : "Slots with your own samples are never touched.")
          : "Slots holding your own samples are never touched (checked when you connect)."));
  }

  // step 2 -------------------------------------------------------------------------------------------------------

  private stepConnect(): Child[] {
    const c = this.controller;
    if (!c) return [];
    if (c.connected) {
      const d = c.device;
      const plugs = d?.plugs.plugs ?? [];
      const ours = plugs.filter((p: { id: number }) => this.engines.some((e) => e.manifest.plug.id === p.id)).length;
      const samples = plugs.filter(isUserSample).length;
      return [
        h("p", null, c.kind === "demo" ? "Connected to the demo OP-Z: it lives in this page, nothing is sent anywhere." : `Connected over USB to ${c.deviceName}.`),
        d ? h("ul", { class: "ld-facts" },
          h("li", null, `${plugs.length} plugs installed, ${ours} of them from this site`),
          h("li", null, `${samples} sample${samples === 1 ? "" : "s"} you made on the OP-Z: their slots are never touched`)) : null,
        this.first ? h("p", { class: "ld-ok", "data-testid": "first-backup" }, "Backed up. First backup in this browser: ", h("code", null, this.first.id),
          h("span", { class: "dim" }, " — what “original” and Restore go back to.")) : h("p", { class: "dim" }, "Backing up…"),
        h("div", { class: "ld-row" },
          h("button", { type: "button", class: "btn btn--small btn--ghost", disabled: !!c.busy, onclick: () => void c.backup().then(() => this.reloadBackups()).then(() => this.render()) }, "Back up again"),
          h("button", { type: "button", class: "btn btn--small btn--ghost", disabled: !!c.busy, onclick: () => this.disconnect() }, "Disconnect"),
          c.kind === "demo" ? h("button", { type: "button", class: "btn btn--small btn--ghost", disabled: !!c.busy, onclick: () => this.resetDemo() }, "Reset the demo") : null),
      ];
    }
    const why = midiUnavailable();
    return [
      h("p", { class: "ld-callout ld-callout--warn" }, h("b", null, "Quit teenage engineering's OP-Z app first. "),
        "Two programs talking to the OP-Z at once break both transfers."),
      h("ol", { class: "ld-howto" },
        h("li", null, "Connect the OP-Z with a USB cable and switch it on (normal mode)."),
        h("li", null, "Press Connect and allow MIDI devices with SysEx when the browser asks."),
        h("li", null, "The site reads your OP-Z's settings and backs them up in this browser before anything else.")),
      why ? h("p", { class: "ld-callout ld-callout--info", "data-testid": "midi-unavailable" }, why) : null,
      this.connectError ? h("p", { class: "ld-callout ld-callout--error", "data-testid": "connect-error" }, this.connectError) : null,
      h("div", { class: "ld-row" },
        h("button", { type: "button", class: "btn btn--primary btn--small", disabled: !!why, "data-key": "connect-midi", onclick: () => void this.connectMidi() }, "Connect over USB"),
        h("button", { type: "button", class: "btn btn--small", "data-key": "connect-demo", "data-testid": "connect-demo", onclick: () => void this.connectDemo() }, "Try the demo OP-Z")),
      h("p", { class: "ld-small dim" }, "Web MIDI with SysEx: Chrome or Edge on a computer, over https. The demo runs every step without a device."),
    ];
  }

  private resetDemo(): void {
    if (this.controller.busy) return;
    this.controller.disconnect();
    resetDemoFiles();
    this.demoDisk = new DemoDisk();
    this.lastInstall = null;
    this.lastBake = null;
    void this.connectDemo();
  }

  // step 3 -------------------------------------------------------------------------------------------------------

  private stepInstall(): Child[] {
    const c = this.controller;
    if (!c?.connected) return [h("p", { class: "dim" }, "Connect first (step 2): the plan is made from your OP-Z's own settings.")];
    const plan = this.plan();
    const out: Child[] = [];
    if (plan) {
      const work = plan.installs.filter((i) => i.action !== "ready");
      out.push(h("ul", { class: "ld-facts", "data-testid": "plan-summary" },
        h("li", null, `${work.length} engine${work.length === 1 ? "" : "s"} to send`, work.length ? h("span", { class: "dim" }, ` (${work.map((i) => i.label).join(", ")})`) : null),
        h("li", null, `${plan.changes.length} slot${plan.changes.length === 1 ? "" : "s"} change`),
        plan.skipped.length ? h("li", null, `${plan.skipped.length} slot${plan.skipped.length === 1 ? "" : "s"} skipped: your samples`) : null,
        plan.installs.some((i) => i.restart) ? h("li", { class: "ld-warn" }, "restart needed afterwards (an engine is upgraded)") : null));
      for (const e of plan.errors) out.push(h("p", { class: "ld-callout ld-callout--error" }, e));
      for (const w of plan.warnings.slice(0, 4)) out.push(h("p", { class: "ld-small ld-warn" }, w));
    }
    if (this.lastInstall?.ok) {
      out.push(h("p", { class: "ld-ok", "data-testid": "install-ok" }, "Installed and verified by reading back."
        + (this.lastInstall.restart ? " Restart the OP-Z before you play: it keeps the old engine code until then." : "")));
    }
    out.push(h("p", { class: "ld-small dim" }, "One sheet shows every slot before → after, then each engine goes through the same backup, check and "
      + "read-back as the command-line tools. Pushed presets stay until the OP-Z restarts; step 4 keeps them for good."));
    out.push(h("div", { class: "ld-row" }, h("button", {
      type: "button", class: "btn btn--primary btn--small", "data-testid": "install", disabled: !!c.busy || !plan || plan.errors.length > 0,
      onclick: () => void this.install(),
    }, "Review and install…")));
    return out;
  }

  // step 4 -------------------------------------------------------------------------------------------------------

  private stepContent(): Child[] {
    const c = this.controller;
    const gate = this.gate();
    const out: Child[] = [
      h("p", null, "Content mode turns the OP-Z into a USB disk. Your picks' presets go into its own preset store "
        + "(config/userPresets.dat), so they survive every restart. Engines can't go in this way (content mode has no "
        + "engine folder): step 3 installed them."),
    ];
    out.push(this.gateView(gate));
    const demo = c?.kind === "demo";
    out.push(h("ol", { class: "ld-howto" },
      demo ? h("li", null, "Demo: the demo OP-Z's disk stands in for content mode; nothing to switch.")
        : [h("li", null, "Disconnect here, then switch the OP-Z off."),
          h("li", null, "Hold the track key while you switch it on (content mode, manual p.61). It shows up as a disk named OP-Z."),
          h("li", null, "Choose that disk below.")]));
    const engines = this.bakeEngines();
    const picker = diskPicker();
    out.push(h("label", { class: "ld-check" }, h("input", {
      type: "checkbox", checked: this.overwriteUser, onchange: (e: Event) => { this.overwriteUser = (e.target as HTMLInputElement).checked; },
    }), h("span", null, "Also replace presets I stored by hand on the same keys")));
    if (!demo && !picker) {
      out.push(h("p", { class: "ld-callout ld-callout--info" }, "Choosing the disk needs Chrome or Edge on a computer (the folder picker)."));
    }
    if (this.lastBake) out.push(h("p", { class: this.lastBake.ok ? "ld-ok" : "ld-callout ld-callout--error", "data-testid": "bake-result" }, this.lastBake.text));
    out.push(h("div", { class: "ld-row" }, h("button", {
      type: "button", class: "btn btn--primary btn--small", "data-testid": "bake",
      disabled: !!c?.busy || !gateOpen(gate) || !engines.length || (!demo && !picker),
      onclick: () => void this.disk({ kind: "bake", engines, overwriteUser: this.overwriteUser }),
    }, demo ? "Bake onto the demo disk…" : "Choose the OP-Z disk and bake…")));
    out.push(h("p", { class: "ld-small dim" }, `${engines.length} engine${engines.length === 1 ? "" : "s"} with presets picked. `
      + "Proven on hardware with the command-line tool; from the browser not yet tested on hardware."));
    return out;
  }

  private gateView(g: SampleGate): Child {
    const c = this.controller;
    switch (g.kind) {
      case "blocked":
        return h("div", { class: "ld-callout ld-callout--error", "data-testid": "gate-blocked" },
          h("p", null, h("b", null, "Stop: content mode would delete your samples. "), looseText(g.check).join(", "),
            g.check.loose.length === 1 ? " is in no sound slot." : " are in no sound slot."),
          h("p", null, "Put each one back in a slot first (Restore in step 5 puts back a backup's layout that had it), then check again."),
          h("button", { type: "button", class: "btn btn--small", disabled: !!c?.busy, onclick: () => void this.refresh() }, "Check again"));
      case "checked":
        return h("p", { class: "ld-ok", "data-testid": "gate-checked" }, `Sample check passed: all ${g.check.total} sample${g.check.total === 1 ? "" : "s"} you made on the OP-Z `
          + `${g.check.total === 1 ? "is" : "are"} in a slot${g.live ? "" : ` (checked ${formatTime(g.check.at)})`}.`);
      case "needs-read":
        return h("p", null, "Reading your OP-Z to check your samples… ", h("button", { type: "button", class: "btn btn--small", onclick: () => void this.refresh() }, "Read now"));
      default:
        return h("div", { class: "ld-callout ld-callout--warn", "data-testid": "gate-confirm" },
          h("p", null, h("b", null, "Your samples. "), "When the OP-Z leaves content mode it deletes every sample you made on it that "
            + "sits in no sound slot. Connect over USB in normal mode first and the site checks it for you; otherwise confirm:"),
          h("label", { class: "ld-check" }, h("input", {
            type: "checkbox", checked: g.kind === "confirmed", "data-key": "gate-confirm",
            onchange: (e: Event) => { this.sampleConfirmed = (e.target as HTMLInputElement).checked; this.render(); },
          }), h("span", null, "Every sample I made on the OP-Z is in a sound slot")));
    }
  }

  // step 5 -------------------------------------------------------------------------------------------------------

  private stepEject(): Child[] {
    const c = this.controller;
    const connected = !!c?.connected;
    const settings = this.backups.filter(isSettingsBackup).filter((b) => !connected || b.source === this.store.source);
    // the loader's own backups first; every core step's backup (install-…, presets-…) folded away
    const main = settings.filter((b) => MAIN_BACKUPS.has(b.label) || b.id === this.first?.id);
    const rest = settings.filter((b) => !main.includes(b));
    const row = (b: BackupRecord) => h("li", null,
      h("span", null, formatTime(b.created), h("small", { class: "dim" }, ` ${b.label}${b.id === this.first?.id ? " · first" : ""}`)),
      h("button", { type: "button", class: "ld-link", disabled: !connected || !!c?.busy, onclick: () => void this.restore(b.id) }, "Restore…"));
    const presets = this.backups.filter(isUserPresetsBackup);
    return [
      h("ol", { class: "ld-howto" },
        h("li", null, "Eject the OP-Z disk (Finder: ⏏; Windows: Safely remove). The OP-Z imports the presets and restarts."),
        h("li", null, "Pick a sound: hold a track key and tap black key 1…0 for slot 1…10 (manual p.25)."),
        h("li", null, "Play. If a track goes silent or the OP-Z restarts, come back here and restore.")),
      h("p", { class: "label" }, "Restore"),
      this.first && connected ? h("button", { type: "button", class: "btn btn--small", "data-testid": "restore-original", disabled: !!c.busy, onclick: () => void this.restore(this.first!.id) },
        "Restore original layout…") : h("p", { class: "ld-small dim" }, "Connect (step 2) to restore a slot layout."),
      main.length ? h("ul", { class: "ld-backups", "data-testid": "backups" }, main.slice(0, 10).map(row)) : null,
      rest.length ? h("details", { class: "ld-details" }, h("summary", null, `Every step's backup (${rest.length})`),
        h("ul", { class: "ld-backups" }, rest.slice(0, 40).map(row))) : null,
      presets.length ? [h("p", { class: "label" }, "Preset store backups (content mode)"),
        h("ul", { class: "ld-backups" }, presets.slice(0, 6).map((b) => h("li", null,
          h("span", null, formatTime(b.created), h("small", { class: "dim" }, ` ${b.source}`)),
          h("button", { type: "button", class: "ld-link", disabled: !!c?.busy, onclick: () => void this.disk({ kind: "restore", data: b.files["userPresets.dat"], source: b.id }) }, "Put back on the disk…"))))] : null,
      h("p", { class: "ld-small dim" }, "A restore writes only the slot map: plugs installed since stay installed. Slots holding your samples stay as they are."),
    ];
  }
}

/** Stand-in bytes for an engine that is not built, for the demo only (the demo OP-Z never runs it). */
function demoPlaceholder(dir: string): Uint8Array {
  const bytes = new TextEncoder().encode(`DEMO PLACEHOLDER for ${dir}: not a real engine.\n`);
  const out = new Uint8Array(2048);
  for (let i = 0; i < out.length; i++) out[i] = bytes[i % bytes.length];
  return out;
}

export function mountLoader(el: HTMLElement): Mounted {
  const panel = new LoaderPanel(el);
  return { destroy: () => panel.destroy() };
}
