/**
 * OP-Z projects and patterns: `projects/projectNN.opz`, the pattern bank and the project-global block. Port of
 * tools/opz_project.py (same layout, checks and generators; shared vectors in test-vectors/project.json). Layout and
 * evidence: research/patterns.md. Object keys of the read-outs follow the Python tool, so the vectors compare
 * directly.
 */

export const MAJOR = 73;
export const MINOR = 7;
export const KNOWN_MAJORS = [70, 71, 72, 73];
export const GLOBAL_SIZE = 568;
export const PATTERN_SIZE = 21_392;
export const PATTERNS = 16;
export const TRACKS = 16;
export const STEPS = 16;
export const BANK_SIZE = PATTERN_SIZE * PATTERNS;
export const FILE_SIZE = 4 + GLOBAL_SIZE + BANK_SIZE + 4;
export const OLD_FILE_SIZE = FILE_SIZE - 4;
export const PROJECTS = 10;

export const TRACK_NAMES = ["kick", "snare", "perc", "sample", "bass", "lead", "arp", "chord", "fx1", "fx2", "tape",
  "master", "perform", "module", "lights", "motion"] as const;
export const NOTES_PER_STEP = [2, 2, 2, 2, 4, 4, 8, 4, 1, 1, 1, 4, 6, 6, 4, 4];
export const NOTE_OFFSETS = [0, 2, 4, 6, 8, 12, 16, 24, 28, 29, 30, 31, 35, 41, 47, 51];
export const SLOTS_PER_STEP = 55;
export const PARAMS = ["p1", "p2", "attack", "decay", "sustain", "release", "fx1", "fx2", "filter", "resonance", "pan",
  "level", "portamento", "lfo_depth", "lfo_speed", "lfo_value", "lfo_shape", "note_style"] as const;
export const COMPONENTS = ["pulse", "spark", "jump", "velocity", "ramp_up", "ramp_down", "random", "glide", "multiply",
  "sweep", "pulse_hold", "tonality", "trigger_spark", "component_spark"] as const;
export const COMPONENT_DEFAULTS = [4, 2, 4, 5, 4, 4, 4, 4, 2, 2, 4, 4, 2, 2];
export const STEP_UNITS = 6200;
export const LEVELS = ["drum", "synth", "punch", "master"] as const;

const NOTES_AT = 192;
const STEPS_AT = 7_232;
const SOUND_AT = 21_056;
const MUTES_AT = 21_344;
const TAPE_AT = 21_384;
const MUTE_GROUP_AT = 21_388;
const STEP_RECORD = 54;
const TRACK_RECORD = 12;
const CHAINS = 16;
const CHAIN_RECORD = 32;
const CHAIN_MAX = 31;
const TEMPO_AT = 516;
const SWING_AT = 561;
const METRONOME_LEVEL_AT = 562;
const METRONOME_SOUND_AT = 563;
const SELECTED_CHAIN_AT = 564;
const STEP_LENGTH_MAX = 10;

export type Param = (typeof PARAMS)[number];
export type Component = (typeof COMPONENTS)[number];

export interface NoteIn { note: number; velocity?: number; duration?: number; micro?: number; age?: number }
export interface NoteRecord { note: number; velocity: number; duration: number; micro: number; age: number }
export interface TrackRecord {
  plug: number; step_count: number; timing: number; step_length: number; quantize: number; byte8: number;
  note_length: number; last_note: number; byte11: number;
}
export interface StepRecord { mask: number; components: Record<string, number>; locks: Record<string, number> }
export interface Summary {
  version: string; tempo: number; swing: number; levels: Record<string, number>; metronome: number[];
  selected_chain: number | null; chains: Record<string, number[]>; patterns_with_notes: number[];
}

export class FormatError extends Error {
  override name = "FormatError";
}

/** GlobalData::GlobalData: chains FF, levels 80 80 00 00, 120 BPM, swing 7F, metronome 100, no chain selected. */
export function defaultGlobal(): Uint8Array {
  const g = new Uint8Array(GLOBAL_SIZE);
  g.fill(0xff, 0, CHAINS * CHAIN_RECORD);
  g.set([0x80, 0x80, 0, 0], 512);
  new DataView(g.buffer).setUint16(TEMPO_AT, 120, true);
  g[SWING_AT] = 0x7f;
  g[METRONOME_LEVEL_AT] = 100;
  g[SELECTED_CHAIN_AT] = 0xff;
  return g;
}

