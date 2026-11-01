/**
 * The web simulator page: a synth slot and two effect slots (FX1, FX2) played together. The keyboard (computer keys,
 * on-screen, Web MIDI) plays the synth; the synth track's FX1 and FX2 sends feed the effects and crossfade its dry
 * path (src/mixer.ts); each slot has its engine, presets and knobs. Each engine's scenarios run on this thread
 * through opzsim's own main (src/opzsim.ts). Audio runs in an AudioWorklet (src/worklet.ts) at 44.1 kHz.
 *
 * Everything is our own host model (sim/src): no TE code or tables.
 */

import type { CatalogEntry, Kind } from "../catalog.ts";
import { Keyboard, connectMidi, noteName } from "./keyboard.ts";
import { type Knob, knob } from "./knob.ts";
import { FX_SLOTS, type FromWorklet, type FxSlot, type SlotName, type ToWorklet } from "./messages.ts";
import { type LiveStats, parseExpectations, problems, runScenario, scenarioDependencies } from "./opzsim.ts";
import { Scope } from "./scope.ts";

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;

const NAMES = ["P1", "P2", "attack", "decay", "sustain", "release", "FX1 send", "FX2 send", "filter", "resonance", "pan",
  "level", "portamento", "lfo depth", "lfo speed", "lfo target", "lfo shape", "note style"];
/** On an FX track some bytes mean something else (engine-abi.md, plug-format.md). */
const FX_NAMES: Record<number, string> = { 10: "pan · dry/wet", 12: "dry level" };
/** opzsim's scenario defaults (sim/README.md): sustain 255, pan 128, level 200, filter 255, the rest 0. */
const DEFAULTS = [0, 0, 0, 0, 255, 0, 0, 0, 255, 0, 128, 200, 0, 0, 0, 0, 0, 0];
const TRACK_LABEL = ["kick", "snare", "perc", "sample", "bass", "lead", "arp", "chord", "FX1", "FX2", "tape", "master"];
const FX_TRACK: Record<FxSlot, number> = { fx1: 8, fx2: 9 };
const SEND_BYTE: Record<FxSlot, number> = { fx1: 6, fx2: 7 };   // the synth track's send to each FX track
const SENDS = [6, 7];
const DRY_LEVEL = 12;
const FIRST_SEND = 128;       // the send an effect starts with when the synth's is 0, so the effect is heard

const state = {
  catalog: [] as CatalogEntry[],
  ctx: null as AudioContext | null,
  node: null as AudioWorkletNode | null,
  analyser: null as AnalyserNode | null,
  volume: null as GainNode | null,
  modules: new Map<string, Promise<ArrayBuffer>>(),
  lastStats: null as Extract<FromWorklet, { type: "stats" }> | null,
  log: "",
  fineClock: false,
};

const scope = new Scope($<HTMLCanvasElement>("scope"));

function post(m: ToWorklet, transfer: Transferable[] = []): void {
  state.node?.port.postMessage(m, transfer);
}

function wasmBytes(url: string): Promise<ArrayBuffer> {
  let p = state.modules.get(url);
  if (!p) {
    p = fetch(url).then((r) => {
      if (!r.ok) throw new Error(`${url}: ${r.status}`);
      return r.arrayBuffer();
    });
    state.modules.set(url, p);
  }
  return p;
}

// ---- a slot: engine, track, presets, knobs ---------------------------------------------------------------------

class Slot {
  entry: CatalogEntry | null = null;
  track: number;
  values = DEFAULTS.slice();
  knobs: Knob[] = [];
  loaded = false;
  readonly name: SlotName;
  readonly root: HTMLElement;
  readonly kinds: Kind[];
  /** a byte changed here (knob or preset) */
  onByte: (index: number, value: number) => void = () => {};

