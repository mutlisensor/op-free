/**
 * Ported unchanged from web/app/src/transport/webmidi.ts.
 *
 * The core's `Transport` over Web MIDI (Chrome, Edge, Android Chrome). Needs `requestMIDIAccess({ sysex: true })`,
 * which the browser only grants on https or localhost and after asking the user.
 *
 * Only the structural parts of the Web MIDI types are used, so tests can pass plain fakes.
 *
 * Failure handling: when the OP-Z's port disappears (unplugged, switched off, Bluetooth dropped) `checkPorts()` fails
 * the transport, so a running flow stops with a clear `TransportClosedError` instead of waiting out the core's
 * timeouts; a send the browser refuses does the same. The app then tells the user what to check (src/explain.ts).
 */

import { SysexQueueTransport, type Sleep, TransportClosedError, timeoutSleep } from "./queue.ts";

export interface MidiPortLike {
  readonly id: string;
  readonly name: string | null;
  readonly manufacturer?: string | null;
  readonly state?: string;
}

export interface MidiInputLike extends MidiPortLike {
  addEventListener(type: "midimessage", listener: (event: { data: Uint8Array | null }) => void): void;
  removeEventListener(type: "midimessage", listener: (event: { data: Uint8Array | null }) => void): void;
  open?(): Promise<unknown>;
  close?(): Promise<unknown>;
}

export interface MidiOutputLike extends MidiPortLike {
  send(data: Uint8Array | number[]): void;
  open?(): Promise<unknown>;
  close?(): Promise<unknown>;
}

export interface MidiAccessLike {
  readonly sysexEnabled: boolean;
  readonly inputs: { values(): Iterable<MidiInputLike> };
  readonly outputs: { values(): Iterable<MidiOutputLike> };
  addEventListener?(type: "statechange", listener: () => void): void;
  removeEventListener?(type: "statechange", listener: () => void): void;
}

export class MidiUnavailableError extends Error {
  override name = "MidiUnavailableError";
}

/** Ask for Web MIDI with SysEx. Throws MidiUnavailableError with a message fit for the user. */
export async function requestMidi(nav: Navigator = navigator, secure: boolean = globalThis.isSecureContext): Promise<MidiAccessLike> {
  if (typeof (nav as Navigator & { requestMIDIAccess?: unknown }).requestMIDIAccess !== "function") {
    throw new MidiUnavailableError(
      "This browser has no Web MIDI. Use Chrome or Edge on a computer, or Chrome on Android. Safari and every "
      + "iOS browser lack Web MIDI (the iOS app will bridge CoreMIDI instead). Demo mode works everywhere.",
    );
  }
  if (!secure) {
    throw new MidiUnavailableError("Web MIDI with SysEx needs https or localhost: open the app from http://localhost or an https address.");
  }
  let access: MIDIAccess;
  try {
    access = await nav.requestMIDIAccess({ sysex: true });
  } catch (e) {
    throw new MidiUnavailableError(permissionMessage(e));
  }
  if (!access.sysexEnabled) {
    throw new MidiUnavailableError("The browser gave MIDI access without SysEx, which the file service needs. Allow "
      + "\"MIDI devices\" with SysEx (\"control and reprogram your MIDI devices\") for this site, then reload.");
  }
  return access as unknown as MidiAccessLike;
}

/** What to tell the user when requestMIDIAccess rejects. */
export function permissionMessage(e: unknown): string {
  const name = (e as Error)?.name ?? "Error";
  if (name === "SecurityError" || name === "NotAllowedError") {
    return `MIDI access with SysEx was refused (${name}). In Chrome: click the icon left of the address, set `
      + "\"MIDI devices\" (control and reprogram) to Allow, then reload and connect again. A browser policy can also block it.";
  }
  if (name === "NotSupportedError") {
    return "This browser does not support Web MIDI with SysEx here (NotSupportedError). Use desktop Chrome or Edge.";
  }
  if (name === "AbortError" || name === "InvalidStateError") {
    return `The MIDI system did not start (${name}). Close other apps that hold the OP-Z's MIDI ports (TE's app, a DAW), `
      + "unplug and replug the OP-Z, then try again.";
  }
  return `MIDI access failed (${name}: ${(e as Error)?.message ?? e}). Reload the page and try again.`;
}

export function ports<T extends MidiPortLike>(map: { values(): Iterable<T> }): T[] {
  return [...map.values()].filter((p) => p.state !== "disconnected");
}

/** The port to preselect: the first whose name mentions the OP-Z. */
export function pickOpz<T extends MidiPortLike>(list: T[]): T | null {
  return list.find((p) => /op-?z/i.test(p.name ?? "")) ?? null;
}

/** A port that came back after an unplug: the same id, else the same name. */
export function findAgain<T extends MidiPortLike>(list: T[], previous: { id: string; name: string | null }): T | null {
  return list.find((p) => p.id === previous.id) ?? list.find((p) => p.name !== null && p.name === previous.name) ?? null;
}

export const PORT_LOST = "The OP-Z's MIDI port disconnected (unplugged, switched off, asleep, or Bluetooth dropped).";

export class WebMidiTransport extends SysexQueueTransport {
  readonly input: MidiInputLike;
  readonly output: MidiOutputLike;
  private readonly listener = (event: { data: Uint8Array | null }) => {
    if (event.data) this.feed(event.data);
  };
  private closed = false;

  constructor(input: MidiInputLike, output: MidiOutputLike, sleep: Sleep = timeoutSleep) {
    super(sleep);
    this.input = input;
    this.output = output;
    input.addEventListener("midimessage", this.listener);
  }

  /**
   * Open both ports explicitly so a busy port fails here rather than on the first send. On Windows a port another app
   * holds (TE's app, a DAW) cannot be opened twice; on macOS ports are shared, so the TE-app warning still matters.
   */
  async open(): Promise<void> {
    try {
      await this.input.open?.();
      await this.output.open?.();
    } catch (e) {
      const name = (e as Error)?.name ?? "Error";
      this.close();
      throw new Error(name === "InvalidAccessError"
        ? "The OP-Z's MIDI port is in use by another app (TE's app or a DAW?). Close it and connect again."
        : `Could not open the MIDI ports (${name}: ${(e as Error)?.message ?? e}).`);
    }
  }

  /** Call on every Web MIDI statechange: fails the transport when either port is gone. Returns true if it is. */
  checkPorts(): boolean {
    const gone = [this.input, this.output].some((p) => p.state === "disconnected");
    if (gone) this.fail(new TransportClosedError(PORT_LOST));
    return gone;
  }

  send(sysex: Uint8Array): void {
    this.checkOpen();
    this.trace?.("out", sysex);
    try {
      this.output.send(sysex);
    } catch (e) {
      // InvalidStateError: the port went away between the statechange and this send
      const error = new TransportClosedError(`${PORT_LOST} (${(e as Error)?.name ?? "send failed"})`);
      this.fail(error);
      throw error;
    }
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.input.removeEventListener("midimessage", this.listener);
    this.clear();
    this.fail(new TransportClosedError("MIDI transport is closed"));
  }
}