/** PatternData::PatternData: no plug, 16 steps, unused notes, no locks or components, tape 00FF, master 00F0. */
export function defaultPattern(): Uint8Array {
  const p = new Uint8Array(PATTERN_SIZE);
  const v = new DataView(p.buffer);
  for (let t = 0; t < TRACKS; t++) p.set([0, 0, 0, 0, 16, 5, 1, 0, 0, 0x40, 60, 0], t * TRACK_RECORD);
  for (let n = 0; n < STEPS * SLOTS_PER_STEP; n++) {
    v.setInt32(NOTES_AT + n * 8, 2560, true);
    p.set([0xff, 100, 0, 0], NOTES_AT + n * 8 + 4);
  }
  for (let r = 0; r < TRACKS * STEPS; r++) p.set(COMPONENT_DEFAULTS, STEPS_AT + r * STEP_RECORD + 2);
  p.fill(0, 19_330, 19_346);
  v.setUint16(TAPE_AT, 0x00ff, true);
  v.setUint16(TAPE_AT + 2, 0x00f0, true);
  return p;
}

function check(p = 0, t = 0, s = 0): number {
  if (![p, t, s].every((x) => Number.isInteger(x) && x >= 0 && x < 16)) {
    throw new FormatError("pattern, track and step are 0..15");
  }
  return t;
}

function byte(value: number, what: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 255) throw new FormatError(`${what} must be 0..255`);
  return value;
}

function noteAt(p: number, t: number, s: number, slot: number): number {
  return p * PATTERN_SIZE + NOTES_AT + (s * SLOTS_PER_STEP + NOTE_OFFSETS[t]! + slot) * 8;
}

function stepAt(p: number, t: number, s: number): number {
  check(p, t, s);
  return p * PATTERN_SIZE + STEPS_AT + (t * STEPS + s) * STEP_RECORD;
}

/** Python's round(): halves go to the even neighbour. */
function roundHalfEven(x: number): number {
  const f = Math.floor(x);
  const d = x - f;
  return d > 0.5 || (d === 0.5 && f % 2 !== 0) ? f + 1 : f;
}

/** A project file in memory, kept as raw bytes so unknown fields survive; `toBytes()` reproduces a parsed file. */
export class Project {
  glob: Uint8Array;
  bank: Uint8Array;
  major: number;
  minor: number | null;

  constructor(glob?: Uint8Array, bank?: Uint8Array, major = MAJOR, minor: number | null = MINOR) {
    this.glob = glob ? glob.slice() : defaultGlobal();
    if (bank) {
      this.bank = bank.slice();
    } else {
      this.bank = new Uint8Array(BANK_SIZE);
      const one = defaultPattern();
      for (let p = 0; p < PATTERNS; p++) this.bank.set(one, p * PATTERN_SIZE);
    }
    if (this.glob.length !== GLOBAL_SIZE || this.bank.length !== BANK_SIZE) {
      throw new FormatError(`global block must be ${GLOBAL_SIZE} bytes and the bank ${BANK_SIZE}`);
    }
    this.major = major;
    this.minor = minor;
  }

  static parse(data: Uint8Array): Project {
    if (data.length !== FILE_SIZE && data.length !== OLD_FILE_SIZE) {
      throw new FormatError(`a project file is ${FILE_SIZE} bytes (${OLD_FILE_SIZE} before version 73), this one is ${data.length}`);
    }
    const v = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const major = v.getUint32(0, true);
    if (!KNOWN_MAJORS.includes(major)) {
      throw new FormatError(`unknown project version ${major} (the OS would load default global data and no patterns)`);
    }
    const minor = data.length === FILE_SIZE ? v.getUint32(4 + GLOBAL_SIZE + BANK_SIZE, true) : null;
    if (major === MAJOR && minor === null) throw new FormatError("a version 73 file ends with its minor version word");
    return new Project(data.subarray(4, 4 + GLOBAL_SIZE), data.subarray(4 + GLOBAL_SIZE, 4 + GLOBAL_SIZE + BANK_SIZE),
      major, minor);
  }