  constructor(name: SlotName, kinds: Kind[], mainKnobs: number[]) {
    this.name = name;
    this.kinds = kinds;
    this.root = $(`slot-${name}`);
    this.track = name === "synth" ? 5 : FX_TRACK[name];
    if (name !== "synth") {                                  // an effect section: cloned from the page's template
      this.root.append(($<HTMLTemplateElement>("fx-template")).content.cloneNode(true));
      this.q(".slot-title").textContent = `Effect on ${TRACK_LABEL[this.track]}`;
    }
    this.q(".slot-engine").dataset.testid = `${name}-engine`;
    for (let i = 0; i < 18; i++) {
      const label = (name !== "synth" && FX_NAMES[i]) || NAMES[i];
      const k = knob(label, i, DEFAULTS[i], DEFAULTS[i], (v) => this.setByte(i, v, false));
      k.el.dataset.slot = name;
      this.knobs.push(k);
      this.q(mainKnobs.includes(i) ? ".main-knobs" : ".more-knobs").append(k.el);
    }
    this.q<HTMLSelectElement>(".slot-engine").addEventListener("change", (e) => void this.select((e.target as HTMLSelectElement).value));
    this.q<HTMLSelectElement>(".slot-track").addEventListener("change", (e) => {
      this.track = Number((e.target as HTMLSelectElement).value);
      this.changedTrack();
      void this.load();
    });
  }

  q<T extends HTMLElement = HTMLElement>(sel: string): T {
    return this.root.querySelector(sel) as T;
  }

  summary() {
    return { id: this.entry?.id ?? "none", loaded: this.loaded, track: this.track, values: this.values.slice() };
  }

  /** Set one byte: the knob, the engine, and whoever listens (the send knobs). */
  setByte(index: number, value: number, immediate: boolean): void {
    this.values[index] = value;
    this.knobs[index].set(value);
    post({ type: "param", slot: this.name, index, value, immediate });
    this.onByte(index, value);
  }

  /** Load a preset's bytes, except `keep` (the synth's sends to effects that are on). */
  setValues(values: number[], immediate: boolean, keep: number[] = []): void {
    for (let i = 0; i < 18; i++) if (!keep.includes(i)) this.setByte(i, values[i] ?? DEFAULTS[i], immediate);
  }

  private keep(): number[] {
    return this.name === "synth" ? effects.filter((e) => e.entry).map((e) => SEND_BYTE[e.name]) : [];
  }

  /** The engines this slot can take: an effect section only those whose trackmask has its FX track. */
  private fits(e: CatalogEntry): boolean {
    return this.kinds.includes(e.kind) && (this.name === "synth" || !!(e.plug.plug.trackmask & (1 << this.track)));
  }

  renderList(): void {
    const sel = this.q<HTMLSelectElement>(".slot-engine");
    sel.textContent = "";
    sel.append(new Option("none", "none"));
    const groups: Array<[string, (e: CatalogEntry) => boolean]> = this.name !== "synth"
      ? [["SDK template", (e) => e.template], ["Effects", (e) => !e.template]]
      : [["SDK templates", (e) => e.template], ["Synths", (e) => !e.template && e.kind === "synth"],
        ["Drums", (e) => !e.template && e.kind === "drum"]];
    for (const [label, keep] of groups) {
      const g = document.createElement("optgroup");
      g.label = label;
      for (const e of state.catalog.filter((x) => this.fits(x) && keep(x))) {
        const o = new Option(e.label + (e.wasm ? "" : " (not built)"), e.id);
        o.disabled = !e.wasm;
        g.append(o);
      }
      sel.append(g);
    }
    if (this.name !== "synth") {
      const others = state.catalog.filter((e) => e.kind === "fx" && !this.fits(e)).map((e) => e.label);
      if (others.length) sel.title = `Not on ${TRACK_LABEL[this.track]} (trackmask): ${others.join(", ")}`;
    }
  }

  async select(id: string): Promise<void> {
    const entry = state.catalog.find((e) => e.id === id && this.fits(e)) ?? null;
    this.entry = entry?.wasm ? entry : null;
    this.q<HTMLSelectElement>(".slot-engine").value = this.entry?.id ?? "none";
    this.root.classList.toggle("empty", !this.entry);
    this.renderInfo();
    this.renderTracks();
    this.renderPresets();
    const names = this.entry?.plug.plug.parameter_names ?? [];
    this.knobs[0].rename(names[0] ? `P1 ${names[0]}` : "P1");
    this.knobs[1].rename(names[1] ? `P2 ${names[1]}` : "P2");
    const keep = this.keep();
    const first = this.entry?.presets[0]?.values ?? DEFAULTS;
    for (let i = 0; i < 18; i++) {
      if (keep.includes(i)) continue;
      this.values[i] = first[i] ?? DEFAULTS[i];
      this.knobs[i].set(this.values[i]);
    }
    updateUrl();
    renderScenarios();
    slotChanged();
    await this.load();
  }

