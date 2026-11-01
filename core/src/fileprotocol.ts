/** OP-Z filesystem-over-SysEx state machines. Port of opzsysex/fileprotocol.py. */

import { concat, readU16le, readU32le, u16le, u32le, utf8 } from "./bytes.ts";
import { dumps } from "./pyjson.ts";
import { ProtocolError, compress, decodeFrame, decompress, frame } from "./sysex.ts";
import { crc32 } from "./zlib.ts";

export const PART_SIZE = 200;
export const SLOT_CONFIGURATION_ID = 0;
export const PLUGS_ID = 1;
export const SYNC_JOB_ID = 2;

export interface FileDescriptor {
  fileId: number;
  path: string;
  crc: number;
  size: number;
}

export function fileId(path: string): number {
  return crc32(utf8(path.toLowerCase()));
}

const RESERVED: Record<string, number> = {
  "settings/slotconfiguration.json": SLOT_CONFIGURATION_ID,
  "settings/plugs.json": PLUGS_ID,
  "syncjob.json": SYNC_JOB_ID,
};

export function downloadFileId(path: string): number {
  const key = path.toLowerCase();
  return Object.prototype.hasOwnProperty.call(RESERVED, key) ? RESERVED[key] : fileId(path);
}

export function decodeFileFrame(data: Uint8Array): [number, Uint8Array] {
  const decoded = decodeFrame(data);
  return [decoded.messageId, decoded.payload];
}

/** The host side of a sync job: serves syncjob.json and the listed files as the OP-Z requests them. */
export class UploadState {
  static maximumInFlightParts = 64;

  readonly descriptors: FileDescriptor[] = [];
  readonly syncJob: Uint8Array;
  activeFileId: number | null = null;
  nextPart = 0;
  lastPartSentAt = -Infinity;
  readonly sentLastFileIds = new Set<number>();
  sentPartCount = 0;
  clientCompletedPartCount: number | null = null;
  private readonly files = new Map<number, Uint8Array>();

  constructor(files: Array<[string, Uint8Array]>, removePlugIds: number[] | null = null) {
    for (const [path, data] of files) {
      if (data.length === 0) throw new ProtocolError(`empty hosted files are not supported: ${path}`);
      const identifier = fileId(path);
      if (this.files.has(identifier)) throw new ProtocolError(`file-id collision for ${path}: ${identifier}`);
      if (data.length > PART_SIZE * 0xffff) throw new ProtocolError(`file exceeds UInt16 part limit: ${path}`);
      this.files.set(identifier, data);
      this.descriptors.push({ fileId: identifier, path, crc: crc32(data), size: data.length });
    }
    const manifestFiles: Array<Record<string, number | string>> = this.descriptors.map((item) => ({
      fileId: item.fileId, path: item.path, crc: item.crc, size: item.size,
    }));
    for (const value of removePlugIds ?? []) manifestFiles.push({ operation: "remove", plugId: value });
    this.syncJob = utf8(dumps({ files: manifestFiles }, { sortKeys: true, separators: [",", ":"] }));
  }

  acceptRequest(payload: Uint8Array, clientId: number): void {
    if (payload.length !== 8) throw new ProtocolError(`$35 request payload must be 8 bytes, got ${payload.length}`);
    const requester = readU16le(payload, 0);
    if (requester !== clientId) throw new ProtocolError(`unexpected file client ${requester}; expected ${clientId}`);
    this.activeFileId = readU32le(payload, 2);
    this.nextPart = readU16le(payload, 6);
    this.lastPartSentAt = -Infinity;
  }

  observeCompletedParts(count: number): void {
    this.clientCompletedPartCount = Math.max(this.clientCompletedPartCount ?? 0, count);
  }

  get inFlightParts(): number {
    return Math.max(0, this.sentPartCount - (this.clientCompletedPartCount ?? 0));
  }

  get sentAllFiles(): boolean {
    return this.descriptors.every((item) => this.sentLastFileIds.has(item.fileId));
  }

  /** The next $53 part to send, or null while waiting (no request, too many in flight, or pacing). Times in s. */
  nextFrame(serverId: number, now: number = performance.now() / 1000, minimumInterval = 0.01): Uint8Array | null {
    if (this.activeFileId === null || this.inFlightParts >= UploadState.maximumInFlightParts) return null;
    const inFlight = this.inFlightParts;
    const interval = Math.max(minimumInterval, inFlight >= 33 ? 0.05 : inFlight >= 17 ? 0.02 : minimumInterval);
    if (now - this.lastPartSentAt < interval) return null;
    const data = this.activeFileId === SYNC_JOB_ID ? this.syncJob : this.files.get(this.activeFileId);
    if (data === undefined) throw new ProtocolError(`unknown hosted file id ${this.activeFileId}`);
    const start = this.nextPart * PART_SIZE;
    if (start >= data.length) throw new ProtocolError(`part ${this.nextPart} is beyond file ${this.activeFileId}`);
    const end = Math.min(start + PART_SIZE, data.length);
    const last = end === data.length;
    const payload = concat(u16le(serverId), u32le(this.activeFileId), u16le(this.nextPart), Uint8Array.of(last ? 1 : 0),
      compress(data.subarray(start, end)));
    const result = frame(0x53, payload);
    this.lastPartSentAt = now;
    this.sentPartCount += 1;
    if (last) {
      if (this.activeFileId !== SYNC_JOB_ID) this.sentLastFileIds.add(this.activeFileId);
      this.activeFileId = null;
    } else {
      this.nextPart += 1;
    }
    return result;
  }
}

/** The host side of downloads: requests one file at a time and reassembles its parts. */
export class DownloadState {
  readonly queued: Array<[number, string]>;
  current: { id: number; path: string; part: number; content: Uint8Array[] } | null = null;

  constructor(files: Array<[number, string]>) {
    this.queued = [...files];
  }

  get complete(): boolean {
    return this.current === null && this.queued.length === 0;
  }

  begin(serverId: number): Uint8Array | null {
    if (this.current !== null || this.queued.length === 0) return null;
    const [id, path] = this.queued.shift()!;
    this.current = { id, path, part: 0, content: [] };
    return this.request(serverId);
  }

  acceptPart(payload: Uint8Array, clientId: number, serverId: number): [[string, Uint8Array] | null, Uint8Array | null] {
    if (payload.length < 9 || this.current === null) throw new ProtocolError("invalid or unexpected $53 file part");
    const sender = readU16le(payload, 0);
    const identifier = readU32le(payload, 2);
    const part = readU16le(payload, 6);
    const { id, path, part: expectedPart, content } = this.current;
    if (sender !== clientId || identifier !== id || part !== expectedPart) {
      throw new ProtocolError("out-of-order file part or unexpected client");
    }
    content.push(decompress(payload.subarray(9), PART_SIZE));
    if (payload[8]) {
      const completed: [string, Uint8Array] = [path, concat(...content)];
      this.current = null;
      return [completed, this.begin(serverId)];
    }
    this.current = { id, path, part: expectedPart + 1, content };
    return [null, this.request(serverId)];
  }

  private request(serverId: number): Uint8Array {
    const { id, part } = this.current!;
    return frame(0x35, concat(u16le(serverId), u32le(id), u16le(part)));
  }
}