  toBytes(): Uint8Array {
    const out = new Uint8Array(this.minor === null ? OLD_FILE_SIZE : FILE_SIZE);
    const v = new DataView(out.buffer);
    v.setUint32(0, this.major, true);
    out.set(this.glob, 4);
    out.set(this.bank, 4 + GLOBAL_SIZE);
    if (this.minor !== null) v.setUint32(4 + GLOBAL_SIZE + BANK_SIZE, this.minor, true);
    return out;
  }

  get version(): string {
    return `${this.major}.${this.minor ?? 0}`;
  }

  private get b(): DataView {
    return new DataView(this.bank.buffer, this.bank.byteOffset, this.bank.byteLength);
  }

  pattern(p: number): Uint8Array {
    check(p);
    return this.bank.slice(p * PATTERN_SIZE, (p + 1) * PATTERN_SIZE);
  }

  track(p: number, t: number): TrackRecord {
    check(p, t);
    const at = p * PATTERN_SIZE + t * TRACK_RECORD;
    const r = this.bank;
    return {
      plug: this.b.getUint32(at, true), step_count: r[at + 4]!, timing: r[at + 5]!, step_length: r[at + 6]!,
      quantize: r[at + 7]!, byte8: r[at + 8]!, note_length: r[at + 9]!, last_note: r[at + 10]!, byte11: r[at + 11]!,
    };
  }

  notes(p: number, t: number, s: number): NoteRecord[] {
    const out: NoteRecord[] = [];
    for (let slot = 0; slot < NOTES_PER_STEP[check(p, t, s)]!; slot++) {
      const at = noteAt(p, t, s, slot);
      const note = this.bank[at + 4]!;
      if (note === 0xff) continue;
      out.push({ note, velocity: this.bank[at + 5]!, duration: this.b.getInt32(at, true), micro: this.b.getInt8(at + 6),
        age: this.bank[at + 7]! });
    }
    return out;
  }

  step(p: number, t: number, s: number): StepRecord {
    const at = stepAt(p, t, s);
    const mask = this.b.getUint16(at, true);
    const components: Record<string, number> = {};
    const locks: Record<string, number> = {};
    COMPONENTS.forEach((name, i) => { if ((mask >> i) & 1) components[name] = this.bank[at + 2 + i]!; });
    PARAMS.forEach((name, i) => { if (this.bank[at + 36 + i]) locks[name] = this.bank[at + 18 + i]!; });
    return { mask, components, locks };
  }

  sound(p: number, t: number): number[] {
    check(p, t);
    const at = p * PATTERN_SIZE + SOUND_AT + t * PARAMS.length;
    return Array.from(this.bank.subarray(at, at + PARAMS.length));
  }

  muteGroup(p: number): number {
    check(p);
    return this.bank[p * PATTERN_SIZE + MUTE_GROUP_AT]!;
  }

  muted(p: number, t: number, group?: number): boolean {
    check(p, t);
    const g = Math.min(group ?? this.muteGroup(p), 9);
    return ((this.bank[p * PATTERN_SIZE + MUTES_AT + g * 4 + (t >> 2)]! >> ((t % 4) * 2 + 1)) & 1) === 1;
  }

  routing(p: number): [number, number] {
    check(p);
    const at = p * PATTERN_SIZE + TAPE_AT;
    return [this.b.getUint16(at, true), this.b.getUint16(at + 2, true)];
  }

  patternHasNotes(p: number): boolean {
    const base = p * PATTERN_SIZE + NOTES_AT;
    for (let n = 0; n < STEPS * SLOTS_PER_STEP; n++) if (this.bank[base + n * 8 + 4] !== 0xff) return true;
    return false;
  }

  chain(slot: number): number[] {
    if (!Number.isInteger(slot) || slot < 0 || slot >= CHAINS) throw new FormatError("chain slot must be 0..15");
    const rec = Array.from(this.glob.subarray(slot * CHAIN_RECORD, (slot + 1) * CHAIN_RECORD));
    const end = rec.indexOf(0xff);
    return end < 0 ? rec : rec.slice(0, end);
  }