  private renderInfo(): void {
    const e = this.entry;
    const info = this.q(".slot-info");
    this.q(".readme pre").textContent = e?.readme ?? "";
    if (!e) {
      info.innerHTML = `<p class="hint">${this.name !== "synth" ? `Nothing on ${TRACK_LABEL[this.track]}.` : "No synth: the effects run on their generators; keys go to an effect's onNoteOn."}</p>`;
      setText(`st-size-${this.name}`, "–");
      return;
    }
    const p = e.plug;
    const kind = { synth: "synth", drum: "drum", fx: "effect" }[e.kind];
    info.innerHTML = `<span class="badge ${e.kind}">${kind}</span> <span class="hint">${p.plug.name} · plug ${p.plug.id || "–"} ·
      slot ${p.memoryslot || "– (template)"} · <code>${e.dir}/</code></span>`;
    const slot = p.memoryslot, cls = slot <= 7 ? 50 : slot <= 57 ? 10 : 5;
    $(`st-size-${this.name}`).innerHTML = slot
      ? `not measured here: <code>tools/opz_build.py ${e.dir}</code> fits slot ${slot} (${cls} KB)`
      : "not measured; a template gets its slot from <code>opz.py new</code>";
  }

  private renderTracks(): void {
    if (this.name !== "synth") return;                       // an effect section is fixed to its FX track
    const sel = this.q<HTMLSelectElement>(".slot-track");
    sel.textContent = "";
    sel.hidden = !this.entry;
    if (!this.entry) return;
    const tracks = [0, 1, 2, 3, 4, 5, 6, 7].filter((t) => this.entry!.plug.plug.trackmask & (1 << t));
    for (const t of tracks) sel.append(new Option(`${t + 1} · ${TRACK_LABEL[t]}`, String(t)));
    const preferred = this.entry.kind === "drum" ? 0 : 5;
    this.track = tracks.includes(preferred) ? preferred : tracks[0] ?? preferred;
    sel.value = String(this.track);
    this.changedTrack();
  }

  private changedTrack(): void {
    if (this.name === "synth") keyboard.configure(this.track < 4);
    slotChanged();
  }

  private renderPresets(): void {
    const box = this.q(".preset-list");
    box.textContent = "";
    if (!this.entry) return;
    const add = (name: string, values: number[], isPreset: boolean) => {
      const b = document.createElement("button");
      b.textContent = name;
      if (isPreset) b.dataset.preset = name;
      b.addEventListener("click", () => {
        box.querySelectorAll(".active").forEach((x) => x.classList.remove("active"));
        b.classList.add("active");
        this.setValues(values, true, this.keep());
      });
      box.append(b);
      return b;
    };
    add("init", DEFAULTS, false).title = "opzsim's defaults: sustain 255, pan 128, level 200, filter 255, the rest 0";
    for (const p of this.entry.presets) add(p.name, p.values, true);
    (box.querySelector("[data-preset]") ?? box.querySelector("button"))?.classList.add("active");
  }

  /** (Re)open this slot's engine in the worklet; its bytes follow once it is loaded. */
  async load(): Promise<void> {
    this.loaded = false;
    document.body.dataset[`${this.name}Loaded`] = "";
    if (!state.node) return;                  // audio not started: loaded when it is
    const bytes = this.entry?.wasm ? (await wasmBytes(this.entry.wasm)).slice(0) : null;
    post({ type: "load", slot: this.name, bytes, track: this.track }, bytes ? [bytes] : []);
  }

