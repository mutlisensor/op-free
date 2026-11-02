/**
 * The synth lab (workstream W1): sim/web retooled for the site. A synth slot (a synth or drum engine on its track)
 * feeds FX1 and FX2 through its sends, as on the OP-Z; each slot has its presets and the four OP-Z dials (P1, P2,
 * filter, resonance), the synth its two sends. Notes come from the on-screen keys, the computer keyboard and Web
 * MIDI. The engine browser lists the catalogue (src/shared/catalogue.ts); each card's "Add to my OP-Z" talks to the
 * loader through `opfree:pick`, and the loader's `opfree:audition` loads an engine here. Nothing here talks to an
 * OP-Z: the lab only plays, in this browser.
 */

import "./lab.css";
import { type CatalogueEngine, type Kind, type Preset, loadCatalogue } from "../shared/catalogue.ts";
import type { AuditionEvent, Mounted, PickEvent } from "../shared/mount.ts";
import { LabAudio } from "./audio.ts";
import { FX_SLOTS, type FromWorklet, type FxSlot, type SlotName } from "./engine/messages.ts";
import { Keyboard, noteName } from "./keyboard.ts";
import { type Knob, knob } from "./knob.ts";
import { MidiIn } from "./midi.ts";
import { Scope } from "./scope.ts";

/** The 18 bytes of a track (engine-abi.md). Neutral start: sustain 255, filter 128 (centre), pan 128, level 200,
 * LFO depth 128 (bipolar: off). Presets set all 18. */
const DEFAULTS = [0, 0, 0, 0, 255, 0, 0, 0, 128, 0, 128, 200, 0, 128, 0, 0, 0, 0];
const TRACKS = ["kick", "snare", "perc", "sample", "bass", "lead", "arp", "chord", "FX1", "FX2"];
const FX_TRACK: Record<FxSlot, number> = { fx1: 8, fx2: 9 };
const SEND_BYTE: Record<FxSlot, number> = { fx1: 6, fx2: 7 };
const FIRST_SEND = 128;       // the send an effect starts with when the synth's is 0, so the effect is heard
/** The OP-Z's four dials, in their colours: green P1, blue P2, yellow filter, red resonance. */
const DIALS: Array<[number, string, "green" | "blue" | "yellow" | "red"]> = [
  [0, "P1", "green"], [1, "P2", "blue"], [8, "filter", "yellow"], [9, "resonance", "red"],
];
/** What the lab opens with when these are in the catalogue (else the first featured engine of the kind). */
const START: Record<SlotName, string> = { synth: "super", fx1: "swirl", fx2: "plate" };
const KIND_LABEL: Record<Kind, string> = { synth: "Synths", drum: "Drums", fx: "Effects" };

const esc = (s: string) => s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]!);

function paramNames(e: CatalogueEngine): string {
  const [p1, p2] = e.manifest.plug.parameter_names ?? [];
  return [p1 && `P1 ${p1}`, p2 && `P2 ${p2}`].filter(Boolean).join(" · ");
}

/** The tracks an engine may sit on: synth or drum tracks for the synth slot. */
function tracksOf(e: CatalogueEngine): number[] {
  return [0, 1, 2, 3, 4, 5, 6, 7].filter((t) => e.manifest.plug.trackmask & (1 << t));
}

/** Presets are per synth track (slot 0 bass … 3 chord); drums and effects use slot 0 (presets.md). */
function presetsFor(e: CatalogueEngine, track: number): Preset[] {
  const slot = track >= 4 && track < 8 ? track - 4 : 0;
  const mine = e.presets.filter((p) => (p.slot ?? 0) === slot);
  return mine.length ? mine : e.presets;
}

const fitsFx = (e: CatalogueEngine, slot: FxSlot) => e.kind === "fx" && !!(e.manifest.plug.trackmask & (1 << FX_TRACK[slot]));

