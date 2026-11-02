/**
 * Deterministic transports: a virtual clock with a transcript (the TypeScript twin of tools/record_vectors.py's
 * Recorder) and a replayer for recorded transcripts (test-vectors/*.json).
 *
 * Clock model, shared with the recorder: time starts at `start` seconds; a receive() that returns a frame advances
 * it by `latency`; one that times out advances it by the full timeout. Nothing else moves the clock.
 */

import { bytesEqual, fromHex, toHex } from "./bytes.ts";
import type { Transport } from "./session.ts";

/** One line of a transcript, as stored in test-vectors/*.json (frames as hex). */
export type VectorEvent =
  | { send: string }
  | { recv: string | null; timeout: number; repeat?: number };

export interface DeviceLike {
  send(raw: Uint8Array): void | Promise<void>;
  receive(timeoutMs: number): Promise<Uint8Array | null>;
}

/** Wraps a device (e.g. FakeOpz): virtual time, and every frame in both directions recorded. */
export class VirtualTransport implements Transport {
  now: number;
  readonly latency: number;
  readonly events: VectorEvent[] = [];
  private readonly device: DeviceLike;

  constructor(device: DeviceLike, options: { start?: number; latency?: number } = {}) {
    this.device = device;
    this.now = options.start ?? 0;
    this.latency = options.latency ?? 0.001;
  }

  readonly clock = (): number => this.now;

  async send(raw: Uint8Array): Promise<void> {
    this.events.push({ send: toHex(raw) });
    await this.device.send(raw);
  }

  async receive(timeoutMs: number): Promise<Uint8Array | null> {
    const timeout = timeoutMs / 1000;
    const raw = await this.device.receive(timeoutMs);
    if (raw === null) {
      this.now += timeout;
      const last = this.events.at(-1);
      if (last !== undefined && "recv" in last && last.recv === null && last.timeout === timeout) {
        last.repeat = (last.repeat ?? 1) + 1;
      } else {
        this.events.push({ recv: null, timeout });
      }
      return null;
    }
    this.now += this.latency;
    this.events.push({ recv: toHex(raw), timeout });
    return raw;
  }
}

export class ReplayMismatch extends Error {
  override name = "ReplayMismatch";
}

/**
 * Plays the device side of a recorded transcript: every host frame must equal the recorded one byte for byte, and
 * every receive() must come with the recorded timeout. The first difference is kept in `mismatch` (a session's
 * `finally` may send more frames after it) and thrown.
 */
export class ReplayTransport implements Transport {
  now: number;
  readonly latency: number;
  private readonly events: VectorEvent[];
  private index = 0;
  private repeatLeft = 0;
  mismatch: ReplayMismatch | null = null;

  constructor(events: VectorEvent[], options: { start?: number; latency?: number } = {}) {
    this.events = events;
    this.now = options.start ?? 0;
    this.latency = options.latency ?? 0.001;
  }

  readonly clock = (): number => this.now;

  /** Events consumed so far, and whether the whole transcript was. */
  get position(): number {
    return this.index;
  }

  get finished(): boolean {
    return this.index >= this.events.length && this.repeatLeft === 0;
  }

  private fail(message: string): never {
    this.mismatch ??= new ReplayMismatch(`event ${this.index}: ${message}`);
    throw this.mismatch;
  }

  async send(raw: Uint8Array): Promise<void> {
    if (this.mismatch) throw this.mismatch;
    if (this.repeatLeft > 0) this.fail(`host sent ${toHex(raw)} while ${this.repeatLeft} more empty receives were recorded`);
    const e = this.events[this.index];
    if (e === undefined) this.fail(`host sent ${toHex(raw)} after the end of the transcript`);
    if (!("send" in e)) this.fail(`host sent ${toHex(raw)}; recorded a receive (${e.timeout} s)`);
    if (!bytesEqual(raw, fromHex(e.send))) this.fail(`host sent\n  ${toHex(raw)}\nrecorded\n  ${e.send}`);
    this.index++;
  }

  async receive(timeoutMs: number): Promise<Uint8Array | null> {
    if (this.mismatch) throw this.mismatch;
    const timeout = timeoutMs / 1000;
    if (this.repeatLeft > 0) {
      const e = this.events[this.index - 1] as { timeout: number };
      if (e.timeout !== timeout) this.fail(`receive(${timeout} s); recorded ${e.timeout} s`);
      this.repeatLeft--;
      this.now += timeout;
      return null;
    }
    const e = this.events[this.index];
    if (e === undefined) this.fail(`receive(${timeout} s) after the end of the transcript`);
    if (!("recv" in e)) this.fail(`receive(${timeout} s); recorded host frame ${e.send}`);
    if (e.timeout !== timeout) this.fail(`receive(${timeout} s); recorded ${e.timeout} s`);
    this.index++;
    if (e.recv === null) {
      this.repeatLeft = (e.repeat ?? 1) - 1;
      this.now += timeout;
      return null;
    }
    this.now += this.latency;
    return fromHex(e.recv);
  }
}