  get tempo(): number {
    return new DataView(this.glob.buffer, this.glob.byteOffset).getUint16(TEMPO_AT, true);
  }

  summary(): Summary {
    const g = this.glob;
    const chains: Record<string, number[]> = {};
    for (let n = 0; n < CHAINS; n++) if (this.chain(n).length) chains[String(n)] = this.chain(n);
    const levels: Record<string, number> = {};
    LEVELS.forEach((name, i) => (levels[name] = g[512 + i]!));
    const patterns: number[] = [];
    for (let p = 0; p < PATTERNS; p++) if (this.patternHasNotes(p)) patterns.push(p);
    return {
      version: this.version, tempo: this.tempo, swing: g[SWING_AT]!, levels,
      metronome: [g[METRONOME_LEVEL_AT]!, g[METRONOME_SOUND_AT]!],
      selected_chain: g[SELECTED_CHAIN_AT] === 0xff ? null : g[SELECTED_CHAIN_AT]!, chains, patterns_with_notes: patterns,
    };
  }

  /** Replace a step's notes (note 0-127, velocity 1-127, duration > 0, micro ticks -12..+11, age 0-255). */
  setNotes(p: number, t: number, s: number, notes: NoteIn[]): void {
    if (notes.length > NOTES_PER_STEP[check(p, t, s)]!) {
      throw new FormatError(`${TRACK_NAMES[t]} holds at most ${NOTES_PER_STEP[t]} notes per step`);
    }
    for (const n of notes) {
      const micro = n.micro ?? 0;
      const ok = [n.note, n.velocity ?? 100, n.duration ?? STEP_UNITS, micro, n.age ?? 0].every(Number.isInteger);
      if (!ok || n.note < 0 || n.note > 127 || (n.velocity ?? 100) < 1 || (n.velocity ?? 100) > 127 ||
          (n.duration ?? STEP_UNITS) <= 0 || micro < -12 || micro > 11 || (n.age ?? 0) < 0 || (n.age ?? 0) > 255) {
        throw new FormatError(`note out of range: ${JSON.stringify(n)}`);
      }
    }
    for (let slot = 0; slot < NOTES_PER_STEP[t]!; slot++) {
      const at = noteAt(p, t, s, slot);
      const n = notes[slot];
      if (n) {
        this.b.setInt32(at, n.duration ?? STEP_UNITS, true);
        this.bank[at + 4] = n.note;
        this.bank[at + 5] = n.velocity ?? 100;
        this.b.setInt8(at + 6, (n.micro ?? 0) * 8);
        this.bank[at + 7] = n.age ?? 0;
      } else {
        this.bank[at + 4] = 0xff;
      }
    }
  }

  setComponent(p: number, t: number, s: number, name: Component, value: number | null): void {
    const at = stepAt(p, t, s);
    const i = COMPONENTS.indexOf(name);
    if (i < 0) throw new FormatError(`unknown component ${name}`);
    let mask = this.b.getUint16(at, true);
    if (value === null) {
      mask &= ~(1 << i);
    } else {
      this.bank[at + 2 + i] = byte(value, "component value");
      mask |= 1 << i;
    }
    this.b.setUint16(at, mask, true);
  }

  setLock(p: number, t: number, s: number, param: Param, value: number | null): void {
    const at = stepAt(p, t, s);
    const i = PARAMS.indexOf(param);
    if (i < 0) throw new FormatError(`unknown parameter ${param}`);
    if (value === null) {
      this.bank[at + 36 + i] = 0;
    } else {
      this.bank[at + 18 + i] = byte(value, "lock value");
      this.bank[at + 36 + i] = 0xff;
    }
  }

  setSound(p: number, t: number, values: number[]): void {
    const v = values.map((x) => byte(x, "sound value"));
    if (v.length !== PARAMS.length) throw new FormatError("a sound is 18 values");
    check(p, t);
    this.bank.set(v, p * PATTERN_SIZE + SOUND_AT + t * PARAMS.length);
  }

