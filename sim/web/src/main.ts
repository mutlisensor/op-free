/**
 * The web simulator page: eight instrument tracks (OP-Z tracks 1–8, each its own engine) and two effect slots (FX1,
 * FX2) played together. The keyboard (computer keys, on-screen) plays the selected track; Web MIDI plays every track
 * on its own channel, so an OP-Z (or any controller) drives the page multitimbrally: notes by channel, CCs moving the
 * matching knobs, MIDI learn for anything else (src/midimap.ts). Each track's FX1 and FX2 sends feed the effects and
 * crossfade its dry path (src/mixer.ts); each slot has its engine, presets and knobs. Each engine's scenarios run on
 * this thread through opzsim's own main (src/opzsim.ts). Audio runs in an AudioWorklet (src/worklet.ts) at 44.1 kHz.
 *
 * Everything is our own host model (sim/src): no TE code or tables.
 */

import type { CatalogEntry, Kind } from "../catalog.ts";
import { Keyboard, connectMidi, noteName } from "./keyboard.ts";
import { type Knob, knob } from "./knob.ts";
import {
  ALL_SLOTS, FX_SLOTS, type FromWorklet, type FxSlot, INST_SLOTS, type InstSlot, type SlotName, type ToWorklet, isInst,
  slotOf, trackOf,
} from "./messages.ts";
import { type MidiAction, MidiRouter, PARAM_TO_CC, TRACK_NAMES, byteToCc, describe, dialCc } from "./midimap.ts";
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
const SEND_BYTE: Record<FxSlot, number> = { fx1: 6, fx2: 7 };   // an instrument track's send to each FX track
const SENDS = [6, 7];
const DRY_LEVEL = 12;
const FIRST_SEND = 128;       // the send an effect starts with when the track's is 0, so the effect is heard
const INST_KNOBS = [0, 1, 2, 3, 4, 5, 8, 9, 10, 11];
/** The OP-Z's four dial pages, as opzsim bytes (src/midimap.ts). */
const PAGES = [[0, 1, 8, 9], [2, 3, 4, 5], [13, 14, 15, 16], [6, 7, 10, 11]];
const PAGE_NAMES = ["P1 P2 filter res", "envelope", "LFO", "sends pan level"];
const STORE = "opzsim.midi";

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
  selected: "lead" as InstSlot,
  learn: false,
  midiLog: [] as string[],
};

const scope = new Scope($<HTMLCanvasElement>("scope"));
const router = new MidiRouter();

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

function saveRouter(): void {
  try {
    localStorage.setItem(STORE, JSON.stringify(router.save()));
  } catch { /* no storage: the map lasts for this page only */ }
}

function loadRouter(): void {
  try {
    const raw = localStorage.getItem(STORE);
    if (raw) router.load(JSON.parse(raw));
  } catch { /* unreadable: keep the defaults */ }
}

// ---- a slot: engine, track, presets, knobs ---------------------------------------------------------------------

class Slot {
  entry: CatalogEntry | null = null;
  readonly track: number;
  values = DEFAULTS.slice();
  knobs: Knob[] = [];
  loaded = false;
  readonly name: SlotName;
  readonly root: HTMLElement;
  readonly kinds: Kind[];
  /** a byte changed here (knob, preset or MIDI) */
  onByte: (index: number, value: number) => void = () => {};

