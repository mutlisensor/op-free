/**
 * The audio thread, ported from sim/web/src/worklet.ts: a synth slot (an instrument on its track) and two effect
 * slots (FX1, FX2), each a LiveEngine with its own host. Per 64-frame block the synth renders, the track filter runs
 * (trackfilter.ts), its FX1 and FX2 sends feed the effects' inputs (IN_EXTERNAL) and its dry path goes to the mix
 * with both returns (mixer.ts). Q31 out of the hosts, float to the speakers. The site's lab always feeds the effects
 * from the synth (sim/web's generators stay there).
 */

import { Ramp, gains, scaleBlock } from "./mixer.ts";
import { FX_SLOTS, type FromWorklet, type FxSlot, type SlotName, type ToWorklet } from "./messages.ts";
import { BLOCK, LiveEngine } from "./opzsim.ts";
import { TrackFilter } from "./trackfilter.ts";

declare function registerProcessor(name: string, ctor: unknown): void;
declare class AudioWorkletProcessor {
  readonly port: MessagePort;
}

const Q31 = 1 / 2147483648;
const SEND_BYTE: Record<FxSlot, number> = { fx1: 6, fx2: 7 };   // the synth track's FX1 / FX2 send bytes
const FX_DRY_LEVEL = 12;               // on an FX track: the dry level of the tracks sending to it

class Effect {
  engine: LiveEngine | null = null;
  bytes = new Uint8Array(18);
  filter = new TrackFilter();
  ramp = new Ramp(0);
  send = new Int32Array(2 * BLOCK);
  out = new Int32Array(2 * BLOCK);
  extL = new Int32Array(BLOCK);
  extR = new Int32Array(BLOCK);
}

class LabProcessor extends AudioWorkletProcessor {
  private synth: LiveEngine | null = null;
  private synthFilter = new TrackFilter();
  private fx: Record<FxSlot, Effect> = { fx1: new Effect(), fx2: new Effect() };
  private synthBytes = new Uint8Array(18);
  private dryRamp = new Ramp(1);
  private pending = new Float32Array(2 * BLOCK);
  private pendingAt = 2 * BLOCK;
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
      this.send({ type: "error", log: `error   ${(err as Error).message}\n` });
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
          this.send({ type: "loaded", slot: m.slot, ok: true, empty: true, isFx: m.slot !== "synth", log: "" });
          return;
        }
        let text = "";
        const engine = new LiveEngine(new WebAssembly.Module(m.bytes), m.track, (t) => void (text += t));
        if (engine.ok) {
          if (m.slot === "synth") this.synth = engine;
          else {
            this.fx[m.slot].engine = engine;
            engine.input("external");
          }
        }
        this.send({ type: "loaded", slot: m.slot, ok: engine.ok, empty: false, isFx: engine.isFx, log: text });
        break;
      }
      case "note": {
        const target = this.engine(m.slot);
        if (m.on) target?.noteOn(m.note, m.velocity);
        else target?.noteOff(m.note);
        break;
      }
      case "param": {
        const fx = m.slot === "synth" ? null : this.fx[m.slot];
        (fx ? fx.bytes : this.synthBytes)[m.index] = m.value;
        if (m.index === 8 || m.index === 9) (fx ? fx.filter : this.synthFilter).set(m.index, m.value);
        this.engine(m.slot)?.param(m.index, m.value, m.immediate);
        if (m.immediate) this.settleGains();
        break;
      }
      case "bend":
        for (const e of this.engines()) e.bend(m.cents);
        break;
      case "tempo":
        for (const e of this.engines()) e.tempo(m.bpm);
        break;
      case "panic":
        for (const e of this.engines()) for (let n = 0; n < 128; n++) e.noteOff(n);
        break;
    }
  }

  private engines(): LiveEngine[] {
    return [this.synth, this.fx.fx1.engine, this.fx.fx2.engine].filter((e): e is LiveEngine => !!e);
  }

  /** Target gains of the send/return paths (mixer.ts); an empty effect slot takes no send. */
  private targets(): { sends: [number, number]; dry: number } {
    const g = gains(FX_SLOTS.map((s) => ({
      sendByte: this.synth && this.fx[s].engine ? this.synthBytes[SEND_BYTE[s]] : 0,
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
      this.synthFilter.process(this.synthBlock);
      scaleBlock(this.synthBlock, d0, d1, this.dryBlock);
      for (let i = 0; i < 2 * BLOCK; i++) out[i] = this.dryBlock[i] * Q31;
    }
    FX_SLOTS.forEach((s, k) => {
      const fx = this.fx[s];
      const [s0, s1] = fx.ramp.step(t.sends[k]);
      if (!fx.engine) return;
      if (this.synth) scaleBlock(this.synthBlock, s0, s1, fx.send);
      else fx.send.fill(0);
      for (let i = 0; i < BLOCK; i++) {
        fx.extL[i] = fx.send[2 * i];
        fx.extR[i] = fx.send[2 * i + 1];
      }
      fx.engine.setExternal(fx.extL, fx.extR);
      fx.out.set(fx.engine.render());
      fx.filter.process(fx.out);
      for (let i = 0; i < 2 * BLOCK; i++) out[i] += fx.out[i] * Q31;
    });
  }

  process(_inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    const out = outputs[0];
    const left = out[0], right = out[1] ?? out[0];
    if (!this.engines().length) {
      left.fill(0);
      right.fill(0);
      return true;
    }
    for (let n = 0; n < left.length; n++) {
      if (this.pendingAt >= 2 * BLOCK) {
        this.renderBlock();
        this.pendingAt = 0;
      }
      left[n] = this.pending[this.pendingAt];
      right[n] = this.pending[this.pendingAt + 1];
      this.pendingAt += 2;
    }
    return true;
  }
}

registerProcessor("opfree-lab", LabProcessor);
