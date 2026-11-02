/**
 * Ported from web/app/src/transport/demo.ts (only the storage keys and the factory state differ).
 *
 * Demo mode: the core's FakeOpz behind the same `Transport` seam as Web MIDI, with real waiting (FakeOpz answers
 * synchronously and returns null at once when it has nothing, which would make the session loops spin and freeze the
 * page). The fake's files, presets and track sounds are kept in localStorage so the demo device survives a reload.
 *
 * The fake keeps its own time (it ends a StateSync session 5 s after the last hello): every wait here moves it on, and
 * `clock` reads it, so the core's flows run on the same time base as the fake even when `sleep` returns at once.
 */

import { FakeOpz, type FakeState, type Transport, defaultFiles, fromHex, toHex } from "../core.ts";
import { type Sleep, timeoutSleep } from "./queue.ts";

const STORAGE_KEY = "op-free.demo-device";
const STATE_KEY = "op-free.demo-state";

export class DemoTransport implements Transport {
  readonly fake: FakeOpz;
  private readonly sleep: Sleep;
  /** Simulated wire delay per received frame, in ms (0: deliver at once). */
  latencyMs: number;
  trace: ((direction: "in" | "out", data: Uint8Array) => void) | null = null;
  /** Set by fail(): the demo's "unplug", so the app's recovery paths can be tried without hardware. */
  failure: Error | null = null;

  constructor(fake: FakeOpz = new FakeOpz(), sleep: Sleep = timeoutSleep, latencyMs = 0) {
    this.fake = fake;
    this.sleep = sleep;
    this.latencyMs = latencyMs;
  }

  send(sysex: Uint8Array): void {
    if (this.failure) throw this.failure;
    this.trace?.("out", sysex);
    this.fake.send(sysex);
  }

  /** Simulate the OP-Z going away: every later send and receive throws `error`. */
  fail(error: Error): void {
    this.failure ??= error;
  }

  /** Seconds on the fake's clock: pass it to the core (Opz `clock`) for this transport. */
  readonly clock = (): number => this.fake.now;

  async receive(timeoutMs: number): Promise<Uint8Array | null> {
    if (this.failure) throw this.failure;
    if (!this.fake.out.length) {
      await this.sleep(timeoutMs);
      if (this.failure) throw this.failure;
      this.fake.elapse(timeoutMs / 1000);
      return null;
    }
    if (this.latencyMs) await this.sleep(Math.min(this.latencyMs, timeoutMs));
    const message = await this.fake.receive(timeoutMs);
    if (message) this.trace?.("in", message);
    return message;
  }
}

type SavedState = Pick<FakeState, "sounds" | "presets">;

/** The demo device's presets and track sounds from localStorage (time and sessions always start fresh). */
export function loadDemoState(fake: FakeOpz, storage: Storage | undefined = globalThis.localStorage): void {
  try {
    const raw = storage?.getItem(STATE_KEY);
    if (raw) fake.loadState({ ...fake.state(), ...(JSON.parse(raw) as SavedState) });
  } catch {
    // unreadable or blocked storage: start fresh
  }
}

export function saveDemoState(fake: FakeOpz, storage: Storage | undefined = globalThis.localStorage): void {
  try {
    const { sounds, presets } = fake.state();
    storage?.setItem(STATE_KEY, JSON.stringify({ sounds, presets }));
  } catch {
    // quota or blocked storage
  }
}

/** The demo device's files from localStorage, or `factory` (the fake's factory state by default). */
export function loadDemoFiles(
  storage: Storage | undefined = globalThis.localStorage, factory: () => Map<string, Uint8Array> = defaultFiles,
): Map<string, Uint8Array> {
  try {
    const raw = storage?.getItem(STORAGE_KEY);
    if (raw) return new Map(Object.entries(JSON.parse(raw) as Record<string, string>).map(([k, v]) => [k, fromHex(v)]));
  } catch {
    // unreadable or blocked storage: start fresh
  }
  return factory();
}

export function saveDemoFiles(files: Map<string, Uint8Array>, storage: Storage | undefined = globalThis.localStorage): void {
  try {
    storage?.setItem(STORAGE_KEY, JSON.stringify(Object.fromEntries([...files].map(([k, v]) => [k, toHex(v)]))));
  } catch {
    // quota or blocked storage: the demo just won't survive a reload
  }
}

export function resetDemoFiles(storage: Storage | undefined = globalThis.localStorage): void {
  try {
    storage?.removeItem(STORAGE_KEY);
    storage?.removeItem(STATE_KEY);
  } catch {
    // ignore
  }
}
