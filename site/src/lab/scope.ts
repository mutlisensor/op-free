/**
 * Oscilloscope and level meter of the lab's mix, before the volume control, trimmed from sim/web/src/scope.ts. The
 * trace triggers on a rising zero crossing so periodic sounds stand still; the meter shows the peak in dBFS of the
 * hosts' Q31 full scale, and turns red at full scale (the host saturates there).
 */

function db(v: number): number {
  return v > 1e-9 ? 20 * Math.log10(v) : -Infinity;
}

export class Scope {
  private l: AnalyserNode | null = null;
  private r: AnalyserNode | null = null;
  private bufL = new Float32Array(2048);
  private bufR = new Float32Array(2048);
  private raf = 0;
  /** Last reading, dBFS (for the page's checks). */
  level = { peak: -Infinity, maxPeak: -Infinity };
  private readonly canvas: HTMLCanvasElement;
  private readonly meter: HTMLElement;

  constructor(canvas: HTMLCanvasElement, meter: HTMLElement) {
    this.canvas = canvas;
    this.meter = meter;
    this.draw();
  }

  attach(l: AnalyserNode, r: AnalyserNode): void {
    this.l = l;
    this.r = r;
    this.raf = requestAnimationFrame(this.frame);
  }

  destroy(): void {
    cancelAnimationFrame(this.raf);
    this.l = this.r = null;
  }

  private frame = (): void => {
    if (!this.l || !this.r) return;
    this.l.getFloatTimeDomainData(this.bufL);
    this.r.getFloatTimeDomainData(this.bufR);
    let pk = 0;
    for (const b of [this.bufL, this.bufR]) for (const v of b) pk = Math.max(pk, Math.abs(v));
    const peak = db(pk);
    this.level = { peak, maxPeak: Math.max(this.level.maxPeak, peak) };
    this.meter.style.setProperty("--level", String(Math.max(0, Math.min(1, (peak + 60) / 60))));
    this.meter.classList.toggle("clip", pk >= 0.999);
    this.draw();
    this.raf = requestAnimationFrame(this.frame);
  };

  private draw(): void {
    const c = this.canvas;
    const dpr = window.devicePixelRatio || 1;
    const w = c.clientWidth || c.width, h = c.clientHeight || c.height;
    if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
      c.width = Math.round(w * dpr);
      c.height = Math.round(h * dpr);
    }
    const g = c.getContext("2d");
    if (!g) return;
    const css = getComputedStyle(c);
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    g.strokeStyle = css.getPropertyValue("--grid") || "#242428";
    g.lineWidth = 1;
    g.beginPath();
    g.moveTo(0, Math.round(h / 2) + 0.5);
    g.lineTo(w, Math.round(h / 2) + 0.5);
    g.stroke();
    let start = 0;
    for (let i = 1; i < this.bufL.length / 2; i++)
      if (this.bufL[i - 1] < 0 && this.bufL[i] >= 0) {
        start = i;
        break;
      }
    const n = Math.min(1024, this.bufL.length - start);
    const trace = (b: Float32Array, color: string, width: number) => {
      g.strokeStyle = color;
      g.lineWidth = width;
      g.beginPath();
      for (let i = 0; i < n; i++) {
        const x = (i / (n - 1)) * w, y = h / 2 - b[start + i] * (h / 2) * 0.9;
        if (i) g.lineTo(x, y);
        else g.moveTo(x, y);
      }
      g.stroke();
    };
    trace(this.bufR, css.getPropertyValue("--trace-r") || "#63636b", 1);
    trace(this.bufL, css.getPropertyValue("--trace-l") || "#2bb35c", 1.5);
  }
}