  setTrack(p: number, t: number, o: { step_count?: number; step_length?: number; plug?: number; note_length?: number;
    quantize?: number }): void {
    check(p, t);
    const at = p * PATTERN_SIZE + t * TRACK_RECORD;
    if (o.step_count !== undefined) {
      if (!Number.isInteger(o.step_count) || o.step_count < 1 || o.step_count > STEPS) {
        throw new FormatError("step count must be 1..16");
      }
      this.bank[at + 4] = o.step_count;
    }
    if (o.step_length !== undefined) {
      if (!Number.isInteger(o.step_length) || o.step_length < 1 || o.step_length > STEP_LENGTH_MAX) {
        throw new FormatError(`step length must be 1..${STEP_LENGTH_MAX}`);
      }
      this.bank[at + 6] = o.step_length;
    }
    if (o.plug !== undefined) {
      if (!Number.isInteger(o.plug) || o.plug <= 0 || o.plug > 0xffffffff) throw new FormatError("plug id must be a non-zero u32");
      this.b.setUint32(at, o.plug, true);
    }
    if (o.note_length !== undefined) this.bank[at + 9] = byte(o.note_length, "note length");
    if (o.quantize !== undefined) this.bank[at + 7] = byte(o.quantize, "quantize");
  }

  setMuted(p: number, t: number, muted: boolean, group?: number): void {
    check(p, t);
    const g = Math.min(group ?? this.muteGroup(p), 9);
    const at = p * PATTERN_SIZE + MUTES_AT + g * 4 + (t >> 2);
    const bit = 1 << ((t % 4) * 2 + 1);
    this.bank[at] = muted ? this.bank[at]! | bit : this.bank[at]! & ~bit;
  }

  copyPattern(src: number, dst: number): void {
    check(dst);
    this.bank.set(this.pattern(src), dst * PATTERN_SIZE);
  }

  /** Saved chain slot 0-14 (the device shows 1-15), up to 31 zero-based patterns; [] clears it. */
  setChain(slot: number, patterns: number[]): void {
    if (!Number.isInteger(slot) || slot < 0 || slot >= CHAINS - 1) {
      throw new FormatError("saved chain slot must be 0..14 (record 15 is the live chain)");
    }
    if (patterns.length > CHAIN_MAX || patterns.some((v) => !Number.isInteger(v) || v < 0 || v >= PATTERNS)) {
      throw new FormatError(`a chain holds up to ${CHAIN_MAX} patterns 0..15`);
    }
    this.glob.fill(0xff, slot * CHAIN_RECORD, (slot + 1) * CHAIN_RECORD);
    this.glob.set(patterns, slot * CHAIN_RECORD);
  }

  setTempo(bpm: number): void {
    if (!Number.isInteger(bpm) || bpm < 40 || bpm > 200) throw new FormatError("tempo must be 40..200 BPM");
    new DataView(this.glob.buffer, this.glob.byteOffset).setUint16(TEMPO_AT, bpm, true);
  }

