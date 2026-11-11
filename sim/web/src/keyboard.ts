/**
 * Note input: an on-screen keyboard (pointer), the computer keyboard (A W S E D F T G Y H U J K O L P ; ' play
 * the notes, Z and X move an octave) and Web MIDI input (any input port; no SysEx, so no HTTPS needed): the page routes
 * each message by channel (src/midimap.ts) and marks the selected track's MIDI notes here.
 */

const COMPUTER_KEYS = ["a", "w", "s", "e", "d", "f", "t", "g", "y", "h", "u", "j", "k", "o", "l", "p", ";", "'"];
const BLACK = new Set([1, 3, 6, 8, 10]);
const NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

export function noteName(n: number): string {
  return NAMES[n % 12] + (Math.floor(n / 12) - 1);
}

export interface NoteSink {
  (on: boolean, note: number, velocity: number): void;
}

export class Keyboard {
  readonly el: HTMLElement;
  private lo = 48;                                // first key on screen
  private count = 25;
  private cbase = 48;                             // the C the computer keys start from
  private fixedWindow = false;                    // drums: the screen stays on the OP-Z's 24 drum keys
  private held = new Map<string, number>();      // computer key -> note
  private pointerNote = new Map<number, number>(); // pointer id -> note
  private sounding = new Map<number, number>();  // note -> count of sources holding it
  velocity = 100;
  private readonly sink: NoteSink;
  private readonly onBaseChange: (first: number, last: number) => void;

  constructor(sink: NoteSink, onBaseChange: (first: number, last: number) => void = () => {}) {
    this.sink = sink;
    this.onBaseChange = onBaseChange;
    this.el = document.createElement("div");
    this.el.className = "keyboard";
    this.el.setAttribute("role", "group");
    this.el.setAttribute("aria-label", "Keyboard");
    this.build();
    window.addEventListener("keydown", (e) => this.key(e, true));
    window.addEventListener("keyup", (e) => this.key(e, false));
    window.addEventListener("blur", () => this.releaseAll());
  }

  /** Synth tracks: C3-C5 on screen, computer keys from C3. Drum tracks: F3-E5 (the OP-Z's drum keys), computer keys
   * from C4. Z and X move the computer keys (and a synth's screen) by an octave. */
  configure(drum: boolean): void {
    this.releaseAll();
    this.fixedWindow = drum;
    this.lo = drum ? 53 : 48;
    this.count = drum ? 24 : 25;
    this.cbase = drum ? 60 : 48;
    this.build();
  }

  private shift(octaves: number): void {
    this.releaseAll();
    this.cbase = Math.max(0, Math.min(108, this.cbase + 12 * octaves));
    if (!this.fixedWindow) this.lo = Math.min(127 - this.count, this.cbase);
    this.build();
  }

  private press(note: number, on: boolean, velocity = this.velocity): void {
    const count = this.sounding.get(note) ?? 0;
    if (on) {
      this.sounding.set(note, count + 1);
      if (count === 0) this.sink(true, note, velocity);
    } else if (count > 0) {
      this.sounding.set(note, count - 1);
      if (count === 1) this.sink(false, note, 0);
    }
    this.el.querySelector(`[data-note="${note}"]`)?.classList.toggle("down", (this.sounding.get(note) ?? 0) > 0);
  }

  /** From MIDI or a test: velocity 0 means off. */
  midi(on: boolean, note: number, velocity: number): void {
    this.press(note, on && velocity > 0, velocity);
  }

  /** Show a note held from MIDI (it is played by the page, not through this keyboard's sink). */
  mark(note: number, on: boolean): void {
    this.el.querySelector(`[data-note="${note}"]`)?.classList.toggle("midi", on);
  }

  clearMarks(): void {
    this.el.querySelectorAll(".midi").forEach((k) => k.classList.remove("midi"));
  }

  releaseAll(): void {
    for (const [note, count] of this.sounding) if (count > 0) this.sink(false, note, 0);
    this.sounding.clear();
    this.held.clear();
    this.pointerNote.clear();
    this.el.querySelectorAll(".down").forEach((k) => k.classList.remove("down"));
  }

  private key(e: KeyboardEvent, down: boolean): void {
    const t = e.target as HTMLElement | null;
    if (e.metaKey || e.ctrlKey || e.altKey) return;
    if (t && (t.tagName === "INPUT" || t.tagName === "SELECT" || t.tagName === "TEXTAREA" || t.isContentEditable)) return;
    const k = e.key.toLowerCase();
    if (down && (k === "z" || k === "x")) {
      this.shift(k === "z" ? -1 : 1);
      e.preventDefault();
      return;
    }
    const i = COMPUTER_KEYS.indexOf(k);
    if (i < 0) return;
    e.preventDefault();
    if (down) {
      if (e.repeat || this.held.has(k)) return;
      const note = this.cbase + i;
      this.held.set(k, note);
      this.press(note, true);
    } else {
      const note = this.held.get(k);
      if (note === undefined) return;
      this.held.delete(k);
      this.press(note, false);
    }
  }

  private build(): void {
    this.el.textContent = "";
    const keys = document.createElement("div");
    keys.className = "keys";
    for (let i = 0; i < this.count; i++) {
      const note = this.lo + i;
      const k = document.createElement("div");
      const black = BLACK.has(note % 12);
      k.className = black ? "key black" : "key white";
      k.dataset.note = String(note);
      k.setAttribute("aria-label", noteName(note));
      const hint = note >= this.cbase ? COMPUTER_KEYS[note - this.cbase] : undefined;
      k.innerHTML = `<span class="key-hint">${hint ? hint.toUpperCase() : ""}</span>${note % 12 === 0 || i === 0 ? `<span class="key-name">${noteName(note)}</span>` : ""}`;
      k.addEventListener("pointerdown", (e) => {
        e.preventDefault();
        k.setPointerCapture(e.pointerId);
        this.pointerNote.set(e.pointerId, note);
        this.press(note, true);
      });
      const up = (e: PointerEvent) => {
        const n = this.pointerNote.get(e.pointerId);
        if (n === undefined) return;
        this.pointerNote.delete(e.pointerId);
        this.press(n, false);
      };
      k.addEventListener("pointerup", up);
      k.addEventListener("pointercancel", up);
      keys.append(k);
    }
    this.el.append(keys);
    this.onBaseChange(this.cbase, this.cbase + COMPUTER_KEYS.length - 1);
  }
}

/** Web MIDI input from every input port (and ports plugged in later). Returns the port names, or a reason it is
 * unavailable. */
export async function connectMidi(onMessage: (data: Uint8Array) => void,
  onPorts: (ports: string[]) => void = () => {}): Promise<{ ports: string[]; error?: string }> {
  if (!("requestMIDIAccess" in navigator)) return { ports: [], error: "this browser has no Web MIDI (use Chrome or Edge)" };
  try {
    const access = await navigator.requestMIDIAccess();
    const attach = () => {
      const names: string[] = [];
      for (const input of access.inputs.values()) {
        names.push(input.name ?? "MIDI input");
        input.onmidimessage = (m) => {
          if (m.data && m.data.length) onMessage(m.data);
        };
      }
      return names;
    };
    const ports = attach();
    access.onstatechange = () => onPorts(attach());
    return { ports };
  } catch (e) {
    return { ports: [], error: (e as Error).message || "MIDI access refused" };
  }
}
