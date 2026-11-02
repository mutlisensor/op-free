/**
 * Oscilloscope and level meter of the engine's output, before the volume control: what the host hands on, in dBFS
 * of the Q31 full scale. Peak holds for a second; "clip" lights at full scale (the host saturates there).
 */

const HOLD_MS = 1000;

function db(v: number): number {
  return v > 1e-9 ? 20 * Math.log10(v) : -Infinity;
}

function fmt(v: number): string {
  return Number.isFinite(v) ? v.toFixed(1) : "-∞";
}

export class Scope {
  private l: AnalyserNode | null = null;
  private r: AnalyserNode | null = null;
  private bufL = new Float32Array(2048);
  private bufR = new Float32Array(2048);
  private hold = { db: -Infinity, at: 0 };
  private clipAt = 0;
  /** Last reading, dBFS (for the page and the e2e tests). */
  level = { peak: -Infinity, rms: -Infinity, maxPeak: -Infinity };
  private readonly canvas: HTMLCanvasElement;

  constructor(canvas: HTMLCanvasElement) {
    this.canvas = canvas;
    this.draw();
  }

  attach(l: AnalyserNode, r: AnalyserNode): void {
    this.l = l;
    this.r = r;
    requestAnimationFrame(this.frame);
  }

  /** Another source (a scenario render) into the same analysers. */
  connectExtra(split: ChannelSplitterNode): void {
    if (this.l && this.r) {
      split.connect(this.l, 0);
      split.connect(this.r, 1);
    }
  }

  private frame = (): void => {
    if (!this.l || !this.r) return;
    this.l.getFloatTimeDomainData(this.bufL);
    this.r.getFloatTimeDomainData(this.bufR);
    this.meter();
    this.draw();
    requestAnimationFrame(this.frame);
  };

  private meter(): void {
    const now = performance.now();
    const side = (b: Float32Array) => {
      let pk = 0, acc = 0;
      for (const v of b) {
        const a = Math.abs(v);
        if (a > pk) pk = a;
        acc += v * v;
      }
      return { pk, rms: Math.sqrt(acc / b.length) };
    };
    const a = side(this.bufL), b = side(this.bufR);
    const peak = db(Math.max(a.pk, b.pk)), rms = db(Math.max(a.rms, b.rms));
    if (peak >= this.hold.db || now - this.hold.at > HOLD_MS) this.hold = { db: peak, at: now };
    if (Math.max(a.pk, b.pk) >= 0.999) this.clipAt = now;
    this.level = { peak, rms, maxPeak: Math.max(this.level.maxPeak, peak) };
    const bar = (el: HTMLElement | null, d: number) => {
      if (el) el.style.setProperty("--level", `${Math.max(0, Math.min(1, (d + 60) / 60))}`);
    };
    bar(document.getElementById("meter-l"), db(a.pk));
    bar(document.getElementById("meter-r"), db(b.pk));
    document.getElementById("meter-peak")!.textContent = fmt(this.hold.db);
    document.getElementById("meter-rms")!.textContent = fmt(rms);
    document.getElementById("meter-clip")!.hidden = now - this.clipAt > HOLD_MS;
  }

  private draw(): void {
    const c = this.canvas;
    const dpr = window.devicePixelRatio || 1;
    const w = c.clientWidth || c.width, h = c.clientHeight || c.height;
    if (c.width !== Math.round(w * dpr)) {
      c.width = Math.round(w * dpr);
      c.height = Math.round(h * dpr);
    }
    const g = c.getContext("2d")!;
    const css = getComputedStyle(c);
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    g.strokeStyle = css.getPropertyValue("--grid") || "#333";
    g.lineWidth = 1;
    g.beginPath();
    for (const y of [0.25, 0.5, 0.75]) {
      g.moveTo(0, Math.round(h * y) + 0.5);
      g.lineTo(w, Math.round(h * y) + 0.5);
    }
    g.stroke();
    // trigger on a rising zero crossing so periodic sounds stand still
    let start = 0;
    for (let i = 1; i < this.bufL.length / 2; i++)
      if (this.bufL[i - 1] < 0 && this.bufL[i] >= 0) {
        start = i;
        break;
      }
    const n = Math.min(1024, this.bufL.length - start);
    const trace = (b: Float32Array, color: string) => {
      g.strokeStyle = color;
      g.lineWidth = 1.5;
      g.beginPath();
      for (let i = 0; i < n; i++) {
        const x = (i / (n - 1)) * w, y = h / 2 - b[start + i] * (h / 2) * 0.95;
        if (i) g.lineTo(x, y);
        else g.moveTo(x, y);
      }
      g.stroke();
    };
    trace(this.bufR, css.getPropertyValue("--trace-r") || "#888");
    trace(this.bufL, css.getPropertyValue("--trace-l") || "#fff");
  }
}