  /** What would make this file unsafe to hand to the OP-Z: anything outside what the OS writes itself. */
  problems(): string[] {
    const out: string[] = [];
    if (this.major !== MAJOR || this.minor !== MINOR) {
      out.push(`version ${this.version}: only ${MAJOR}.${MINOR} files are written (older ones load through the OS's upgrade path)`);
    }
    for (let p = 0; p < PATTERNS; p++) {
      for (let t = 0; t < TRACKS; t++) {
        const tr = this.track(p, t);
        const where = `pattern ${p + 1} ${TRACK_NAMES[t]}`;
        if (tr.step_count < 1 || tr.step_count > STEPS) out.push(`${where}: step count ${tr.step_count}`);
        if (tr.step_length < 1 || tr.step_length > STEP_LENGTH_MAX) out.push(`${where}: step length ${tr.step_length}`);
        for (let s = 0; s < STEPS; s++) {
          for (let slot = 0; slot < NOTES_PER_STEP[t]!; slot++) {
            const at = noteAt(p, t, s, slot);
            const note = this.bank[at + 4]!;
            if (note === 0xff) continue;
            const velocity = this.bank[at + 5]!;
            const micro = this.b.getInt8(at + 6);
            if (note > 127 || velocity > 127 || micro % 8 !== 0 || micro < -96 || micro > 88) {
              out.push(`${where} step ${s + 1}: note ${note} velocity ${velocity} micro ${micro}`);
            }
          }
          const at = stepAt(p, t, s);
          if (this.b.getUint16(at, true) & 0xc000) out.push(`${where} step ${s + 1}: reserved component bits`);
          if (this.bank.subarray(at + 36, at + 54).some((x) => x !== 0 && x !== 0xff)) {
            out.push(`${where} step ${s + 1}: lock enable not 00/FF`);
          }
        }
      }
      if (this.muteGroup(p) > 9) out.push(`pattern ${p + 1}: mute group ${this.muteGroup(p)}`);
    }
    for (let n = 0; n < CHAINS; n++) {
      const chain = this.chain(n);
      if (chain.some((v) => v >= PATTERNS) || chain.length > CHAIN_MAX) {
        const rec = Array.from(this.glob.subarray(n * CHAIN_RECORD, (n + 1) * CHAIN_RECORD));
        out.push(`chain ${n + 1}: ${rec.map((x) => x.toString(16).padStart(2, "0")).join("")}`);
      }
    }
    if (this.tempo < 40 || this.tempo > 200) out.push(`tempo ${this.tempo}`);
    const sel = this.glob[SELECTED_CHAIN_AT]!;
    if (sel !== 0xff && sel >= CHAINS - 1) out.push(`selected chain ${sel}`);
    return out;
  }
}

/** `hits` onsets spread evenly over `steps` (Bresenham form of Bjorklund: E(3,8) = x..x..x.), rotated right. */
export function euclid(hits: number, steps = 16, rotate = 0): boolean[] {
  if (!(steps > 0 && steps <= 64 && hits >= 0 && hits <= steps)) throw new FormatError("need 0 <= hits <= steps <= 64");
  const base = Array.from({ length: steps }, (_, i) => (i * hits) % steps < hits);
  const r = ((rotate % steps) + steps) % steps;
  return r ? [...base.slice(-r), ...base.slice(0, -r)] : base;
}

/** Replace one track's notes in one pattern with `hits` (one entry per step, up to 16); sets the step count. */
export function writeRhythm(project: Project, p: number, t: number, hits: boolean[], note = 60, velocity = 100,
  length = 1.0): void {
  if (hits.length < 1 || hits.length > STEPS) throw new FormatError("a pattern track holds 1..16 steps");
  for (let s = 0; s < STEPS; s++) {
    project.setNotes(p, t, s, s < hits.length && hits[s] ? [{ note, velocity, duration: roundHalfEven(length * STEP_UNITS) }] : []);
  }
  project.setTrack(p, t, { step_count: hits.length });
}

/** F-18: up to 64 steps of one track as consecutive patterns from `first`, plus a saved chain over them. */
export function writePages(project: Project, t: number, steps: NoteIn[][], first: number, chainSlot: number): number[] {
  const pages = Math.ceil(steps.length / STEPS);
  if (steps.length === 0 || steps.length > 4 * STEPS || first + pages > PATTERNS) {
    throw new FormatError(`1..64 steps, and the pages must fit in patterns ${first + 1}..16`);
  }
  const used = Array.from({ length: pages }, (_, n) => first + n);
  used.forEach((p, n) => {
    if (p !== first) project.copyPattern(first, p);
    const page = steps.slice(n * STEPS, (n + 1) * STEPS);
    for (let s = 0; s < STEPS; s++) project.setNotes(p, t, s, page[s] ?? []);
    project.setTrack(p, t, { step_count: page.length });
  });
  project.setChain(chainSlot, used);
  return used;
}

/** Device path of project 1-10; the OS reloads any "projects/projectNN" write into RAM slot NN-1 unchecked. */
export function projectPath(number: number): string {
  if (!Number.isInteger(number) || number < 1 || number > PROJECTS) throw new FormatError(`projects are numbered 1..${PROJECTS}`);
  return `projects/project${String(number).padStart(2, "0")}.opz`;
}

/** File-service download id of project 1-10: 3..12. */
export function projectFileId(number: number): number {
  projectPath(number);
  return number + 2;
}