  constructor(name: SlotName, kinds: Kind[], mainKnobs: number[], root: HTMLElement, template: string) {
    this.name = name;
    this.kinds = kinds;
    this.root = root;
    this.track = trackOf(name);
    this.root.append(($<HTMLTemplateElement>(template)).content.cloneNode(true));
    this.q(".slot-title").textContent = isInst(name) ? `${this.track + 1} · ${TRACK_LABEL[this.track]}` : `Effect on ${TRACK_LABEL[this.track]}`;
    this.q(".slot-engine").dataset.testid = `${name}-engine`;
    for (let i = 0; i < 18; i++) {
      const label = (!isInst(name) && FX_NAMES[i]) || NAMES[i];
      const k = knob(label, i, DEFAULTS[i], DEFAULTS[i], (v) => this.setByte(i, v, false));
      k.el.dataset.slot = name;
      k.el.title = `MIDI: CC ${PARAM_TO_CC[i]} on this track's channel${PARAM_TO_CC[i] > 16 ? " (the OP-Z reads it but does not send it)" : ""}, or Learn`;
      // learn mode: a click or Enter on a knob arms it instead of turning it
      const arm = (e: Event) => {
        if (!state.learn) return;
        e.preventDefault();
        e.stopPropagation();
        armKnob(this, i);
      };
      k.el.addEventListener("pointerdown", arm, { capture: true });
      k.el.addEventListener("keydown", (e) => {
        if ((e as KeyboardEvent).key === "Enter") arm(e);
      }, { capture: true });
      this.knobs.push(k);
      this.q(mainKnobs.includes(i) ? ".main-knobs" : ".more-knobs").append(k.el);
    }
    this.q<HTMLSelectElement>(".slot-engine").addEventListener("change", (e) => void this.select((e.target as HTMLSelectElement).value));
    const ch = this.q<HTMLSelectElement>(".slot-channel");
    ch.dataset.testid = `${name}-channel`;
    for (let c = 1; c <= 16; c++) ch.append(new Option(String(c), String(c - 1)));
    ch.value = String(router.channels[this.track]);
    ch.addEventListener("change", () => {
      router.channels[this.track] = Number(ch.value);
      saveRouter();
      renderTabs();
      renderVopzTracks();
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

  /** Load a preset's bytes, except `keep` (an instrument's sends to effects that are on). */
  setValues(values: number[], immediate: boolean, keep: number[] = []): void {
    for (let i = 0; i < 18; i++) if (!keep.includes(i)) this.setByte(i, values[i] ?? DEFAULTS[i], immediate);
  }

  private keep(): number[] {
    return isInst(this.name) ? effects.filter((e) => e.entry).map((e) => SEND_BYTE[e.name]) : [];
  }

  /** The engines this slot can take: its kinds, and a trackmask with its track. */
  fits(e: CatalogEntry): boolean {
    return this.kinds.includes(e.kind) && !!(e.plug.plug.trackmask & (1 << this.track));
  }

  renderList(): void {
    const sel = this.q<HTMLSelectElement>(".slot-engine");
    sel.textContent = "";
    sel.append(new Option("none", "none"));
    const groups: Array<[string, (e: CatalogEntry) => boolean]> = !isInst(this.name)
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
      if (g.children.length) sel.append(g);
    }
    const others = state.catalog.filter((e) => this.kinds.includes(e.kind) && !this.fits(e)).map((e) => e.label);
    if (others.length) sel.title = `Not on ${TRACK_LABEL[this.track]} (trackmask): ${others.join(", ")}`;
  }

  async select(id: string): Promise<void> {
    const entry = state.catalog.find((e) => e.id === id && this.fits(e)) ?? null;
    this.entry = entry?.wasm ? entry : null;
    this.q<HTMLSelectElement>(".slot-engine").value = this.entry?.id ?? "none";
    this.root.classList.toggle("empty", !this.entry);
    if (isInst(this.name)) this.root.classList.toggle("drum", this.track < 4);
    this.renderInfo();
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
      info.innerHTML = `<p class="hint">Nothing on ${TRACK_LABEL[this.track]}.${isInst(this.name)
        ? ` Pick an engine; MIDI on channel ${router.channels[this.track] + 1} plays it.` : ""}</p>`;
      if (this.name === state.selected || !isInst(this.name)) setText(`st-size-${this.statsId()}`, "–");
      return;
    }
    const p = e.plug;
    const kind = { synth: "synth", drum: "drum", fx: "effect" }[e.kind];
    info.innerHTML = `<span class="badge ${e.kind}">${kind}</span> <span class="hint">${p.plug.name} · plug ${p.plug.id || "–"} ·
      slot ${p.memoryslot || "– (template)"} · <code>${e.dir}/</code></span>`;
    if (this.name === state.selected || !isInst(this.name)) this.renderSize();
  }

  statsId(): string {
    return isInst(this.name) ? "track" : this.name;
  }

  renderSize(): void {
    const e = this.entry;
    if (!e) return setText(`st-size-${this.statsId()}`, "–");
    const slot = e.plug.memoryslot, cls = slot <= 7 ? 50 : slot <= 57 ? 10 : 5;
    $(`st-size-${this.statsId()}`).innerHTML = slot
      ? `not measured here: <code>tools/opz_build.py ${e.dir}</code> fits slot ${slot} (${cls} KB)`
      : "not measured; a template gets its slot from <code>opz.py new</code>";
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

  /** The learned CC under each knob. */
  renderLearned(): void {
    this.knobs.forEach((k, i) => {
      const l = router.learnedFor(this.track, i);
      k.setCc(l ? `ch ${l.channel + 1} CC ${l.cc}` : null);
      const armed = router.armedBinding;
      k.setArmed(!!armed && armed.track === this.track && armed.param === i);
    });
  }

  /** (Re)open this slot's engine in the worklet; its bytes follow once it is loaded. */
  async load(): Promise<void> {
    this.loaded = false;
    setLoaded(this.name, "");
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
    setLoaded(this.name, m.ok ? (this.entry?.id ?? "none") : "failed");
  }
}

/** body[data-<slot>-loaded], and body[data-track-loaded] for the selected track (the e2e tests wait on these). */
function setLoaded(slot: SlotName, value: string): void {
  document.body.dataset[`${slot}Loaded`] = value;
  if (slot === state.selected) document.body.dataset.trackLoaded = value;
}

// ---- an effect section: the selected track's send, dry level and input ----------------------------------------

class Effect extends Slot {
  declare readonly name: FxSlot;
  readonly sendKnob: Knob;
  readonly dryKnob: Knob;
  readonly source: HTMLSelectElement;
  /** a generator picked by hand stays until the user picks the send again */
  private manualSource = false;
  /** tracks this effect already gave a first send to (once each, so a send turned to 0 stays there) */
  private primed = new Set<InstSlot>();

  constructor(name: FxSlot) {
    super(name, ["fx"], [0, 1, 10, 11, 8, 9], $(`slot-${name}`), "fx-template");
    const sendByte = SEND_BYTE[name];
    this.sendKnob = knob(`Send`, sendByte, 0, 0, (v) => selectedSlot().setByte(sendByte, v, false));
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

  /** After a slot or the selection changes: the input, the knobs, and a send that lets this effect be heard. */
  refresh(): void {
    const src = this.source;
    const anyTrack = insts.some((s) => s.entry);
    (src.querySelector('option[value="send"]') as HTMLOptionElement).disabled = !anyTrack;
    if (!anyTrack && src.value === "send") src.value = "saw";
    if (anyTrack && !this.manualSource) src.value = "send";
    const sel = selectedSlot();
    const fed = !!sel.entry && !!this.entry && src.value === "send";
    const sendByte = SEND_BYTE[this.name];
    if (fed && !this.primed.has(state.selected)) {
      this.primed.add(state.selected);
      if (sel.values[sendByte] === 0) sel.setByte(sendByte, FIRST_SEND, true);
    }
    this.sendKnob.rename(`Send from ${TRACK_LABEL[sel.track]} (byte ${sendByte})`);
    this.sendKnob.set(sel.values[sendByte]);
    this.dryKnob.set(this.values[DRY_LEVEL]);
    this.q(".routing").hidden = !this.entry;
    this.q(".send-knobs").hidden = !fed;
    this.q(".source-generator").hidden = src.value === "send";
    this.q(".source-f1-label").hidden = src.value !== "sweep";
    this.q(".routing-note").textContent = src.value !== "send"
      ? `${TRACK_LABEL[this.track]} runs on the generator; the tracks' sends to it are ignored.`
      : "Every track sends to it with its own byte (the knob shows the selected track's). The send crossfades: turning " +
        "it up moves the track from the dry path into this effect. Dry level 255 keeps the dry path as well " +
        "(send/return); at 0 the send is a crossfade.";
  }
}

// ---- keyboard -------------------------------------------------------------------------------------------------

/** Where the on-screen and computer keys go: the selected track, or an effect's onNoteOn. */
function keysSlot(): SlotName {
  const v = $<HTMLSelectElement>("keys-target").value;
  return v === "fx1" || v === "fx2" ? v : state.selected;
}

const keyboard = new Keyboard(
  (on, note, velocity) => post({ type: "note", slot: keysSlot(), on, note, velocity }),
  (first, last) => void ($("octave").textContent = `Keys A–' play ${noteName(first)}–${noteName(last)} · Z/X octave`),
);
$("keyboard").append(keyboard.el);

const instRoot = $("inst-slots");
const insts = INST_SLOTS.map((name) => {
  const root = document.createElement("section");
  root.className = "panel slot synth";
  root.id = `slot-${name}`;
  root.dataset.testid = `slot-${name}`;
  root.setAttribute("role", "tabpanel");
  root.setAttribute("aria-label", `Track ${trackOf(name) + 1}, ${name}`);
  root.hidden = name !== state.selected;
  instRoot.append(root);
  return new Slot(name, ["synth", "drum"], INST_KNOBS, root, "inst-template") as Slot & { readonly name: InstSlot };
});
const effects = FX_SLOTS.map((name) => new Effect(name));
const slots: Slot[] = [...insts, ...effects];
const slotByName = (name: SlotName) => slots.find((s) => s.name === name)!;
const selectedSlot = () => slotByName(state.selected);

for (const s of insts) {
  s.onByte = (i) => {
    if (s.name === state.selected && SENDS.includes(i)) for (const e of effects) e.sendKnob.set(s.values[SEND_BYTE[e.name]]);
    if (vopz.track === s.track) vopzSync();
  };
}
for (const e of effects) {
  const base = e.onByte;
  e.onByte = (i, v) => {
    base(i, v);
    if (vopz.track === e.track) vopzSync();
  };
}

/** For the e2e tests and the curious: the page's state, the last level reading, and a MIDI input. */
(window as unknown as { websim: unknown }).websim = {
  state,
  slots: () => Object.fromEntries(slots.map((s) => [s.name, s.summary()])),
  level: () => scope.level,
  resetLevel: () => void (scope.level.maxPeak = -Infinity),
  /** One MIDI message, exactly as if it came from a Web MIDI port. */
  midiIn: (bytes: number[]) => midiIn(Uint8Array.from(bytes)),
  selected: () => state.selected,
  router: () => router.save(),
};

// ---- tracks: the strip, selection, MIDI activity ---------------------------------------------------------------

const tabs = new Map<SlotName, HTMLElement>();
const ledTimers = new Map<SlotName, number>();

function buildTabs(): void {
  const strip = document.querySelector<HTMLElement>(".track-strip")!;
  for (const s of insts) {
    const b = document.createElement("button");
    b.className = "track-tab";
    b.setAttribute("role", "tab");
    b.dataset.slot = s.name;
    b.dataset.testid = `tab-${s.name}`;
    b.innerHTML = `<span class="led" aria-hidden="true"></span><span class="t-name"></span><span class="t-engine"></span>
      <span class="t-ch"></span><span class="t-meter" aria-hidden="true"><span></span></span>`;
    b.addEventListener("click", () => selectTrack(s.name));
    b.addEventListener("keydown", (e) => {
      const i = INST_SLOTS.indexOf(s.name);
      const to = e.key === "ArrowRight" ? i + 1 : e.key === "ArrowLeft" ? i - 1 : -1;
      if (to < 0 || to >= INST_SLOTS.length) return;
      e.preventDefault();
      selectTrack(INST_SLOTS[to]);
      tabs.get(INST_SLOTS[to])?.focus();
    });
    strip.append(b);
    tabs.set(s.name, b);
  }
  for (const e of effects) {
    const h = e.q(".slot-head h2");
    const led = document.createElement("span");
    led.className = "led fx-led";
    led.setAttribute("aria-hidden", "true");
    h.prepend(led);
    tabs.set(e.name, e.root);
  }
}

function renderTabs(): void {
  for (const s of insts) {
    const b = tabs.get(s.name)!;
    b.classList.toggle("empty", !s.entry);
    b.classList.toggle("drum", s.track < 4);
    b.setAttribute("aria-selected", String(s.name === state.selected));
    b.tabIndex = s.name === state.selected ? 0 : -1;
    b.querySelector(".t-name")!.textContent = `${s.track + 1} ${TRACK_LABEL[s.track]}`;
    b.querySelector(".t-engine")!.textContent = s.entry?.label ?? "—";
    b.querySelector(".t-ch")!.textContent = `ch ${router.channels[s.track] + 1}`;
  }
  for (const s of slots) s.q<HTMLSelectElement>(".slot-channel").value = String(router.channels[s.track]);
}

function flash(slot: SlotName): void {
  const el = tabs.get(slot)?.querySelector(".led");
  if (!el) return;
  el.classList.add("on");
  clearTimeout(ledTimers.get(slot));
  ledTimers.set(slot, window.setTimeout(() => el.classList.remove("on"), 120));
}

function selectTrack(name: InstSlot): void {
  if (name === state.selected) return;
  keyboard.releaseAll();
  keyboard.clearMarks();
  state.selected = name;
  router.selected = trackOf(name);
  for (const s of insts) s.root.hidden = s.name !== name;
  const s = selectedSlot();
  keyboard.configure(s.track < 4);
  document.body.dataset.selected = name;
  document.body.dataset.trackLoaded = s.loaded ? (s.entry?.id ?? "none") : (s.entry ? "" : "none");
  $("th-track").textContent = `${s.track + 1} ${TRACK_LABEL[s.track]}`;
  s.renderSize();
  renderTabs();
  updateUrl();
  renderScenarios();
  slotChanged();
  if (state.lastStats) showStats(state.lastStats);
}

/** After any slot changes: each effect's routing, where keys go, the track note, the tabs. */
function slotChanged(): void {
  for (const e of effects) e.refresh();
  const keys = $<HTMLSelectElement>("keys-target");
  for (const f of FX_SLOTS) (keys.querySelector(`option[value="${f}"]`) as HTMLOptionElement).disabled = !slotByName(f).entry;
  const sel = selectedSlot();
  (keys.querySelector('option[value="track"]') as HTMLOptionElement).textContent = `the selected track (${TRACK_LABEL[sel.track]})`;
  if (!slotByName(keysSlot()).entry) keys.value = sel.entry ? "track" : effects.find((e) => e.entry)?.name ?? "track";
  const t = sel.track;
  $("track-note").textContent = !sel.entry
    ? (effects.some((e) => e.entry) ? `Nothing on ${TRACK_LABEL[t]}: keys can go to an effect's onNoteOn.` : `Nothing on ${TRACK_LABEL[t]}.`)
    : t < 4 ? "Drum track: every key gets pitch 1024 (unity), the key number picks the sound. OP-Z drum keys are F3–E5."
      : `${TRACK_LABEL[t]}: ${[2, 2, 2, 2, 2, 3, 2, 4][t]} voices, pitch from the key (A4 = 440 Hz).`;
  renderTabs();
}

function updateUrl(): void {
  const q = new URLSearchParams();
  for (const s of insts) if (s.entry) q.set(s.name, s.entry.id);
  for (const e of effects) q.set(e.name, e.entry?.id ?? "none");
  if (!insts.some((s) => s.entry)) q.set("synth", "none");
  q.set("track", state.selected);
  history.replaceState(null, "", "?" + q.toString().replaceAll("%2F", "/"));
}

// ---- MIDI in: route, apply, monitor, learn ---------------------------------------------------------------------

function midiIn(data: Uint8Array): void {
  const actions = router.handle(data);
  if (!actions.length) return;                 // clock, SysEx …: not shown
  const notes: string[] = [];
  for (const a of actions) notes.push(apply(a));
  const line = `${describe(data)}${notes.filter(Boolean).map((n) => ` → ${n}`).join("")}`;
  state.midiLog = [line, ...state.midiLog].slice(0, 6);
  $("midi-monitor").textContent = state.midiLog[0];
  $("midi-monitor").title = state.midiLog.join("\n");
}

/** Apply one routed action; returns what it did, for the monitor. */
function apply(a: MidiAction): string {
  if (a.type === "unmapped") return a.why;
  const slotName = slotOf(a.track);
  if (!slotName) return "";
  const slot = slotByName(slotName);
  if (a.type === "learned") {
    renderLearned();
    saveRouter();
    setStatus(`Learned ch ${a.channel + 1} CC ${a.cc} → ${TRACK_LABEL[a.track]} ${NAMES[a.param]}. Click another knob, or Learn to finish.`);
    return "learned";
  }
  flash(slotName);
  if (a.type === "note") {
    post({ type: "note", slot: slotName, on: a.on, note: a.note, velocity: a.velocity });
    if (slotName === state.selected) keyboard.mark(a.note, a.on);
    return slot.entry ? TRACK_LABEL[a.track] : `${TRACK_LABEL[a.track]} (empty)`;
  }
  if (a.type === "bend") {
    post({ type: "bend", slot: slotName, cents: a.cents });
    return `${TRACK_LABEL[a.track]} bend ${a.cents} cents`;
  }
  slot.setByte(a.param, a.byte, false);
  if (!a.learned && isInst(slotName) && slotName !== state.selected && $<HTMLInputElement>("midi-follow").checked) selectTrack(slotName);
  return `${TRACK_LABEL[a.track]} ${NAMES[a.param]} ${a.byte}`;
}

function renderLearned(): void {
  for (const s of slots) s.renderLearned();
  $("midi-forget").textContent = router.learnedCount ? `Clear learned (${router.learnedCount})` : "Clear learned";
  ($("midi-forget") as HTMLButtonElement).disabled = !router.learnedCount;
}

function setStatus(text: string): void {
  $("midi-status").textContent = text;
}

function armKnob(slot: Slot, param: number): void {
  router.arm({ track: slot.track, param });
  renderLearned();
  setStatus(`Move a control on your MIDI device for ${TRACK_LABEL[slot.track]} ${NAMES[param]}…`);
}

function setLearn(on: boolean): void {
  state.learn = on;
  if (!on) router.arm(null);
  document.body.classList.toggle("learning", on);
  $("midi-learn").setAttribute("aria-pressed", String(on));
  $("midi-learn").textContent = on ? "Learning: click a knob" : "Learn";
  setStatus(on ? "Learn: click a knob, then move a control. The OP-Z's own dials need no learning." : "");
  renderLearned();
}

// ---- the virtual OP-Z: sends what the device sends (offline testing) -------------------------------------------

const vopz = { track: 5, page: 0, dials: [] as Knob[] };

function renderVopzTracks(): void {
  const sel = $<HTMLSelectElement>("vopz-track");
  sel.textContent = "";
  for (let t = 0; t < TRACK_NAMES.length; t++) sel.append(new Option(`${t + 1} ${TRACK_NAMES[t]} (ch ${router.channels[t] + 1})`, String(t)));
  sel.value = String(vopz.track);
}

function buildVopz(): void {
  renderVopzTracks();
  $<HTMLSelectElement>("vopz-track").addEventListener("change", (e) => {
    vopz.track = Number((e.target as HTMLSelectElement).value);
    vopzSync();
  });
  const pages = document.querySelector<HTMLElement>(".vopz-pages")!;
  PAGE_NAMES.forEach((name, p) => {
    const b = document.createElement("button");
    b.setAttribute("role", "radio");
    b.dataset.page = String(p);
    b.textContent = `${p + 1} · ${name}`;
    b.addEventListener("click", () => {
      vopz.page = p;
      vopzSync();
    });
    pages.append(b);
  });
  const box = document.querySelector<HTMLElement>(".vopz-dials")!;
  for (let d = 0; d < 4; d++) {
    const k = knob(`dial ${d + 1}`, d, 0, 0, (v) => {
      const ch = router.channels[vopz.track];
      midiIn(Uint8Array.from([0xb0 | ch, dialCc(vopz.page, d), byteToCc(v)]));
    });
    k.el.querySelector<HTMLElement>(".knob-dial")!.dataset.param = `vopz-${d}`;
    k.el.classList.add("big");
    vopz.dials.push(k);
    box.append(k.el);
  }
  $("vopz-note").addEventListener("click", () => {
    const ch = router.channels[vopz.track], note = vopz.track < 4 ? 53 + 2 * vopz.track : 60;
    void startAudio();
    midiIn(Uint8Array.from([0x90 | ch, note, 100]));
    setTimeout(() => midiIn(Uint8Array.from([0x80 | ch, note, 0])), 400);
  });
  vopzSync();
}

/** The virtual dials show the bytes of the track and page they are on. */
function vopzSync(): void {
  if (!vopz.dials.length) return;
  const name = slotOf(vopz.track);
  const s = name ? slotByName(name) : null;
  PAGES[vopz.page].forEach((param, d) => {
    vopz.dials[d].rename(`CC ${dialCc(vopz.page, d)} · ${(s && !isInst(s.name) && FX_NAMES[param]) || NAMES[param]}`);
    if (s) vopz.dials[d].set(s.values[param]);
  });
  document.querySelectorAll<HTMLElement>(".vopz-pages button").forEach((b) => {
    b.setAttribute("aria-checked", String(Number(b.dataset.page) === vopz.page));
  });
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
  for (const name of [state.selected, ...FX_SLOTS]) {
    const s = m.slots[name], isFx = !isInst(name), id = isFx ? name : "track";
    if (!s) {
      setText(`st-state-${id}`, "–");
      setText(`st-cpu-${id}`, "–");
      if (!isFx) {
        setText("st-add-track", "–");
        setText("st-voices-track", "–");
      }
      continue;
    }
    const budget = isFx ? "200 B per FX" : "280 B per voice";
    setText(`st-state-${id}`, s.stateViolations ? `VIOLATED ${s.stateViolations}× (see log)` : `ok · ${budget}`, !s.stateViolations);
    setText(`st-cpu-${id}`, cpu(s, isFx));
    if (!isFx) {
      setText("st-add-track", s.addViolations ? `VIOLATED ${s.addViolations}× (see log)` : "ok", !s.addViolations);
      setText("st-voices-track", `${s.active} of ${s.voices} active (most ${s.maxUsed}), ${s.stolen} stolen`);
    }
  }
  // a violation on a track that is not selected still shows, on its tab
  for (const name of INST_SLOTS) {
    const s = m.slots[name];
    tabs.get(name)?.classList.toggle("violated", !!s && (s.stateViolations > 0 || s.addViolations > 0));
  }
  const pct = (x: number) => `${Math.round(x * 100)}%`;
  const mix = m.mix[state.selected];
  const fed = effects.filter((e) => m.slots[e.name] && e.source.value === "send");
  $("st-mix").textContent = m.slots[state.selected] && mix && fed.length
    ? `Mix of ${TRACK_LABEL[trackOf(state.selected)]}: ${fed.map((e) => `${pct(mix.sends[FX_SLOTS.indexOf(e.name)])} into ${TRACK_LABEL[e.track]}`).join(", ")}, ${pct(mix.dry)} dry.`
    : "";
  for (const name of ALL_SLOTS) {
    const bar = tabs.get(name)?.querySelector<HTMLElement>(".t-meter span");
    const p = m.peaks[name] ?? 0;
    const db = p > 0 ? 20 * Math.log10(p) : -Infinity;
    if (bar) bar.style.width = `${Math.max(0, Math.min(100, ((db + 60) / 60) * 100)).toFixed(0)}%`;
  }
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
  for (const slot of [selectedSlot(), ...effects]) {
    const entry = slot.entry;
    if (!entry) continue;
    const h = document.createElement("h3");
    h.innerHTML = `<span class="dot ${isInst(slot.name) ? "synth" : "fx"}"></span>${entry.label} <span class="hint">on ${TRACK_LABEL[slot.track]}</span>`;
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
  keyboard.clearMarks();
  post({ type: "panic" });
});
$("velocity").addEventListener("input", (e) => (keyboard.velocity = Number((e.target as HTMLInputElement).value)));
$("keys-target").addEventListener("change", () => keyboard.releaseAll());
// a first key or click on the keyboard starts audio (browsers need a gesture)
window.addEventListener("keydown", () => void (state.ctx ? null : startAudio()), { once: true });
$("keyboard").addEventListener("pointerdown", () => void startAudio());
const showPorts = (ports: string[]) => {
  $("midi").textContent = ports.length ? `Web MIDI: ${ports.join(", ")}` : "Web MIDI: no input ports";
};
$("midi-connect").addEventListener("click", async () => {
  const r = await connectMidi(midiIn, showPorts);
  if (r.error) $("midi").textContent = `Web MIDI: ${r.error}`;
  else showPorts(r.ports);
  void startAudio();
});
$<HTMLInputElement>("midi-omni").addEventListener("change", (e) => {
  router.omni = (e.target as HTMLInputElement).checked;
  saveRouter();
});
$("midi-learn").addEventListener("click", () => setLearn(!state.learn));
$("midi-forget").addEventListener("click", () => {
  router.forget();
  saveRouter();
  renderLearned();
  setStatus("Learned CCs cleared; the OP-Z map is unchanged.");
});
$("vopz-toggle").addEventListener("click", () => {
  const box = $("vopz");
  box.hidden = !box.hidden;
  $("vopz-toggle").setAttribute("aria-pressed", String(!box.hidden));
  if (!box.hidden) {
    vopz.track = trackOf(state.selected);
    renderVopzTracks();
    vopzSync();
  }
});
window.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && state.learn) setLearn(false);
});

