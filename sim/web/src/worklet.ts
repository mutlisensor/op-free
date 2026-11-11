/**
 * The audio thread: eight instrument tracks (each a LiveEngine with its own host, on its OP-Z track) and two effect
 * slots (FX1, FX2). Per 64-frame block every instrument renders; each track's FX1 and FX2 sends feed the effects'
 * inputs (IN_EXTERNAL, summed over the tracks) and its dry path goes to the mix with both returns (src/mixer.ts). An
 * effect whose input is a generator ignores the tracks; with no instrument both run on their generators. Q31 out of
 * the hosts, float to the speakers.
 */

import { Ramp, addBlock, gains } from "./mixer.ts";
import {
  FX_SLOTS, type FromWorklet, type FxSlot, INST_SLOTS, type InstSlot, type SlotName, type ToWorklet, type TrackMix, isInst,
} from "./messages.ts";
import { BLOCK, type InputKind, LiveEngine } from "./opzsim.ts";

declare function registerProcessor(name: string, ctor: unknown): void;
declare class AudioWorkletProcessor {
  readonly port: MessagePort;
}

// Chrome's AudioWorkletGlobalScope has no `performance`: Date.now() ticks in whole ms there, so every timing shown is
// an average since load (a coarse clock still averages right over many blocks).
const fineClock = typeof performance !== "undefined" && typeof performance.now === "function";
const nowMs: () => number = fineClock ? () => performance.now() : () => Date.now();
const Q31 = 1 / 2147483648;
const REPORT_BLOCKS = 64;              // stats to the page about 15 times a second
const SEND_BYTE: Record<FxSlot, number> = { fx1: 6, fx2: 7 };   // an instrument track's FX1 / FX2 send bytes
const FX_DRY_LEVEL = 12;               // on an FX track: the dry level of the tracks sending to it

class Inst {
  engine: LiveEngine | null = null;
  bytes = new Uint8Array(18);
  dry = new Ramp(1);
  sends: Record<FxSlot, Ramp> = { fx1: new Ramp(0), fx2: new Ramp(0) };
  block = new Int32Array(2 * BLOCK);
  peak = 0;
}

class Effect {
  engine: LiveEngine | null = null;
  /** the input is the tracks' sends (else a generator) */
  fromSend = true;
  bytes = new Uint8Array(18);
  send = new Int32Array(2 * BLOCK);
  extL = new Int32Array(BLOCK);
  extR = new Int32Array(BLOCK);
  peak = 0;
}

function peakOf(block: Int32Array, peak: number): number {
  for (let i = 0; i < block.length; i++) {
    const a = Math.abs(block[i] * Q31);
    if (a > peak) peak = a;
  }
  return peak;
}

class SimProcessor extends AudioWorkletProcessor {
  private inst = Object.fromEntries(INST_SLOTS.map((s) => [s, new Inst()])) as Record<InstSlot, Inst>;
  private fx: Record<FxSlot, Effect> = { fx1: new Effect(), fx2: new Effect() };
  private pending = new Float32Array(2 * BLOCK);
  private pendingAt = 2 * BLOCK;
  private log = "";
  private busyMs = 0;
  private blocks = 0;
  private mixBlock = new Int32Array(2 * BLOCK);

  constructor() {
    super();
    this.port.onmessage = (e: MessageEvent<ToWorklet>) => this.message(e.data);
  }

  private send(m: FromWorklet): void {
    this.port.postMessage(m);
  }

  private message(m: ToWorklet): void {
    try {
      this.handle(m);
    } catch (err) {
      this.log += `error   ${(err as Error).message}\n`;
      this.flush();
    }
  }

  private engine(name: SlotName): LiveEngine | null {
    return isInst(name) ? this.inst[name].engine : this.fx[name].engine;
  }

  private bytes(name: SlotName): Uint8Array {
    return isInst(name) ? this.inst[name].bytes : this.fx[name].bytes;
  }

  private handle(m: ToWorklet): void {
    switch (m.type) {
      case "load": {
        if (isInst(m.slot)) this.inst[m.slot].engine = null;
        else this.fx[m.slot].engine = null;
        if (!m.bytes) {
          this.send({ type: "loaded", slot: m.slot, ok: true, empty: true, isFx: !isInst(m.slot), log: "", fineClock });
          return;
        }
        let text = "";
        const engine = new LiveEngine(new WebAssembly.Module(m.bytes), m.track, (t) => {
          text += t;
          this.log += t;
        });
        if (engine.ok) {
          if (isInst(m.slot)) this.inst[m.slot].engine = engine;
          else {
            this.fx[m.slot].engine = engine;
            if (this.fx[m.slot].fromSend) engine.input("external");
          }
        }
        this.send({ type: "loaded", slot: m.slot, ok: engine.ok, empty: false, isFx: engine.isFx, log: text, fineClock });
        this.log = "";
        this.busyMs = this.blocks = 0;
        break;
      }
      case "note": {
        const target = this.engine(m.slot);
        if (m.on) target?.noteOn(m.note, m.velocity);
        else target?.noteOff(m.note);
        break;
      }
      case "param": {
        this.bytes(m.slot)[m.index] = m.value;
        this.engine(m.slot)?.param(m.index, m.value, m.immediate);
        if (m.immediate) this.settleGains();
        break;
      }
      case "bend":
        this.engine(m.slot)?.bend(m.cents);
        break;
      case "input": {
        const fx = this.fx[m.slot];
        fx.fromSend = m.kind === "send";
        if (fx.fromSend) fx.engine?.input("external");
        else fx.engine?.input(m.kind as InputKind, m.f0, m.f1, m.amp);
        break;
      }
      case "gain":
        this.fx[m.slot].engine?.gain(m.gain);
        break;
      case "tempo":
        for (const e of this.engines()) e.tempo(m.bpm);
        break;
      case "transport":
        for (const e of this.engines()) e.transport(m.play);
        break;
      case "panic":
        for (const e of this.engines()) for (let n = 0; n < 128; n++) e.noteOff(n);
        break;
    }
  }