  afterLoad(m: Extract<FromWorklet, { type: "loaded" }>): void {
    this.loaded = m.ok && !m.empty;
    appendLog(m.log);
    if (this.loaded) {
      this.values.forEach((v, i) => post({ type: "param", slot: this.name, index: i, value: v, immediate: true }));
      post({ type: "tempo", bpm: Number($<HTMLInputElement>("tempo").value) || 120 });
      if (this instanceof Effect) this.sendSetup();
    }
    document.body.dataset[`${this.name}Loaded`] = m.ok ? (this.entry?.id ?? "none") : "failed";
  }
}

// ---- an effect section: its send from the synth, dry level and input ------------------------------------------

class Effect extends Slot {
  declare readonly name: FxSlot;
  readonly sendKnob: Knob;
  readonly dryKnob: Knob;
  readonly source: HTMLSelectElement;
  /** a generator picked by hand stays until the user picks the send again */
  private manualSource = false;

  constructor(name: FxSlot) {
    super(name, ["fx"], [0, 1, 10, 11, 8, 9]);
    const sendByte = SEND_BYTE[name];
    this.sendKnob = knob(`Send to ${TRACK_LABEL[this.track]} (synth byte ${sendByte})`, sendByte, 0, 0,
      (v) => synth.setByte(sendByte, v, false));
    this.dryKnob = knob("Dry level", DRY_LEVEL, 0, 0, (v) => this.setByte(DRY_LEVEL, v, false));
    for (const [k, param] of [[this.sendKnob, `${name}-send`], [this.dryKnob, `${name}-dry`]] as const) {
      k.el.classList.add("big");
      k.el.querySelector<HTMLElement>(".knob-dial")!.dataset.param = param;
      this.q(".send-knobs").append(k.el);
    }
    this.onByte = (i, v) => {
      if (i === DRY_LEVEL) this.dryKnob.set(v);
    };
    this.source = this.q<HTMLSelectElement>(".source");
    this.source.dataset.testid = `${name}-source`;
    this.q(".routing").dataset.testid = `${name}-routing`;
    this.source.addEventListener("change", () => {
      this.manualSource = this.source.value !== "send";
      this.sendInput();
      slotChanged();
    });
    for (const c of [".source-f0", ".source-f1", ".source-amp"]) this.q(c).addEventListener("input", () => this.sendInput());
    this.q(".fx-gain").addEventListener("input", () => this.sendGain());
  }

  sendInput(): void {
    const kind = this.source.value;
    const n = (c: string) => Number(this.q<HTMLInputElement>(c).value);
    // impulses: f0 is "every N steps" in opzsim's `input impulse`; use 1 step
    post({ type: "input", slot: this.name, kind, f0: kind === "impulse" ? 1 : n(".source-f0") || 110,
      f1: n(".source-f1") || 4000, amp: n(".source-amp") / 100 });
  }

  private sendGain(): void {
    post({ type: "gain", slot: this.name, gain: Number(this.q<HTMLInputElement>(".fx-gain").value) / 100 });
  }

  sendSetup(): void {
    this.sendInput();
    this.sendGain();
  }

  /** After a slot changes: the input, the knobs, and a send that lets this effect be heard. */
  refresh(): void {
    const src = this.source;
    (src.querySelector('option[value="send"]') as HTMLOptionElement).disabled = !synth.entry;
    if (!synth.entry && src.value === "send") src.value = "saw";
    if (synth.entry && !this.manualSource) src.value = "send";
    const fed = !!synth.entry && !!this.entry && src.value === "send";
    const sendByte = SEND_BYTE[this.name];
    if (fed && synth.values[sendByte] === 0) synth.setByte(sendByte, FIRST_SEND, true);
    this.sendKnob.set(synth.values[sendByte]);
    this.dryKnob.set(this.values[DRY_LEVEL]);
    this.q(".routing").hidden = !this.entry;
    this.q(".send-knobs").hidden = !fed;
    this.q(".source-generator").hidden = src.value === "send";
    this.q(".source-f1-label").hidden = src.value !== "sweep";
    this.q(".routing-note").textContent = src.value !== "send"
      ? `${TRACK_LABEL[this.track]} runs on the generator; the synth's send to it is ignored.`
      : "The send crossfades: turning it up moves the synth from the dry path into this effect. Dry level 255 keeps " +
        "the dry path as well (send/return); at 0 the send is a crossfade.";
  }
}

