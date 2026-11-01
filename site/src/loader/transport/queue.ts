/**
 * Ported unchanged from web/app/src/transport/queue.ts.
 *
 * The receive side every byte-stream MIDI backend shares: raw MIDI bytes go in through `feed`, complete SysEx
 * messages come out of `receive(timeoutMs)` in arrival order. Web MIDI uses it (src/transport/webmidi.ts) and the
 * iOS CoreMIDI bridge will too (docs/ios-bridge.md): both only have to call `feed` and implement `send`.
 *
 * `fail(error)` ends the transport for good: a waiting `receive` and every later `send`/`receive` throw that error, so
 * a core flow stops at once (the OP-Z was unplugged) instead of running into its 15–30 s timeouts.
 */

import { MIDIStreamParser, type Transport } from "../core.ts";

/** Resolve after ms. The app passes a worker-backed timer (src/transport/timer.ts) so background tabs keep time. */
export type Sleep = (ms: number) => Promise<void>;

export const timeoutSleep: Sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/** The MIDI connection is gone (port unplugged, closed, or the app disconnected): nothing more can be sent. */
export class TransportClosedError extends Error {
  override name = "TransportClosedError";
}

export abstract class SysexQueueTransport implements Transport {
  private readonly parser = new MIDIStreamParser();
  private readonly queue: Uint8Array[] = [];
  private waiter: { resolve: (message: Uint8Array | null) => void; reject: (error: Error) => void } | null = null;
  protected readonly sleep: Sleep;
  /** Set once the transport has failed or closed; every call throws it from then on. */
  failure: Error | null = null;
  /** Every complete SysEx message seen, both directions, for a debug trace (off unless set). */
  trace: ((direction: "in" | "out", data: Uint8Array) => void) | null = null;

  constructor(sleep: Sleep = timeoutSleep) {
    this.sleep = sleep;
  }

  abstract send(sysex: Uint8Array): void | Promise<void>;

  /** Raw MIDI bytes from the device: whole messages, fragments of a long SysEx, or realtime bytes in between. */
  feed(data: Uint8Array): void {
    if (this.failure) return;
    for (const event of this.parser.feed(data)) {
      if (event.type !== "sysex") continue;
      this.trace?.("in", event.data);
      if (this.waiter) {
        const { resolve } = this.waiter;
        this.waiter = null;
        resolve(event.data);
      } else {
        this.queue.push(event.data);
      }
    }
  }

  /** Messages waiting to be received. */
  get pending(): number {
    return this.queue.length;
  }

  /** Drop anything queued (stale replies from an earlier session). */
  clear(): void {
    this.queue.length = 0;
  }

  /** End the transport: a waiting receive rejects with `error`, and so does every later call. Idempotent. */
  fail(error: Error): void {
    if (this.failure) return;
    this.failure = error;
    this.queue.length = 0;
    const waiter = this.waiter;
    this.waiter = null;
    waiter?.reject(error);
  }

  protected checkOpen(): void {
    if (this.failure) throw this.failure;
  }

  receive(timeoutMs: number): Promise<Uint8Array | null> {
    if (this.failure) return Promise.reject(this.failure);
    const next = this.queue.shift();
    if (next !== undefined) return Promise.resolve(next);
    if (this.waiter) throw new Error("receive() is already waiting");
    return new Promise((resolve, reject) => {
      let settled = false;
      this.waiter = {
        resolve: (message) => {
          settled = true;
          resolve(message);
        },
        reject: (error) => {
          settled = true;
          reject(error);
        },
      };
      void this.sleep(timeoutMs).then(() => {
        if (settled) return;
        this.waiter = null;
        resolve(null);
      });
    });
  }
}