  private engines(): LiveEngine[] {
    return [...INST_SLOTS.map((s) => this.inst[s].engine), this.fx.fx1.engine, this.fx.fx2.engine]
      .filter((e): e is LiveEngine => !!e);
  }

  /** Target gains of one track's send/return paths (src/mixer.ts); an effect not fed by the tracks takes no send. */
  private targets(s: InstSlot): TrackMix {
    const t = this.inst[s];
    const g = gains(FX_SLOTS.map((f) => ({
      sendByte: this.fx[f].engine && this.fx[f].fromSend ? t.bytes[SEND_BYTE[f]] : 0,
      dryLevelByte: this.fx[f].bytes[FX_DRY_LEVEL],
    })));
    return { sends: [g.sends[0], g.sends[1]], dry: g.dry };
  }

  private settleGains(): void {
    for (const s of INST_SLOTS) {
      const t = this.targets(s), inst = this.inst[s];
      FX_SLOTS.forEach((f, i) => (inst.sends[f].value = t.sends[i]));
      inst.dry.value = t.dry;
    }
  }

  /** One 64-frame block of the mix, as floats into this.pending. */
  private renderBlock(): void {
    const mix = this.mixBlock;
    mix.fill(0);
    for (const f of FX_SLOTS) this.fx[f].send.fill(0);
    for (const s of INST_SLOTS) {
      const inst = this.inst[s], t = this.targets(s);
      const [d0, d1] = inst.dry.step(t.dry);
      const ramps = FX_SLOTS.map((f, i) => inst.sends[f].step(t.sends[i]));
      if (!inst.engine) continue;
      inst.block.set(inst.engine.render());
      inst.peak = peakOf(inst.block, inst.peak);
      addBlock(inst.block, d0, d1, mix);
      FX_SLOTS.forEach((f, i) => {
        if (this.fx[f].engine && this.fx[f].fromSend) addBlock(inst.block, ramps[i][0], ramps[i][1], this.fx[f].send);
      });
    }
    const out = this.pending;
    for (let i = 0; i < 2 * BLOCK; i++) out[i] = mix[i] * Q31;
    for (const f of FX_SLOTS) {
      const fx = this.fx[f];
      if (!fx.engine) continue;
      if (fx.fromSend) {
        for (let i = 0; i < BLOCK; i++) {
          fx.extL[i] = fx.send[2 * i];
          fx.extR[i] = fx.send[2 * i + 1];
        }
        fx.engine.setExternal(fx.extL, fx.extR);
      }
      const r = fx.engine.render();
      fx.peak = peakOf(r, fx.peak);
      for (let i = 0; i < 2 * BLOCK; i++) out[i] += r[i] * Q31;
    }
  }

  private flush(): void {
    const slots: Partial<Record<SlotName, ReturnType<LiveEngine["stats"]> | null>> = {};
    const peaks: Partial<Record<SlotName, number>> = {};
    const mix: Partial<Record<InstSlot, TrackMix>> = {};
    for (const s of INST_SLOTS) {
      const inst = this.inst[s];
      slots[s] = inst.engine?.stats() ?? null;
      peaks[s] = inst.peak;
      inst.peak = 0;
      mix[s] = { sends: [inst.sends.fx1.value, inst.sends.fx2.value], dry: inst.dry.value };
    }
    for (const f of FX_SLOTS) {
      slots[f] = this.fx[f].engine?.stats() ?? null;
      peaks[f] = this.fx[f].peak;
      this.fx[f].peak = 0;
    }
    this.send({ type: "stats", slots, busyMs: this.busyMs, blocks: this.blocks, log: this.log, mix, peaks });
    this.log = "";
  }

  process(_inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    const out = outputs[0];
    const left = out[0], right = out[1] ?? out[0];
    if (!this.engines().length) {
      left.fill(0);
      right.fill(0);
      return true;
    }
    const t0 = nowMs();
    for (let n = 0; n < left.length; n++) {
      if (this.pendingAt >= 2 * BLOCK) {
        this.renderBlock();
        this.pendingAt = 0;
        if (++this.blocks % REPORT_BLOCKS === 0) this.flush();
      }
      left[n] = this.pending[this.pendingAt];
      right[n] = this.pending[this.pendingAt + 1];
      this.pendingAt += 2;
    }
    this.busyMs += nowMs() - t0;
    return true;
  }
}

registerProcessor("opzsim", SimProcessor);