// ---- keyboard -------------------------------------------------------------------------------------------------

const keyboard = new Keyboard(
  (on, note, velocity) => post({ type: "note", slot: $<HTMLSelectElement>("keys-target").value as SlotName, on, note, velocity }),
  (first, last) => void ($("octave").textContent = `Keys A–' play ${noteName(first)}–${noteName(last)} · Z/X octave`),
);
$("keyboard").append(keyboard.el);

const synth = new Slot("synth", ["synth", "drum"], [0, 1, 2, 3, 4, 5, 8, 9, 10, 11]);
const effects = FX_SLOTS.map((name) => new Effect(name));
const slots: Slot[] = [synth, ...effects];

/** For the e2e tests and the curious: the page's state and the last level reading. */
(window as unknown as { websim: unknown }).websim = {
  state,
  slots: () => Object.fromEntries(slots.map((s) => [s.name, s.summary()])),
  level: () => scope.level,
  resetLevel: () => void (scope.level.maxPeak = -Infinity),
};

synth.onByte = (i) => {
  if (SENDS.includes(i)) for (const e of effects) e.sendKnob.set(synth.values[SEND_BYTE[e.name]]);
};

/** After any slot changes: each effect's routing, where keys go, the track note. */
function slotChanged(): void {
  for (const e of effects) e.refresh();
  const keys = $<HTMLSelectElement>("keys-target");
  for (const s of slots) (keys.querySelector(`option[value="${s.name}"]`) as HTMLOptionElement).disabled = !s.entry;
  const current = slots.find((s) => s.name === keys.value);
  if (!current?.entry) keys.value = slots.find((s) => s.entry)?.name ?? "synth";
  const t = synth.entry ? synth.track : null;
  $("track-note").textContent = t === null ? "No synth: keys go to an effect's onNoteOn."
    : t < 4 ? "Drum track: every key gets pitch 1024 (unity), the key number picks the sound. OP-Z drum keys are F3–E5."
      : `Synth on ${TRACK_LABEL[t]}: ${[2, 2, 2, 2, 2, 3, 2, 4][t]} voices, pitch from the key (A4 = 440 Hz).`;
}

function updateUrl(): void {
  history.replaceState(null, "", "?" + slots.map((s) => `${s.name}=${s.entry?.id ?? "none"}`).join("&"));
}

// ---- audio ----------------------------------------------------------------------------------------------------