export function mountLab(el: HTMLElement): Mounted {
  el.classList.add("lab-host");
  el.innerHTML = `
    <div class="lab">
      <header class="lab__head">
        <div class="lab__title">
          <p class="label"><i class="dot dot--green"></i>Synth lab</p>
          <p class="lab__status" data-status>Loading the engines…</p>
        </div>
        <div class="lab__tools">
          <label class="lab__vol"><span class="label">Volume</span>
            <input type="range" min="0" max="100" value="80" data-volume aria-label="Volume" /></label>
          <button class="btn btn--small" type="button" data-panic title="All notes off">Notes off</button>
        </div>
      </header>
      <div class="lab__rack">
        <section class="slot slot--synth" data-slot="synth" aria-label="Synth"></section>
        <section class="slot slot--fx" data-slot="fx1" aria-label="Effect on FX1"></section>
        <section class="slot slot--fx" data-slot="fx2" aria-label="Effect on FX2"></section>
      </div>
      <div class="lab__play">
        <div class="lab__bar">
          <div class="lab__oct">
            <button class="btn btn--small btn--icon" type="button" data-oct="-1" aria-label="Octave down">−</button>
            <span class="label" data-range>C3–F4</span>
            <button class="btn btn--small btn--icon" type="button" data-oct="1" aria-label="Octave up">+</button>
          </div>
          <label class="lab__vel"><span class="label">Velocity <b data-vel-n>100</b></span>
            <input type="range" min="1" max="127" value="100" data-velocity aria-label="Velocity" /></label>
          <div class="lab__midi">
            <button class="btn btn--small" type="button" data-midi>MIDI in</button>
            <select data-midi-port hidden aria-label="MIDI input"></select>
            <span class="lab__midi-note dim" data-midi-note></span>
          </div>
          <div class="lab__scope">
            <canvas data-scope aria-hidden="true"></canvas>
            <span class="lab__meter" data-meter aria-hidden="true"></span>
          </div>
        </div>
        <div data-keys></div>
        <p class="lab__hint dim" data-keyhint>Play with the keys A to ' on your keyboard (Z and X: octave), tap or click
          the keys, or plug in a MIDI keyboard.</p>
      </div>
      <div class="lab__browser">
        <div class="lab__browser-head">
          <div class="lab__tabs" role="tablist" aria-label="Engine kind" data-tabs></div>
          <label class="lab__only"><input type="checkbox" data-featured /> <span>Featured only</span></label>
        </div>
        <ul class="lab__cards" data-cards aria-live="polite"></ul>
      </div>
    </div>`;

  const $ = <T extends HTMLElement = HTMLElement>(sel: string) => el.querySelector(sel) as T;
  const status = $("[data-status]");
  const audio = new LabAudio();
  const scope = new Scope($<HTMLCanvasElement>("[data-scope]"), $("[data-meter]"));
  const midi = new MidiIn({
    note: (on, note, velocity) => {
      keyboard.midi(on, note, velocity);
      $("[data-midi-note]").textContent = on && velocity > 0 ? `${noteName(note)} · ${velocity}` : "";
      if (on) void startAudio();
    },
    bend: (cents) => audio.post({ type: "bend", cents }),
    ports: (inputs) => {
      const sel = $<HTMLSelectElement>("[data-midi-port]");
      const was = sel.value;
      sel.replaceChildren(new Option("No input", ""), ...inputs.map((i) => new Option(i.name, i.id)));
      sel.hidden = false;
      sel.value = inputs.some((i) => i.id === was) ? was : inputs[0]?.id ?? "";
      midi.select(sel.value || null);
      $("[data-midi]").hidden = true;
    },
  });

  let catalogue: CatalogueEngine[] = [];
  const picked = new Set<string>();
  let tab: Kind = "synth";
  let destroyed = false;
  const off: Array<() => void> = [];
  const on = <K extends keyof DocumentEventMap>(target: Document | Window, type: K | string, fn: (e: Event) => void) => {
    target.addEventListener(type, fn);
    off.push(() => target.removeEventListener(type, fn));
  };

  // ---- slots ----------------------------------------------------------------------------------------------------

  class Slot {
    readonly name: SlotName;
    readonly root: HTMLElement;
    entry: CatalogueEngine | null = null;
    track: number;
    values = DEFAULTS.slice();
    knobs = new Map<number, Knob>();
    sends = new Map<FxSlot, Knob>();
    loaded = false;
    preset: string | null = null;

    constructor(name: SlotName) {
      this.name = name;
      this.root = $(`[data-slot="${name}"]`);
      this.track = name === "synth" ? 5 : FX_TRACK[name];
    }

    /** Set one byte: the knob, the worklet. */
    setByte(index: number, value: number, immediate: boolean): void {
      this.values[index] = value;
      this.knobs.get(index)?.set(value);
      if (this.name === "synth") for (const s of FX_SLOTS) if (SEND_BYTE[s] === index) this.sends.get(s)?.set(value);
      audio.post({ type: "param", slot: this.name, index, value, immediate });
    }

    /** The synth's two sends are the player's: they survive a preset change and a change of synth. */
    private keep(): number[] {
      return this.name === "synth" ? FX_SLOTS.map((s) => SEND_BYTE[s]) : [];
    }

    applyPreset(name: string | null, values: number[]): void {
      const keep = this.keep();
      for (let i = 0; i < 18; i++) if (!keep.includes(i)) this.setByte(i, values[i] ?? DEFAULTS[i], true);
      this.preset = name;
      this.root.querySelectorAll<HTMLButtonElement>("[data-preset]").forEach((b) =>
        b.setAttribute("aria-pressed", String(b.dataset.preset === (name ?? ""))));
    }

    async select(entry: CatalogueEngine | null): Promise<void> {
      this.entry = entry?.wasm ? entry : null;
      if (this.name === "synth" && this.entry) {
        const tracks = tracksOf(this.entry);
        const preferred = this.entry.kind === "drum" ? 0 : 5;
        if (!tracks.includes(this.track)) this.track = tracks.includes(preferred) ? preferred : tracks[0] ?? preferred;
        keyboard.configure(this.track < 4);
      }
      const first = this.entry ? presetsFor(this.entry, this.track)[0] : undefined;
      const keep = this.keep();
      for (let i = 0; i < 18; i++) if (!keep.includes(i)) this.values[i] = first?.values[i] ?? DEFAULTS[i];
      this.preset = first?.name ?? null;
      this.render();
      routing();
      renderCards();
      await this.load();
    }

    /** Change the synth slot's track (voices and presets differ per track). */
    async setTrack(track: number): Promise<void> {
      this.track = track;
      keyboard.configure(track < 4);
      await this.select(this.entry);
    }

    /** (Re)open this slot's engine in the worklet; its bytes follow once it is loaded. */
    async load(): Promise<void> {
      this.loaded = false;
      if (!audio.running) return;              // audio not started: loaded when it is
      const entry = this.entry;
      let bytes: ArrayBuffer | null = null;
      try {
        bytes = entry?.wasm ? await audio.bytes(entry.wasm) : null;
      } catch (e) {
        setStatus(`Couldn't load ${entry?.label}: ${(e as Error).message}`, true);
      }
      if (entry !== this.entry || destroyed) return;   // another engine was chosen meanwhile
      audio.post({ type: "load", slot: this.name, bytes, track: this.track }, bytes ? [bytes] : []);
    }

    afterLoad(m: Extract<FromWorklet, { type: "loaded" }>): void {
      this.loaded = m.ok && !m.empty;
      if (!m.ok) setStatus(`${this.entry?.label ?? "The engine"} refused to start in the simulator.`, true);
      if (this.loaded) {
        this.values.forEach((v, i) => audio.post({ type: "param", slot: this.name, index: i, value: v, immediate: true }));
        audio.post({ type: "tempo", bpm: 120 });
      }
      this.root.dataset.loaded = this.loaded ? this.entry!.dir : "";
    }

    render(): void {
      const e = this.entry;
      const isSynth = this.name === "synth";
      const tag = isSynth ? (e?.kind === "drum" ? "Drum" : "Synth") : this.name.toUpperCase();
      const dot = isSynth ? "green" : this.name === "fx1" ? "blue" : "yellow";
      this.knobs.clear();
      this.sends.clear();
      this.root.classList.toggle("slot--empty", !e);
      this.root.innerHTML = `
        <header class="slot__head">
          <p class="label"><i class="dot dot--${dot}"></i>${tag}${isSynth && e ? ` · track ${this.track + 1} ${TRACKS[this.track]}` : ""}</p>
          ${!isSynth && e ? `<button class="slot__off" type="button" data-off aria-label="Take ${esc(e.label)} off ${tag}">Off</button>` : ""}
        </header>
        <h3 class="slot__name">${e ? esc(e.label) : "Empty"}</h3>
        <p class="slot__sub">${e ? esc(paramNames(e)) : isSynth ? "Choose a synth or drum engine below." : "Choose an effect below, or play dry."}</p>
        ${e ? `
          ${isSynth && tracksOf(e).length > 1 ? `<div class="chips chips--tracks" role="group" aria-label="Track">${tracksOf(e).map((t) =>
            `<button type="button" class="chip" data-track="${t}" aria-pressed="${t === this.track}">${TRACKS[t]}</button>`).join("")}</div>` : ""}
          <div class="chips" role="group" aria-label="Presets">
            <button type="button" class="chip chip--init" data-preset="" aria-pressed="${this.preset === null}" title="Neutral settings">init</button>
            ${presetsFor(e, this.track).map((p) => `<button type="button" class="chip" data-preset="${esc(p.name)}" aria-pressed="${p.name === this.preset}">${esc(p.name)}</button>`).join("")}
          </div>
          <div class="slot__knobs" data-knobs></div>` : ""}`;
      if (!e) return;
      const knobs = this.root.querySelector<HTMLElement>("[data-knobs]")!;
      const names = e.manifest.plug.parameter_names ?? [];
      for (const [index, label, colour] of DIALS) {
        const text = index < 2 && names[index] ? names[index] : label;
        const k = knob(text, index, this.values[index], index === 8 ? 128 : DEFAULTS[index], (v) => this.setByte(index, v, false), colour);
        const help = index === 0 ? e.help?.p1 : index === 1 ? e.help?.p2 : index === 8
          ? "The OP-Z's track filter: low-pass below the centre, high-pass above (approximated here)" : "The track filter's resonance";
        if (help) k.el.title = help;
        this.knobs.set(index, k);
        knobs.append(k.el);
      }
      if (isSynth) {
        const sep = document.createElement("span");
        sep.className = "slot__sep";
        knobs.append(sep);
        for (const s of FX_SLOTS) {
          const byte = SEND_BYTE[s];
          const k = knob(`send ${s.toUpperCase()}`, byte, this.values[byte], 0, (v) => {
            sendSet.add(s);
            this.setByte(byte, v, false);
          });
          k.el.classList.add("knob--send");
          k.el.title = `How much of the synth goes into ${s.toUpperCase()}. The send crossfades: turned up, the synth moves from the dry path into the effect.`;
          this.sends.set(s, k);
          knobs.append(k.el);
        }
        this.refreshSends();
      }
      this.root.querySelectorAll<HTMLButtonElement>("[data-preset]").forEach((b) => b.addEventListener("click", () => {
        void startAudio();
        const p = presetsFor(e, this.track).find((x) => x.name === b.dataset.preset);
        this.applyPreset(p?.name ?? null, p?.values ?? DEFAULTS);
      }));
      this.root.querySelectorAll<HTMLButtonElement>("[data-track]").forEach((b) =>
        b.addEventListener("click", () => void this.setTrack(Number(b.dataset.track))));
      this.root.querySelector("[data-off]")?.addEventListener("click", () => void this.select(null));
    }

    /** Send knobs are live only when their effect slot is on. */
    refreshSends(): void {
      for (const [s, k] of this.sends) k.el.classList.toggle("knob--idle", !effects.find((x) => x.name === s)?.entry);
    }
  }

  const keyboard = new Keyboard(
    (on, note, velocity) => {
      void startAudio();
      audio.post({ type: "note", slot: "synth", on, note, velocity });
    },
    (first, last) => void ($("[data-range]").textContent = `${noteName(first)}–${noteName(last)}`),
  );
  $("[data-keys]").append(keyboard.el);

  const synth = new Slot("synth");
  const effects = FX_SLOTS.map((n) => new Slot(n)) as [Slot & { name: FxSlot }, Slot & { name: FxSlot }];
  const slots = [synth, ...effects];

  /** Sends that already have a level (set here once, or turned by the player): never changed for them again. */
  const sendSet = new Set<FxSlot>();

  /** After a slot changes: the first time an effect and a synth are both on, a send that lets the effect be heard;
   * and the send knobs' state. */
  function routing(): void {
    for (const fx of effects) {
      if (sendSet.has(fx.name) || !fx.entry || !synth.entry) continue;
      const byte = SEND_BYTE[fx.name];
      if (synth.values[byte] === 0) synth.setByte(byte, FIRST_SEND, true);
      sendSet.add(fx.name);
    }
    synth.refreshSends();
  }

  function setStatus(text: string, bad = false): void {
    status.textContent = text;
    status.classList.toggle("bad", bad);
  }

  async function startAudio(): Promise<void> {
    if (audio.running || destroyed) {
      if (audio.ctx?.state === "suspended") void audio.ctx.resume();
      return;
    }
    setStatus("Starting the sound…");
    const ok = await audio.start();
    if (destroyed) return;
    if (!ok) return setStatus("This browser couldn't start the sound (it needs AudioWorklet and WebAssembly).", true);
    el.querySelector(".lab")!.setAttribute("data-audio", "on");
    const rate = audio.ctx!.sampleRate;
    setStatus(rate === 44100 ? "Sound on · 44.1 kHz, as on the OP-Z" : `Sound on · ${rate} Hz`);
    scope.attach(...audio.analysers!);
    for (const s of slots) await s.load();
  }

  audio.onMessage = (m) => {
    if (m.type === "loaded") slots.find((s) => s.name === m.slot)?.afterLoad(m);
    else if (m.type === "error") console.warn("lab:", m.log);
  };

  // ---- the engine browser ---------------------------------------------------------------------------------------

  function renderTabs(): void {
    const tabs = $("[data-tabs]");
    tabs.innerHTML = (["synth", "drum", "fx"] as Kind[]).map((k) => {
      const n = catalogue.filter((e) => e.kind === k).length;
      return `<button type="button" role="tab" class="lab__tab" data-tab="${k}" aria-selected="${k === tab}">${KIND_LABEL[k]} <span class="dim">${n}</span></button>`;
    }).join("");
    tabs.querySelectorAll<HTMLButtonElement>("[data-tab]").forEach((b) => b.addEventListener("click", () => {
      tab = b.dataset.tab as Kind;
      renderTabs();
      renderCards();
    }));
  }

  function renderCards(): void {
    const list = $("[data-cards]");
    const featuredOnly = $<HTMLInputElement>("[data-featured]").checked;
    const shown = catalogue
      .filter((e) => e.kind === tab && (!featuredOnly || e.featured))
      .sort((a, b) => Number(!!b.featured) - Number(!!a.featured) || a.label.localeCompare(b.label));
    const where = (e: CatalogueEngine) => slots.filter((s) => s.entry?.dir === e.dir).map((s) => s.name);
    list.innerHTML = shown.map((e) => {
      const at = where(e);
      const isFx = e.kind === "fx";
      const playing = at.length ? `<span class="card__on">${at.map((s) => s === "synth" ? "playing" : `on ${s.toUpperCase()}`).join(" · ")}</span>` : "";
      return `
        <li class="card${at.length ? " card--on" : ""}" data-dir="${e.dir}">
          <button type="button" class="card__main" data-play ${e.wasm ? "" : "disabled"}
            aria-label="${isFx ? "Put" : "Play"} ${esc(e.label)}${isFx ? " on FX1" : ""}">
            <span class="card__top"><span class="card__name">${esc(e.label)}</span>${e.featured ? `<span class="card__feat">featured</span>` : ""}${playing}</span>
            <span class="card__params">${esc(paramNames(e))}</span>
            ${e.summary ? `<span class="card__sum">${esc(e.summary)}</span>` : ""}
            ${e.caution ? `<span class="card__caution">${esc(e.caution)}</span>` : ""}
            <span class="card__by">${e.wasm ? "" : "Not built for the browser yet · "}by ${esc(e.author ?? "OP-FREE")}</span>
          </button>
          <div class="card__actions">
            ${isFx ? FX_SLOTS.map((s) => `<button type="button" class="chip chip--slot" data-fx="${s}" ${fitsFx(e, s) && e.wasm ? "" : "disabled"}
              aria-pressed="${at.includes(s)}">${s.toUpperCase()}</button>`).join("") : ""}
            <button type="button" class="pick" data-pick aria-pressed="${picked.has(e.dir)}">
              <span class="pick__box" aria-hidden="true"></span>${picked.has(e.dir) ? "On my OP-Z list" : "Add to my OP-Z"}</button>
          </div>
        </li>`;
    }).join("") || `<li class="lab__none dim">Nothing here yet.</li>`;
    list.querySelectorAll<HTMLElement>(".card").forEach((card) => {
      const e = catalogue.find((x) => x.dir === card.dataset.dir)!;
      card.querySelector("[data-play]")!.addEventListener("click", () => void audition(e));
      card.querySelectorAll<HTMLButtonElement>("[data-fx]").forEach((b) => b.addEventListener("click", () => {
        const slot = effects.find((s) => s.name === b.dataset.fx)!;
        void startAudio();
        void slot.select(slot.entry?.dir === e.dir ? null : e);
      }));
      card.querySelector("[data-pick]")!.addEventListener("click", () => {
        const next = !picked.has(e.dir);
        document.dispatchEvent(new CustomEvent("opfree:pick", { detail: { dir: e.dir, picked: next } }) as PickEvent);
      });
    });
  }

  /** Load an engine where it belongs: synths and drums into the synth slot, an effect into FX1 (else FX2). */
  async function audition(e: CatalogueEngine): Promise<void> {
    void startAudio();
    if (e.kind !== "fx") return synth.select(e);
    const slot = effects.find((s) => s.entry?.dir === e.dir) ?? effects.find((s) => fitsFx(e, s.name));
    await slot?.select(e);
  }

  // ---- wiring ---------------------------------------------------------------------------------------------------

  $<HTMLInputElement>("[data-volume]").addEventListener("input", (e) =>
    audio.setVolume(Number((e.target as HTMLInputElement).value) / 100));
  $<HTMLInputElement>("[data-velocity]").addEventListener("input", (e) => {
    keyboard.velocity = Number((e.target as HTMLInputElement).value);
    $("[data-vel-n]").textContent = String(keyboard.velocity);
  });
  $("[data-panic]").addEventListener("click", () => {
    keyboard.releaseAll();
    audio.post({ type: "panic" });
  });
  el.querySelectorAll<HTMLButtonElement>("[data-oct]").forEach((b) =>
    b.addEventListener("click", () => keyboard.shift(Number(b.dataset.oct))));
  $("[data-featured]").addEventListener("change", renderCards);
  $("[data-midi]").addEventListener("click", async () => {
    void startAudio();
    const err = await midi.connect();
    if (err) $("[data-midi-note]").textContent = err;
  });
  $<HTMLSelectElement>("[data-midi-port]").addEventListener("change", (e) =>
    midi.select((e.target as HTMLSelectElement).value || null));
  // Audio starts on the first gesture anywhere in the lab, or a played computer key.
  el.addEventListener("pointerdown", () => void startAudio());
  on(window, "keydown", (e) => {
    const k = (e as KeyboardEvent).key.toLowerCase();
    if (!audio.running && "awsedftgyhujkolp;'".includes(k) && !(e.target as HTMLElement)?.closest?.("input,select,textarea,dialog")) void startAudio();
  });
  on(document, "opfree:pick", (ev) => {
    const { dir, picked: p } = (ev as PickEvent).detail;
    if (p) picked.add(dir);
    else picked.delete(dir);
    renderCards();
  });
  on(document, "opfree:audition", (ev) => {
    const e = catalogue.find((x) => x.dir === (ev as AuditionEvent).detail.dir);
    if (!e) return;
    tab = e.kind;
    renderTabs();
    void audition(e);
    el.scrollIntoView({ behavior: "smooth", block: "start" });
  });

  if (import.meta.env.DEV) (window as unknown as { opfreeLab: unknown }).opfreeLab = { audio, scope, slots, picked };

  void (async () => {
    try {
      catalogue = (await loadCatalogue()).engines;
    } catch (e) {
      return setStatus(`The engine list didn't load (${(e as Error).message}).`, true);
    }
    if (destroyed) return;
    for (const e of catalogue) if (e.featured) picked.add(e.dir);
    renderTabs();
    const playable = catalogue.filter((e) => e.wasm);
    if (!playable.length) {
      renderCards();
      return setStatus("No engine is built for the browser yet.", true);
    }
    const pick = (slot: SlotName, kinds: Kind[]) => {
      const ok = (e: CatalogueEngine) => !!e.wasm && kinds.includes(e.kind) && (slot === "synth" || fitsFx(e, slot as FxSlot));
      return playable.find((e) => e.dir === START[slot] && ok(e)) ?? playable.find((e) => e.featured && ok(e)) ?? playable.find(ok) ?? null;
    };
    await synth.select(pick("synth", ["synth"]));
    await effects[0].select(pick("fx1", ["fx"]));
    await effects[1].select(pick("fx2", ["fx"]));
    setStatus(audio.running ? status.textContent ?? "" : "Press a key or tap the keyboard to start the sound.");
  })();

  return {
    destroy() {
      destroyed = true;
      keyboard.destroy();
      midi.destroy();
      scope.destroy();
      audio.close();
      for (const fn of off.splice(0)) fn();
      el.classList.remove("lab-host");
      el.replaceChildren();
    },
  };
}
