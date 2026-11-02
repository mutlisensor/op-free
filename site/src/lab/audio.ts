/**
 * The lab's audio graph: one AudioWorklet (engine/worklet.ts) at 44.1 kHz with the synth and both effects in it, a
 * volume control, and two analysers for the scope. Browsers only start audio from a user gesture, so start() is
 * called from the first click, tap or key; it is safe to call again. Modules are fetched relative to the page
 * (`modules/<dir>.wasm`), so the site works from any folder.
 */

import workletUrl from "./engine/worklet.ts?worker&url";
import type { FromWorklet, ToWorklet } from "./engine/messages.ts";

export class LabAudio {
  ctx: AudioContext | null = null;
  private node: AudioWorkletNode | null = null;
  private volume: GainNode | null = null;
  private starting: Promise<boolean> | null = null;
  private modules = new Map<string, Promise<ArrayBuffer>>();
  private gain = 0.8;
  analysers: [AnalyserNode, AnalyserNode] | null = null;
  onMessage: (m: FromWorklet) => void = () => {};

  get running(): boolean {
    return !!this.node;
  }

  /** Start (or resume) audio. Resolves true once the worklet runs. */
  start(): Promise<boolean> {
    if (this.ctx?.state === "suspended") void this.ctx.resume();
    this.starting ??= this.open().catch((e) => {
      console.error("lab audio:", e);
      this.starting = null;
      return false;
    });
    return this.starting;
  }

  private async open(): Promise<boolean> {
    const ctx = new AudioContext({ sampleRate: 44100, latencyHint: "interactive" });
    this.ctx = ctx;
    await ctx.audioWorklet.addModule(workletUrl);
    const node = new AudioWorkletNode(ctx, "opfree-lab", { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
    const l = ctx.createAnalyser(), r = ctx.createAnalyser();
    l.fftSize = r.fftSize = 2048;
    const split = ctx.createChannelSplitter(2);
    node.connect(split);
    split.connect(l, 0);
    split.connect(r, 1);
    const volume = ctx.createGain();
    volume.gain.value = this.gain;
    node.connect(volume).connect(ctx.destination);
    node.port.onmessage = (e: MessageEvent<FromWorklet>) => this.onMessage(e.data);
    if (ctx.state === "suspended") await ctx.resume();
    this.node = node;
    this.volume = volume;
    this.analysers = [l, r];
    return true;
  }

  post(m: ToWorklet, transfer: Transferable[] = []): void {
    this.node?.port.postMessage(m, transfer);
  }

  setVolume(v: number): void {
    this.gain = v;
    if (this.volume && this.ctx) this.volume.gain.setTargetAtTime(v, this.ctx.currentTime, 0.02);
  }

  /** A module's bytes (cached; each load gets its own copy to transfer). */
  async bytes(url: string): Promise<ArrayBuffer> {
    let p = this.modules.get(url);
    if (!p) {
      p = fetch(new URL(url, document.baseURI)).then((r) => {
        if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
        return r.arrayBuffer();
      });
      p.catch(() => this.modules.delete(url));
      this.modules.set(url, p);
    }
    return (await p).slice(0);
  }

  close(): void {
    this.node?.port.close();
    void this.ctx?.close();
    this.ctx = null;
    this.node = null;
    this.analysers = null;
  }
}
