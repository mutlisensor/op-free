/**
 * The audio thread: a synth slot (an instrument on its track) and two effect slots (FX1, FX2), each a LiveEngine with
 * its own host. Per 64-frame block the synth renders; its FX1 and FX2 sends feed the effects' inputs (IN_EXTERNAL)
 * and its dry path goes to the mix with both returns (src/mixer.ts). An effect whose input is a generator ignores the
 * synth; with no synth both run on their generators. Q31 out of the hosts, float to the speakers.
 */

import { Ramp, gains, scaleBlock } from "./mixer.ts";
import { FX_SLOTS, type FromWorklet, type FxSlot, type SlotName, type ToWorklet } from "./messages.ts";
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
const SEND_BYTE: Record<FxSlot, number> = { fx1: 6, fx2: 7 };   // the synth track's FX1 / FX2 send bytes
const FX_DRY_LEVEL = 12;               // on an FX track: the dry level of the tracks sending to it

class Effect {
  engine: LiveEngine | null = null;
  /** the input is the synth's send (else a generator) */
  fromSend = true;
  bytes = new Uint8Array(18);
  ramp = new Ramp(0);
  send = new Int32Array(2 * BLOCK);
  extL = new Int32Array(BLOCK);
  extR = new Int32Array(BLOCK);
}

class SimProcessor extends AudioWorkletProcessor {
  private synth: LiveEngine | null = null;
  private fx: Record<FxSlot, Effect> = { fx1: new Effect(), fx2: new Effect() };
  private synthBytes = new Uint8Array(18);
  private dryRamp = new Ramp(1);
  private pending = new Float32Array(2 * BLOCK);
  private pendingAt = 2 * BLOCK;
  private log = "";
  private busyMs = 0;
  private blocks = 0;
  private synthBlock = new Int32Array(2 * BLOCK);
  private dryBlock = new Int32Array(2 * BLOCK);

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
    return name === "synth" ? this.synth : this.fx[name].engine;
  }

  private handle(m: ToWorklet): void {
    switch (m.type) {
      case "load": {
        if (m.slot === "synth") this.synth = null;
        else this.fx[m.slot].engine = null;
        if (!m.bytes) {
          this.send({ type: "loaded", slot: m.slot, ok: true, empty: true, isFx: m.slot !== "synth", log: "", fineClock });
          return;
        }
        let text = "";
        const engine = new LiveEngine(new WebAssembly.Module(m.bytes), m.track, (t) => {
          text += t;
          this.log += t;
        });
        if (engine.ok) {
          if (m.slot === "synth") this.synth = engine;
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
        (m.slot === "synth" ? this.synthBytes : this.fx[m.slot].bytes)[m.index] = m.value;
        this.engine(m.slot)?.param(m.index, m.value, m.immediate);
        if (m.immediate) this.settleGains();
        break;
      }
      case "bend":
        for (const e of this.engines()) e.bend(m.cents);
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
    return [this.synth, this.fx.fx1.engine, this.fx.fx2.engine].filter((e): e is LiveEngine => !!e);
  }

  /** Target gains of the send/return paths (src/mixer.ts); an effect not fed by the synth takes no send. */
  private targets(): { sends: [number, number]; dry: number } {
    const fed = FX_SLOTS.map((s) => !!this.synth && !!this.fx[s].engine && this.fx[s].fromSend);
    const g = gains(FX_SLOTS.map((s, i) => ({
      sendByte: fed[i] ? this.synthBytes[SEND_BYTE[s]] : 0,
      dryLevelByte: this.fx[s].bytes[FX_DRY_LEVEL],
    })));
    return { sends: [g.sends[0], g.sends[1]], dry: g.dry };
  }

  private settleGains(): void {
    const t = this.targets();
    FX_SLOTS.forEach((s, i) => (this.fx[s].ramp.value = t.sends[i]));
    this.dryRamp.value = t.dry;
  }

  /** One 64-frame block of the mix, as floats into this.pending. */
  private renderBlock(): void {
    const t = this.targets();
    const [d0, d1] = this.dryRamp.step(t.dry);
    const out = this.pending;
    out.fill(0);
    if (this.synth) {
      this.synthBlock.set(this.synth.render());
      scaleBlock(this.synthBlock, d0, d1, this.dryBlock);
      for (let i = 0; i < 2 * BLOCK; i++) out[i] = this.dryBlock[i] * Q31;
    }
    FX_SLOTS.forEach((s, k) => {
      const fx = this.fx[s];
      const [s0, s1] = fx.ramp.step(t.sends[k]);
      if (!fx.engine) return;
      if (fx.fromSend) {
        if (this.synth) scaleBlock(this.synthBlock, s0, s1, fx.send);
        else fx.send.fill(0);
        for (let i = 0; i < BLOCK; i++) {
          fx.extL[i] = fx.send[2 * i];
          fx.extR[i] = fx.send[2 * i + 1];
        }
        fx.engine.setExternal(fx.extL, fx.extR);
      }
      const f = fx.engine.render();
      for (let i = 0; i < 2 * BLOCK; i++) out[i] += f[i] * Q31;
    });
  }

  private flush(): void {
    this.send({
      type: "stats", synth: this.synth?.stats() ?? null, fx1: this.fx.fx1.engine?.stats() ?? null,
      fx2: this.fx.fx2.engine?.stats() ?? null, busyMs: this.busyMs, blocks: this.blocks, log: this.log,
      sends: [this.fx.fx1.ramp.value, this.fx.fx2.ramp.value], dry: this.dryRamp.value,
    });
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
