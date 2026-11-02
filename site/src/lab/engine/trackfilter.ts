/**
 * The OS's track filter (bytes 8 and 9), approximated for the lab. On the OP-Z the filter knob is the OS's own
 * bipolar filter after the engine, on instrument and FX tracks alike: neutral at the centre, low-pass one way,
 * high-pass the other, cutting to silence at the ends; resonance belongs to it [proven, engine-abi.md]. opzsim
 * doesn't model it. This is our own stand-in (an RBJ-cookbook biquad, public-domain formulas), not TE's tables: the
 * curve and the resonance law are [inferred], so the sound near the ends will differ from the device.
 */

import { SAMPLE_RATE } from "./opzsim.ts";

const Q31 = 1 / 2147483648;
const NEUTRAL = 128;

export class TrackFilter {
  private cutoff = NEUTRAL;
  private resonance = 0;
  private b0 = 1; private b1 = 0; private b2 = 0; private a1 = 0; private a2 = 0;
  private x1 = [0, 0]; private x2 = [0, 0]; private y1 = [0, 0]; private y2 = [0, 0];
  private bypass = true;

  /** byte 8 (filter) or 9 (resonance) */
  set(index: number, value: number): void {
    if (index === 8) this.cutoff = value;
    else this.resonance = value;
    this.update();
  }

  private update(): void {
    const v = this.cutoff;
    const wasBypass = this.bypass;
    this.bypass = Math.abs(v - NEUTRAL) <= 1;
    if (this.bypass) return;
    const lowpass = v < NEUTRAL;
    // exponential sweeps: 128 -> 20 kHz (LP) or 20 Hz (HP); the ends close the filter almost completely
    const t = lowpass ? (NEUTRAL - v) / NEUTRAL : (v - NEUTRAL) / (255 - NEUTRAL);
    const f = lowpass ? 20000 * Math.pow(25 / 20000, t) : 20 * Math.pow(18000 / 20, t);
    const q = 0.707 + (this.resonance / 255) * 9;
    const w = (2 * Math.PI * Math.min(f, SAMPLE_RATE * 0.45)) / SAMPLE_RATE;
    const alpha = Math.sin(w) / (2 * q), cos = Math.cos(w), a0 = 1 + alpha;
    if (lowpass) {
      this.b0 = this.b2 = (1 - cos) / 2 / a0;
      this.b1 = (1 - cos) / a0;
    } else {
      this.b0 = this.b2 = (1 + cos) / 2 / a0;
      this.b1 = -(1 + cos) / a0;
    }
    this.a1 = (-2 * cos) / a0;
    this.a2 = (1 - alpha) / a0;
    if (wasBypass) {
      this.x1 = [0, 0]; this.x2 = [0, 0]; this.y1 = [0, 0]; this.y2 = [0, 0];
    }
  }

  /** In place over an interleaved stereo Q31 block. */
  process(block: Int32Array): void {
    if (this.bypass) return;
    const frames = block.length >> 1;
    for (let c = 0; c < 2; c++) {
      let x1 = this.x1[c], x2 = this.x2[c], y1 = this.y1[c], y2 = this.y2[c];
      for (let i = 0; i < frames; i++) {
        const x = block[2 * i + c] * Q31;
        const y = this.b0 * x + this.b1 * x1 + this.b2 * x2 - this.a1 * y1 - this.a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        const s = Math.round(y * 2147483648);
        block[2 * i + c] = s > 2147483647 ? 2147483647 : s < -2147483648 ? -2147483648 : s;
      }
      this.x1[c] = x1; this.x2[c] = x2;
      // flush denormals
      this.y1[c] = Math.abs(y1) < 1e-20 ? 0 : y1; this.y2[c] = Math.abs(y2) < 1e-20 ? 0 : y2;
    }
  }
}