async function startAudio(): Promise<void> {
  if (state.ctx) {
    if (state.ctx.state === "suspended") await state.ctx.resume();
    return;
  }
  const ctx = new AudioContext({ sampleRate: 44100, latencyHint: "interactive" });
  state.ctx = ctx;
  await ctx.audioWorklet.addModule(new URL("./worklet.ts", import.meta.url));
  const node = new AudioWorkletNode(ctx, "opzsim", { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
  const analyser = ctx.createAnalyser(), analyserR = ctx.createAnalyser();
  analyser.fftSize = analyserR.fftSize = 2048;
  const splitter = ctx.createChannelSplitter(2);
  const volume = ctx.createGain();
  volume.gain.value = Number($<HTMLInputElement>("volume").value) / 100;
  node.connect(splitter);
  splitter.connect(analyser, 0);
  splitter.connect(analyserR, 1);
  node.connect(volume).connect(ctx.destination);
  Object.assign(state, { node, analyser, volume });
  scope.attach(analyser, analyserR);
  node.port.onmessage = (e: MessageEvent<FromWorklet>) => {
    const m = e.data;
    if (m.type === "loaded") {
      state.fineClock = m.fineClock;
      slots.find((s) => s.name === m.slot)?.afterLoad(m);
    } else showStats(m);
  };
  $("audio").textContent = ctx.sampleRate === 44100 ? "Audio on · 44.1 kHz" : `Audio on · ${ctx.sampleRate} Hz (resampled)`;
  $("audio").classList.remove("primary");
  document.body.dataset.audio = "on";
  for (const s of slots) await s.load();
}

/** Audition a scenario render through the same scope and meter. */
function playBuffer(samples: Int32Array): void {
  const ctx = state.ctx;
  if (!ctx || !state.volume) return;
  const frames = samples.length / 2;
  const buf = ctx.createBuffer(2, frames, 44100);
  const l = buf.getChannelData(0), r = buf.getChannelData(1);
  for (let i = 0; i < frames; i++) {
    l[i] = samples[2 * i] / 2147483648;
    r[i] = samples[2 * i + 1] / 2147483648;
  }
  const src = ctx.createBufferSource();
  src.buffer = buf;
  src.connect(state.volume);
  const split = ctx.createChannelSplitter(2);
  src.connect(split);
  scope.connectExtra(split);
  src.start();
}

// ---- host checks ----------------------------------------------------------------------------------------------

function setText(id: string, text: string, ok?: boolean): void {
  const el = $(id);
  el.textContent = text;
  el.className = ok === undefined ? "" : ok ? "ok" : "bad";
}

function appendLog(text: string): void {
  if (!text) return;
  state.log = (state.log + text).split("\n").slice(-200).join("\n");
  $("log").textContent = state.log;
  $("log").scrollTop = $("log").scrollHeight;
}

/** Host ns per sample (per voice for an instrument), averaged since load. */
function cpu(s: LiveStats, isFx: boolean): string {
  const ns = isFx ? s.fxNs : s.instNs, blocks = isFx ? s.fxBlocks : s.voiceBlocks;
  if (!blocks) return isFx ? "–" : "– (play a note)";
  if (!ns && !state.fineClock) return "below the audio thread's ms clock so far; a scenario run measures it (not Blackfin cycles)";
  return `${(ns / (blocks * 64)).toFixed(1)} ns/sample${isFx ? "" : "/voice"} · not Blackfin cycles`;
}

function showStats(m: Extract<FromWorklet, { type: "stats" }>): void {
  state.lastStats = m;
  appendLog(m.log);
  for (const name of ["synth", ...FX_SLOTS] as const) {
    const s = m[name], isFx = name !== "synth";
    if (!s) {
      setText(`st-state-${name}`, "–");
      setText(`st-cpu-${name}`, "–");
      if (!isFx) {
        setText("st-add-synth", "–");
        setText("st-voices-synth", "–");
      }
      continue;
    }
    const budget = isFx ? "200 B per FX" : "280 B per voice";
    setText(`st-state-${name}`, s.stateViolations ? `VIOLATED ${s.stateViolations}× (see log)` : `ok · ${budget}`, !s.stateViolations);
    setText(`st-cpu-${name}`, cpu(s, isFx));
    if (!isFx) {
      setText("st-add-synth", s.addViolations ? `VIOLATED ${s.addViolations}× (see log)` : "ok", !s.addViolations);
      setText("st-voices-synth", `${s.active} of ${s.voices} active (most ${s.maxUsed}), ${s.stolen} stolen`);
    }
  }
  const pct = (x: number) => `${Math.round(x * 100)}%`;
  const fed = effects.filter((e) => m[e.name] && e.source.value === "send");
  $("st-mix").textContent = m.synth && fed.length
    ? `Mix: ${fed.map((e) => `${pct(m.sends[FX_SLOTS.indexOf(e.name)])} into ${TRACK_LABEL[e.track]}`).join(", ")}, ${pct(m.dry)} dry.`
    : "";
  const realMs = (m.blocks * 64) / 44.1;
  $("st-load").textContent = realMs > 0
    ? `Audio thread: ${((m.busyMs / realMs) * 100).toFixed(1)}% of real time since load${state.fineClock ? "" : " (ms clock, averaged)"}.`
    : "";
}

// ---- scenarios ------------------------------------------------------------------------------------------------

function renderScenarios(): void {
  const box = $("scenarios");
  box.textContent = "";
  $("scenario-result").textContent = "";
  for (const slot of slots) {
    const entry = slot.entry;
    if (!entry) continue;
    const h = document.createElement("h3");
    h.innerHTML = `<span class="dot ${slot.name === "synth" ? "synth" : "fx"}"></span>${entry.label}${slot.name === "synth" ? "" : ` <span class="hint">on ${TRACK_LABEL[slot.track]}</span>`}`;
    const ul = document.createElement("ul");
    ul.className = "scenarios";
    if (!entry.scenarios.length) ul.innerHTML = "<li class='hint'>No tests/*.scn yet.</li>";
    for (const scn of entry.scenarios) {
      const li = document.createElement("li");
      const b = document.createElement("button");
      b.textContent = "Run";
      b.dataset.scenario = `${entry.id}/${scn}`;
      b.addEventListener("click", () => void runOne(entry, scn));
      li.append(b, ` ${scn}`);
      ul.append(li);
    }
    box.append(h, ul);
  }
}

async function fetchBytes(path: string): Promise<Uint8Array | null> {
  const r = await fetch("/files" + path);
  return r.ok ? new Uint8Array(await r.arrayBuffer()) : null;
}

async function sha256(bytes: Uint8Array): Promise<string> {
  const d = await crypto.subtle.digest("SHA-256", bytes as BufferSource);
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

const EXIT: Record<number, string> = { 0: "ok", 1: "usage or I/O error", 2: "engine refused", 3: "an expectation failed", 4: "an ABI or budget rule was broken" };

async function runOne(entry: CatalogEntry, scn: string): Promise<void> {
  const box = $("scenario-result");
  box.innerHTML = `<p class="hint">Rendering ${scn}…</p>`;
  const path = `/${entry.dir}/tests/${scn}`;
  const files = new Map<string, Uint8Array>();
  const text = await fetchBytes(path);
  if (!text) return void (box.textContent = `cannot fetch ${path}`);
  files.set(path, text);
  for (const dep of scenarioDependencies(path, new TextDecoder().decode(text))) {
    const b = await fetchBytes(dep);
    if (b) files.set(dep, b);
  }
  const module = await WebAssembly.compile(await wasmBytes(entry.wasm!));
  await new Promise((r) => setTimeout(r, 0));
  const t0 = performance.now();
  const r = runScenario(module, {
    name: entry.name, scenario: path, files,
    ...(entry.template ? {} : { slot: entry.plug.memoryslot, trackmask: entry.plug.plug.trackmask }),
  });
  const ms = performance.now() - t0;
  const golden = await fetchBytes(`/${entry.dir}/tests/golden/${scn.replace(/\.scn$/, "")}.wav.sha256`);
  const want = golden ? new TextDecoder().decode(golden).split(/\s+/)[0] : null;
  const got = r.wav ? await sha256(r.wav) : null;
  const expects = parseExpectations(r.log);
  const warnings = problems(r.log);
  const times = [...r.log.matchAll(/^time\s+(.*)$/gm)].map((t) => t[1]);
  const stats = r.stats as { peak_dbfs?: number; rms_dbfs?: number; frames?: number } | null;
  const goldenLine = !want
    ? `<span class="hint">no golden file (an engine scaffolded from a template gets one from <code>make -C sim golden ENGINE=name</code>)</span>`
    : got === want
      ? `<span class="ok">matches opzsim's golden</span> <code>${got.slice(0, 12)}…</code>`
      : `<span class="bad">differs from opzsim's golden</span> (${got?.slice(0, 12)}… vs ${want.slice(0, 12)}…)`;
  box.innerHTML = `
    <h3>${entry.label} / ${scn} <span class="${r.exitCode === 0 ? "ok" : "bad"}" data-testid="scenario-exit">exit ${r.exitCode}: ${EXIT[r.exitCode] ?? "?"}</span></h3>
    <p data-testid="scenario-golden">${goldenLine}</p>
    <p class="hint">${stats?.frames ? (stats.frames / 44100).toFixed(2) + " s" : ""} · peak ${stats?.peak_dbfs?.toFixed(1) ?? "?"} dBFS · rms ${stats?.rms_dbfs?.toFixed(1) ?? "?"} dBFS · rendered in ${ms.toFixed(0)} ms</p>
    <ul class="expects">${expects.map((x) => `<li class="${x.ok ? "ok" : "bad"}">line ${x.line}: ${escapeHtml(x.what)} — ${x.ok ? "ok" : "FAILED"}</li>`).join("") || "<li class='hint'>no expect lines</li>"}</ul>
    ${times.map((t) => `<p class="hint" data-testid="scenario-time">${escapeHtml(t)} · for Blackfin cycles: <code>python3 tools/opz_cycles.py</code></p>`).join("")}
    ${warnings.length ? `<ul class="warnings">${warnings.map((w) => `<li class="bad">${escapeHtml(w)}</li>`).join("")}</ul>` : ""}
    <div class="row"><button id="scenario-play" ${r.wav ? "" : "disabled"}>Play render</button>
    <a id="scenario-download" class="button" download="${entry.name}-${scn.replace(/\.scn$/, "")}.wav">Download WAV</a></div>
    <details><summary>Full log</summary><pre class="log">${escapeHtml(r.log)}</pre></details>`;
  if (r.wav) {
    const wav = r.wav;
    $<HTMLAnchorElement>("scenario-download").href = URL.createObjectURL(new Blob([wav as BlobPart], { type: "audio/wav" }));
    $("scenario-play").addEventListener("click", async () => {
      await startAudio();
      playBuffer(new Int32Array(wav.buffer.slice(wav.byteOffset + 44, wav.byteOffset + wav.length)));
    });
  }
}

function escapeHtml(s: string): string {
  return s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]!);
}

// ---- wiring ---------------------------------------------------------------------------------------------------

$("audio").addEventListener("click", () => void startAudio());
$("tempo").addEventListener("change", (e) => post({ type: "tempo", bpm: Number((e.target as HTMLInputElement).value) || 120 }));
$("volume").addEventListener("input", (e) => {
  if (state.volume) state.volume.gain.value = Number((e.target as HTMLInputElement).value) / 100;
});
$("panic").addEventListener("click", () => {
  keyboard.releaseAll();
  post({ type: "panic" });
});
$("velocity").addEventListener("input", (e) => (keyboard.velocity = Number((e.target as HTMLInputElement).value)));
$("keys-target").addEventListener("change", () => keyboard.releaseAll());
// a first key or click on the keyboard starts audio (browsers need a gesture)
window.addEventListener("keydown", () => void (state.ctx ? null : startAudio()), { once: true });
$("keyboard").addEventListener("pointerdown", () => void startAudio());
$("midi-connect").addEventListener("click", async () => {
  const r = await connectMidi((on, note, vel) => keyboard.midi(on, note, vel), (cents) => post({ type: "bend", cents }));
  $("midi").textContent = r.error ? `Web MIDI: ${r.error}` : r.ports.length ? `Web MIDI: ${r.ports.join(", ")}` : "Web MIDI: no input ports";
  void startAudio();
});

async function init(): Promise<void> {
  const r = await fetch("/api/catalog");
  state.catalog = await r.json();
  for (const s of slots) s.renderList();
  if (!state.catalog.some((e) => e.wasm)) {
    synth.q(".slot-info").innerHTML = `<p class="bad">No engine is built for the web yet. Run <code>make -C sim wasm</code>
      (needs LLVM with wasm32, see sdk/docs/QUICKSTART.md), then reload.</p>`;
    return;
  }
  const q = new URLSearchParams(location.search);
  // older forms: ?engine=ID (one engine, in the slot it fits) and ?fx=ID (FX1)
  const legacy = state.catalog.find((e) => e.id === q.get("engine"));
  const fromLegacy = (fx: boolean) => (legacy ? ((legacy.kind === "fx") === fx ? legacy.id : "none") : null);
  await synth.select(q.get("synth") ?? fromLegacy(false) ?? "templates/synth");
  await effects[0].select(q.get("fx1") ?? q.get("fx") ?? fromLegacy(true) ?? "engines/filter");
  await effects[1].select(q.get("fx2") ?? (legacy || q.has("fx") ? "none" : "engines/plate"));
}

void init();