async function init(): Promise<void> {
  loadRouter();
  $<HTMLInputElement>("midi-omni").checked = router.omni;
  buildTabs();
  buildVopz();
  renderLearned();
  const r = await fetch("/api/catalog");
  state.catalog = await r.json();
  for (const s of slots) s.renderList();
  renderTabs();
  if (!state.catalog.some((e) => e.wasm)) {
    selectedSlot().q(".slot-info").innerHTML = `<p class="bad">No engine is built for the web yet. Run <code>make -C sim wasm</code>
      (needs LLVM with wasm32, see sdk/docs/QUICKSTART.md), then reload.</p>`;
    return;
  }
  const q = new URLSearchParams(location.search);
  if (q.has("vopz")) $("vopz-toggle").click();
  // older forms: ?synth=ID (one instrument, on the track it fits best), ?engine=ID and ?fx=ID (FX1)
  const legacy = state.catalog.find((e) => e.id === q.get("engine"));
  const synthId = q.get("synth") ?? (legacy ? (legacy.kind !== "fx" ? legacy.id : "none") : null);
  const picks = new Map<InstSlot, string>();
  for (const s of insts) if (q.get(s.name)) picks.set(s.name, q.get(s.name)!);
  if (synthId && synthId !== "none") {
    const e = state.catalog.find((x) => x.id === synthId);
    const preferred: InstSlot = e?.kind === "drum" ? "kick" : "lead";
    const home = insts.find((s) => s.name === preferred && e && s.fits(e)) ?? insts.find((s) => e && s.fits(e));
    if (home) picks.set(home.name, synthId);
  }
  if (!picks.size && !q.has("synth") && !legacy) picks.set("lead", "templates/synth");
  const wanted = q.get("track") as InstSlot | null;
  const first = wanted && INST_SLOTS.includes(wanted) ? wanted : [...picks.keys()][0] ?? "lead";
  state.selected = "lead";
  router.selected = 5;
  document.body.dataset.selected = "lead";
  if (first !== "lead") selectTrack(first);
  else {
    $("th-track").textContent = "6 lead";
    keyboard.configure(false);
  }
  for (const s of insts) await s.select(picks.get(s.name) ?? "none");
  await effects[0].select(q.get("fx1") ?? q.get("fx") ?? (legacy?.kind === "fx" ? legacy.id : null) ?? "engines/filter");
  await effects[1].select(q.get("fx2") ?? (legacy || q.has("fx") ? "none" : "engines/plate"));
}

void init();
