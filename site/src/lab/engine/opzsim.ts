/**
 * opzsim in WebAssembly, ported from sim/web/src/opzsim.ts: one module per engine (`modules/<dir>.wasm`), holding the
 * engine and the same host C as the opzsim CLI (sim/src, wasm entry points in sim/wasm/websim.c). LiveEngine drives
 * that host block by block for the keyboard and knobs. (The scenario runner stays in sim/web: the site plays, it
 * doesn't test.)
 */

import { Wasi } from "./wasi.ts";

/** sim/src/host.h, enum IN_* */
export const Input = { silence: 0, sine: 1, saw: 2, noise: 3, sweep: 4, impulse: 5, external: 7 } as const;
export type InputKind = keyof typeof Input;

/** Tracks as opzsim names them (index = OP-Z track number - 1). */
export const TRACKS = ["kick", "snare", "perc", "sample", "bass", "lead", "arp", "chord", "fx1", "fx2", "tape", "master"] as const;

export const BLOCK = 64;
export const SAMPLE_RATE = 44100;

interface Exports {
  memory: WebAssembly.Memory;
  _initialize(): void;
  malloc(n: number): number;
  opzsim_main(argc: number, argv: number): number;
  ws_open(track: number, generous: number): number;
  ws_mem_request(): number;
  ws_input(kind: number, f0: number, f1: number, amp: number): void;
  ws_ext_in(): number;
  ws_gain(g: number): void;
  ws_tempo(bpm: number): void;
  ws_transport(play: number): void;
  ws_note_on(track: number, note: number, velocity: number): void;
  ws_note_off(track: number, note: number): void;
  ws_bend(track: number, cents: number): void;
  ws_param(track: number, index: number, byte: number, immediate: number): void;
  ws_render(): number;
  ws_stat(which: number, track: number): number;
  ws_time(which: number): number;
}

function instantiate(module: WebAssembly.Module, wasi: Wasi): Exports {
  const instance = new WebAssembly.Instance(module, { wasi_snapshot_preview1: wasi.imports as WebAssembly.ModuleImports });
  const ex = instance.exports as unknown as Exports;
  wasi.bind(ex.memory);
  ex._initialize();
  return ex;
}

export interface LiveStats {
  active: number;
  stateViolations: number;
  addViolations: number;
  stolen: number;
  maxUsed: number;
  voices: number;
  blocks: number;
  instNs: number;
  voiceBlocks: number;
  fxNs: number;
  fxBlocks: number;
}

/** The host driven live: open one track, then events between 64-frame blocks. */
export class LiveEngine {
  private ex: Exports;
  readonly isFx: boolean;
  readonly ok: boolean;
  log = "";

  readonly track: number;

  constructor(module: WebAssembly.Module, track: number, onLog?: (text: string) => void, generous = false) {
    this.track = track;
    const wasi = new Wasi(new Map(), (text) => {
      this.log += text;
      onLog?.(text);
    });
    this.ex = instantiate(module, wasi);
    const rc = this.ex.ws_open(track, generous ? 1 : 0);
    this.ok = rc >= 0;
    this.isFx = rc === 1;
  }

  get memRequest(): number {
    return this.ex.ws_mem_request();
  }

  noteOn(note: number, velocity = 100, track = this.track): void {
    this.ex.ws_note_on(track, note, velocity);
  }

  noteOff(note: number, track = this.track): void {
    this.ex.ws_note_off(track, note);
  }

  /** immediate: load unsmoothed (a preset), else smoothed 1/64 per block like a knob. */
  param(index: number, byte: number, immediate = false, track = this.track): void {
    this.ex.ws_param(track, index, byte, immediate ? 1 : 0);
  }

  bend(cents: number, track = this.track): void {
    this.ex.ws_bend(track, cents);
  }

  input(kind: InputKind, f0 = 0, f1 = 0, amp = 0.5): void {
    this.ex.ws_input(Input[kind], f0, f1, amp);
  }

  gain(g: number): void {
    this.ex.ws_gain(g);
  }

  tempo(bpm: number): void {
    this.ex.ws_tempo(bpm);
  }

  transport(play: boolean): void {
    this.ex.ws_transport(play ? 1 : 0);
  }

  /** IN_EXTERNAL: the next block's input, left then right (64 + 64 values, Q31). */
  setExternal(left: Int32Array, right: Int32Array): void {
    const p = this.ex.ws_ext_in();
    const buf = new Int32Array(this.ex.memory.buffer, p, 2 * BLOCK);
    buf.set(left.subarray(0, BLOCK), 0);
    buf.set(right.subarray(0, BLOCK), BLOCK);
  }

  /** One block, interleaved stereo Q31 (a view: copy it before the next call). */
  render(): Int32Array {
    const p = this.ex.ws_render();
    return new Int32Array(this.ex.memory.buffer, p, 2 * BLOCK);
  }

  stats(): LiveStats {
    const s = (i: number) => this.ex.ws_stat(i, this.track);
    const t = (i: number) => this.ex.ws_time(i);
    return {
      active: s(0), stateViolations: s(1), addViolations: s(2), stolen: s(3), maxUsed: s(4), voices: s(5), blocks: s(6),
      instNs: t(0), voiceBlocks: t(1), fxNs: t(2), fxBlocks: t(3),
    };
  }
}
