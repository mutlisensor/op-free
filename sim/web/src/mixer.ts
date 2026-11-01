/**
 * The OS mixer between a synth track and the FX tracks, as the web simulator models it (opzsim itself has no mixer).
 *
 * - The synth track's FX sends (byte 6 to FX1, byte 7 to FX2) **crossfade**: raising one moves the signal from the dry
 *   path into that effect, so at 255 the track is heard only through the effect [proven, engine-abi.md "renderFX"].
 * - An effect's byte 12 is its **dry level**: 255 keeps the sending track's dry signal (send/return), 0 crossfades it
 *   away (insert-like) [code, app; plug-format.md].
 * - FX1 and FX2 are parallel: each gets its own send of the synth, and both returns are added to the mix with no pan
 *   or level from the OS [code, app `Synth::render`].
 *
 * The curves are linear here and two sends take their shares of the dry path one after the other (the dry gains
 * multiply) [inferred]: the device's exact send law is not measured. The OS's track filter (bytes 8/9) and the FX
 * returns' DC blocker are not modelled either.
 */

export interface Path {
  /** the synth's send byte to this effect */
  sendByte: number;
  /** the effect's byte 12 */
  dryLevelByte: number;
}

export interface Gains {
  /** synth -> each effect's input, in the order of the paths */
  sends: number[];
  /** synth -> mix (dry path) */
  dry: number;
}

export function gains(paths: Path[]): Gains {
  let dry = 1;
  const sends = paths.map((p) => {
    const s = clamp01(p.sendByte / 255), d = clamp01(p.dryLevelByte / 255);
    dry *= 1 - s * (1 - d);
    return s;
  });
  return { sends, dry };
}

function clamp01(x: number): number {
  return x < 0 ? 0 : x > 1 ? 1 : x;
}

const Q31_MAX = 2147483647, Q31_MIN = -2147483648;

/**
 * Scale an interleaved Q31 block by a gain ramping linearly from g0 to g1 across it (no zipper noise when a knob
 * moves), into `out`, saturating.
 */
export function scaleBlock(src: Int32Array, g0: number, g1: number, out: Int32Array): void {
  const frames = src.length >> 1;
  for (let i = 0; i < frames; i++) {
    const g = g0 + ((g1 - g0) * i) / frames;
    for (let c = 0; c < 2; c++) {
      const v = Math.round(src[2 * i + c] * g);
      out[2 * i + c] = v > Q31_MAX ? Q31_MAX : v < Q31_MIN ? Q31_MIN : v;
    }
  }
}

/** A gain that follows its target a step per block (about 12 ms to settle), like a smoothed knob. */
export class Ramp {
  value: number;

  constructor(value: number) {
    this.value = value;
  }

  /** Advance one block towards target; returns [start, end] for scaleBlock. */
  step(target: number): [number, number] {
    const start = this.value;
    this.value += (target - this.value) / 8;
    if (Math.abs(target - this.value) < 1e-6) this.value = target;
    return [start, this.value];
  }
}
