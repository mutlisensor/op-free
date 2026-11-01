/**
 * opzsim in WebAssembly: one module per engine (`make -C sim wasm` -> sim/build/wasm/{engines,templates}/NAME.wasm),
 * holding the engine and the same host C as the opzsim CLI (sim/src, wasm entry points in sim/wasm/websim.c).
 *
 * - runScenario: opzsim's own main over an in-memory file system, so a `.scn` renders bit-identically to the CLI
 *   (the golden check in test/golden.test.ts proves it per engine).
 * - LiveEngine: the same host driven block by block, for the keyboard and knobs.
 */

import { ProcExit, Wasi, normalise } from "./wasi.ts";

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

export interface ScenarioResult {
  /** opzsim's exit code: 0 ok, 1 usage or I/O, 2 engine refused, 3 an expectation failed, 4 an ABI rule broken. */
  exitCode: number;
  log: string;
  wav: Uint8Array | null;
  stats: Record<string, unknown> | null;
}

export interface ScenarioOptions {
  /** Module name as opzsim prints it (the engine directory). */
  name: string;
  /** Absolute path of the scenario inside `files`. */
  scenario: string;
  /** Every file the scenario may open, absolute paths (the scenario, sample packs, preset files). */
  files: Map<string, Uint8Array>;
  slot?: number;
  trackmask?: number;
  extraArgs?: string[];
}

/** Run opzsim's main on a fresh instance (its globals start clean, as a new process's do). */
export function runScenario(module: WebAssembly.Module, o: ScenarioOptions): ScenarioResult {
  let log = "";
  const files = new Map(o.files);
  const wasi = new Wasi(files, (text) => void (log += text));
  const ex = instantiate(module, wasi);
  const args = ["opzsim", "-e", o.name, "-o", "/out.wav", "--stats", "/stats.json"];
  if (o.slot !== undefined) args.push("--slot", String(o.slot));
  if (o.trackmask !== undefined) args.push("--trackmask", String(o.trackmask));
  args.push(...(o.extraArgs ?? []), o.scenario);
  const enc = new TextEncoder();
  const argv = ex.malloc(4 * args.length);
  args.forEach((a, i) => {
    const b = enc.encode(a + "\0");
    const p = ex.malloc(b.length);
    new Uint8Array(ex.memory.buffer, p, b.length).set(b);
    new DataView(ex.memory.buffer).setUint32(argv + 4 * i, p, true);
  });
  let exitCode: number;
  try {
    exitCode = ex.opzsim_main(args.length, argv);
  } catch (e) {
    if (!(e instanceof ProcExit)) throw e;
    exitCode = e.code;
  }
  const wav = files.get("/out.wav") ?? null;
  const statsBytes = files.get("/stats.json");
  let stats: Record<string, unknown> | null = null;
  try {
    stats = statsBytes ? JSON.parse(new TextDecoder().decode(statsBytes)) : null;
  } catch {
    stats = null;
  }
  return { exitCode, log, wav: wav ? wav.slice() : null, stats };
}

/** One `expect` line of opzsim's log. */
export interface Expectation { line: number; what: string; ok: boolean }

export function parseExpectations(log: string): Expectation[] {
  const out: Expectation[] = [];
  for (const m of log.matchAll(/^expect {2}line (\d+): (.*): (ok|FAILED)$/gm))
    out.push({ line: Number(m[1]), what: m[2], ok: m[3] === "ok" });
  return out;
}

/** Warnings and errors of opzsim's log (state budget, ABI, voices left on, ...). */
export function problems(log: string): string[] {
  return log.split("\n").filter((l) => /^(warning|error)\s/.test(l));
}

/** The files a scenario opens besides itself (`sample FILE`, `preset_file PATH`), absolute and normalised. */
export function scenarioDependencies(scenarioPath: string, text: string): string[] {
  const dir = scenarioPath.slice(0, scenarioPath.lastIndexOf("/") + 1);
  const out: string[] = [];
  for (const raw of text.split("\n")) {
    const words = raw.replace(/#.*/, "").trim().split(/\s+/);
    const file = words[0] === "preset_file" ? words[1] : words[0] === "sample" && words[1] !== "builtin" && words[1] !== "unloaded" ? words[1] : null;
    if (file) out.push(normalise(file.startsWith("/") ? file : dir + file));
  }
  return out;
}

/** Interleaved stereo int32 samples of a 32-bit WAV written by opzsim (44-byte header). */
export function wavSamples(wav: Uint8Array): Int32Array {
  return new Int32Array(wav.buffer.slice(wav.byteOffset + 44, wav.byteOffset + wav.length));
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
