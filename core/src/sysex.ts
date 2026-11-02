/** OP-Z SysEx framing, packed-7 encoding, compression and stream parsing. Port of opzsysex/sysex.py. */

import { EMPTY, concat, u16be } from "./bytes.ts";
import { OutputLimitError, compress as zlibCompress, decompress as zlibDecompress } from "./zlib.ts";

export const TE_HEADER = Uint8Array.of(0xf0, 0x00, 0x20, 0x76, 0x01);
export const IDENTITY_INQUIRY = Uint8Array.of(0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7);
export const PATTERN_QUERY = concat(TE_HEADER, Uint8Array.of(0x08, 0xf7));
export const MIDI_CONFIGURATION_QUERY = concat(TE_HEADER, Uint8Array.of(0x0f, 0xf7));
export const SAMPLE_VISUALIZATION_QUERY = concat(TE_HEADER, Uint8Array.of(0x15, 0xf7));
export const GATE_SEQUENCER_STEP = concat(TE_HEADER, Uint8Array.of(0x61, 0xf7));

export class ProtocolError extends Error {
  override name = "ProtocolError";
}

export interface Frame {
  messageId: number;
  payload: Uint8Array;
}

/** Pack arbitrary 8-bit bytes into the OP-Z/TE 7-bit MIDI representation. */
export function pack7(source: Uint8Array): Uint8Array {
  const out: number[] = [];
  for (let start = 0; start < source.length; start += 7) {
    const chunk = source.subarray(start, start + 7);
    let mask = 0;
    chunk.forEach((byte, bit) => (mask |= ((byte >> 7) & 1) << bit));
    out.push(mask);
    for (const byte of chunk) out.push(byte & 0x7f);
  }
  return Uint8Array.from(out);
}

/** Decode OP-Z/TE packed-7 bytes. */
export function unpack7(source: Uint8Array): Uint8Array {
  if (source.some((byte) => byte > 0x7f)) throw new ProtocolError("packed SysEx data contains an 8-bit byte");
  const out: number[] = [];
  for (let start = 0; start < source.length; start += 8) {
    const chunk = source.subarray(start, start + 8);
    const mask = chunk[0];
    chunk.subarray(1).forEach((byte, bit) => out.push(byte | (((mask >> bit) & 1) << 7)));
  }
  return Uint8Array.from(out);
}

export function frame(messageId: number, payload: Uint8Array = EMPTY): Uint8Array {
  if (!Number.isInteger(messageId) || messageId < 0 || messageId > 0x7f) {
    throw new ProtocolError("message id must be a 7-bit MIDI data byte");
  }
  return concat(TE_HEADER, Uint8Array.of(messageId), pack7(payload), Uint8Array.of(0xf7));
}

export function decodeFrame(data: Uint8Array): Frame {
  const startsWithHeader = data.length >= TE_HEADER.length && TE_HEADER.every((b, i) => data[i] === b);
  if (data.length < 7 || !startsWithHeader || data[data.length - 1] !== 0xf7) {
    throw new ProtocolError("invalid OP-Z SysEx frame");
  }
  return { messageId: data[5], payload: unpack7(data.subarray(6, data.length - 1)) };
}

export function compress(data: Uint8Array): Uint8Array {
  return zlibCompress(data);
}

export function decompress(data: Uint8Array, maxSize?: number): Uint8Array {
  try {
    return zlibDecompress(data, maxSize);
  } catch (e) {
    if (e instanceof OutputLimitError) throw new ProtocolError(`inflated payload exceeds ${maxSize} bytes`);
    throw e;
  }
}

export function heartbeat(token: [number, number]): Uint8Array {
  return concat(TE_HEADER, Uint8Array.of(0x00, 0x01, token[0], token[1], 0x06, 0xf7));
}

export function stateSyncClientHello(clientId: number, protocolVersion = 6): Uint8Array {
  return frame(0x00, concat(u16be(clientId), Uint8Array.of(protocolVersion)));
}

export function stateSyncMasterClaim(localSessionId: number, remoteSessionId: number): Uint8Array {
  return frame(0x01, concat(u16be(localSessionId), u16be(remoteSessionId)));
}

export type MidiEvent = { type: "realtime"; byte: number } | { type: "sysex"; data: Uint8Array };

/** Incremental parser that tolerates MIDI realtime bytes inside SysEx (for byte-stream bridges). */
export class MIDIStreamParser {
  private sysex: number[] | null = null;

  feed(data: Uint8Array): MidiEvent[] {
    const events: MidiEvent[] = [];
    for (const byte of data) {
      if (byte >= 0xf8) {
        events.push({ type: "realtime", byte });
        continue;
      }
      if (this.sysex === null) {
        if (byte === 0xf0) this.sysex = [byte];
        continue;
      }
      if (byte === 0xf0) {
        this.sysex = [byte];
      } else {
        this.sysex.push(byte);
        if (byte === 0xf7) {
          events.push({ type: "sysex", data: Uint8Array.from(this.sysex) });
          this.sysex = null;
        }
      }
    }
    return events;
  }
}
