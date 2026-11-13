/**
 * Live OP-Z file-service session over SysEx (USB or BLE MIDI). Port of tools/opz_session.py, which is proven on
 * hardware (docs/STATUS.md):
 *
 *     host $00 hello (protocol 6) + $51 advertise  ->  OP-Z $34 (client id, state CRC)
 *     pull:  host $52 (sync-active 0), $35 request  ->  OP-Z $53 parts (auto-advancing)
 *     push:  host $52 (sync-active 1)  ->  OP-Z $35 for syncjob.json, then for each file it wants
 *            host $53 parts  <-  OP-Z $33 progress (1..126 completed parts, 127 done, 255 state CRC)
 *     close: host $52 (sync-active 0), $55
 *
 * Both the app-protocol hello and the file-server session time out after ~6 s, so both are refreshed every second.
 *
 * `StateSession` is the other half of the app protocol (research/presets.md, patterns.md §5): the `$00` hello
 * opens a StateSync session and the OP-Z answers every hello with `$01`. On firmware 1.2.45 the first hello of a
 * session brings `$07` (play state), `$0C` (global block) and part 0 of the active project's pattern bank, not the `$0E`
 * per track the notes repo describes. The OP-Z sends each further part only after the client's `$0B` ack of the part
 * before; `$08` makes it start the bank over (both read-only). The session inflates the bank and takes each track's
 * current sound (its restore point) from the active pattern's sound block; a `$0E` the OP-Z sends is taken too. A host
 * `$0E` (track + 18 parameter bytes) sets that track's sound in the active pattern (device-proven in the notes repo).
 * The session ends ~5 s after the last hello.
 * All times are in seconds, read from `clock`, exactly where the Python reads them, so that a virtual clock yields
 * the same frames in the same order as the Python tools (test-vectors/).
 */

import { concat, readU16le, readU32le, u16le, u32le } from "./bytes.ts";
import { SYNC_JOB_ID, UploadState, downloadFileId } from "./fileprotocol.ts";
import { BANK_SIZE, PATTERN_SIZE } from "./project.ts";
import { ProtocolError, decodeFrame, decompress, frame, stateSyncClientHello } from "./sysex.ts";
import { ZlibError } from "./zlib.ts";

/** What a MIDI backend provides: Web MIDI in the browser, the CoreMIDI bridge in the iOS app, or a test double. */
export interface Transport {
  /** Send one complete SysEx message (F0 ... F7). */
  send(sysex: Uint8Array): void | Promise<void>;
  /** The next complete SysEx message from the OP-Z, or null if none arrives within timeoutMs. */
  receive(timeoutMs: number): Promise<Uint8Array | null>;
}

/** Monotonic time in seconds. */
export type Clock = () => number;

export const monotonic: Clock = () => performance.now() / 1000;

export const SERVER_ID = 0x4242;
export const STATUS_DONE = 127;
export const STATUS_STATE_CRC = 255;
export const SOUND = 0x0e;
export const SESSION_ACK = 0x01;
/** Notes repo: 6.25 s without hellos reliably ends a StateSync session. */
export const STATE_EXPIRY = 6.5;
export const PLAY_STATE = 0x07;
export const BANK_REQUEST = 0x08;
export const BANK_PART = 0x09;
export const BANK_FINAL = 0x0a;
export const BANK_ACK = 0x0b;
/** Compressed bytes per bank part; a non-final part carries exactly this many. */
export const BANK_CHUNK = 178;
/** A bank transfer in progress extends open()'s wait by this much per part. */
const PART_WAIT = 1.0;
/** `$08` re-requests per session (broken or stalled transfers). */
const BANK_REQUESTS = 2;
const SOUND_AT = 21_056;

export class SessionError extends Error {
  override name = "SessionError";
}

/**
 * The OP-Z asked for a file of an earlier sync job that never finished (UploadState.wantsUnknownFile). Nothing of
 * this push was sent, so the caller may simply try it again.
 */
export class StaleJobError extends SessionError {
  override name = "StaleJobError";
}

/**
 * Refuse a path two folders deep before anything is sent. The OP-Z stages each download in inbox/<path> and makes only
 * its last folder (one mkdir, FUN_80129454), so inbox/a/b/ fails unless inbox/a/ exists: the parts are taken, the
 * sync reports complete, and finalising copies nothing ("invalid file crc", FUN_80127a24) [code, device; Q-49].
 */
export function checkUploadPaths(files: Array<[string, Uint8Array]>): void {
  for (const [path] of files) {
    if (path.split("/").length > 2) {
      throw new SessionError(`${path} is two folders deep: the OP-Z's file service stages downloads in inbox/ with a ` +
        "single mkdir and would drop it without an error (Q-49); nothing sent");
    }
  }
}

export interface PushResult {
  requested: string[];
  skipped: string[];
}

export interface SessionOptions {
  serverId?: number;
  log?: (message: string) => void;
  clock?: Clock;
}

function hex8(value: number): string {
  return value.toString(16).padStart(8, "0");
}

export class FileSession {
  readonly t: Transport;
  readonly serverId: number;
  readonly log: (message: string) => void;
  readonly clock: Clock;
  clientId: number | null = null;
  stateCrc: number | null = null;
  private lastKeepalive = -1e9;
  private active = 0;

  constructor(transport: Transport, options: SessionOptions = {}) {
    this.t = transport;
    this.serverId = options.serverId ?? SERVER_ID;
    this.log = options.log ?? (() => {});
    this.clock = options.clock ?? monotonic;
  }

  // -- framing helpers ------------------------------------------------------------------------------------------
  private sid(): Uint8Array {
    return u16le(this.serverId);
  }

  private async recv(timeout: number): Promise<[number, Uint8Array] | null> {
    const raw = await this.t.receive(timeout * 1000);
    if (raw === null) return null;
    try {
      const f = decodeFrame(raw);
      return [f.messageId, f.payload];
    } catch (e) {
      if (e instanceof ProtocolError) return null;
      throw e;
    }
  }

  private async keepalive(force = false): Promise<void> {
    const now = this.clock();
    if (force || now - this.lastKeepalive >= 1.0) {
      await this.t.send(stateSyncClientHello(this.serverId));
      await this.t.send(frame(0x52, concat(this.sid(), Uint8Array.of(this.active))));
      this.lastKeepalive = now;
    }
  }

  // -- session --------------------------------------------------------------------------------------------------
  async open(timeout = 30.0): Promise<[number, number]> {
    await this.t.send(stateSyncClientHello(this.serverId));
    await this.t.send(frame(0x51, this.sid()));
    const start = this.clock();
    let lastAdvert = start;
    while (this.clock() - start < timeout) {
      if (this.clock() - lastAdvert > 1.5) {
        await this.t.send(frame(0x51, this.sid()));
        lastAdvert = this.clock();
      }
      const msg = await this.recv(0.25);
      if (msg && msg[0] === 0x34 && msg[1].length >= 6) {
        this.clientId = readU16le(msg[1], 0);
        this.stateCrc = readU32le(msg[1], 2);
        this.log(`session open: client ${this.clientId}, state CRC ${hex8(this.stateCrc)}`);
        this.active = 0;
        await this.keepalive(true);
        return [this.clientId, this.stateCrc];
      }
    }
    throw new SessionError(
      `no $34 reply from the OP-Z within ${timeout.toFixed(0)} s (is it in normal mode, and the official app closed?)`,
    );
  }

  async close(): Promise<void> {
    this.active = 0;
    await this.t.send(frame(0x52, concat(this.sid(), Uint8Array.of(0))));
    await this.t.send(frame(0x55, this.sid()));
    this.log("session closed");
  }

  /** Download one served file. Throws SessionError if it is not served or incomplete. */
  async pull(path: string, partTimeout = 2.5, cap = 12_000_000): Promise<Uint8Array> {
    if (this.clientId === null) throw new SessionError("session not open");
    this.active = 0;
    const fid = downloadFileId(path);
    const got = new Map<number, Uint8Array>();
    let want = 0;
    let lastPart: number | null = null;
    let retries = 0;
    await this.keepalive(true);
    for (;;) {
      await this.t.send(frame(0x35, concat(this.sid(), u32le(fid), u16le(want))));
      const deadline = this.clock() + partTimeout;
      let progressed = false;
      let lastRx: number | null = null;
      while (this.clock() < deadline) {
        await this.keepalive();
        if (progressed && lastRx !== null && this.clock() - lastRx > 0.3) break; // stream paused: ask again
        const msg = await this.recv(0.05);
        if (!msg || msg[0] !== 0x53 || msg[1].length < 9) continue;
        const p = msg[1];
        if (readU16le(p, 0) !== this.clientId || readU32le(p, 2) !== fid) continue;
        const part = readU16le(p, 6);
        if (part < want) continue;
        try {
          got.set(part, decompress(p.subarray(9)));
        } catch (e) {
          if (e instanceof ZlibError) continue;
          throw e;
        }
        if (p[8]) lastPart = part;
        lastRx = this.clock();
        if (part === want) {
          progressed = true;
          retries = 0;
          while (got.has(want)) want += 1;
          let total = 0;
          for (const v of got.values()) total += v.length;
          if (total > cap) throw new SessionError(`${path} exceeds ${cap} bytes`);
        }
        if (lastPart !== null && want > lastPart) {
          const parts: Uint8Array[] = [];
          for (let i = 0; i <= lastPart; i++) parts.push(got.get(i)!);
          const data = concat(...parts);
          this.log(`pulled ${path}: ${data.length} bytes in ${lastPart + 1} parts`);
          return data;
        }
      }
      if (got.size === 0) throw new SessionError(`${path} is not served by the file service`);
      if (!progressed) {
        retries += 1;
        if (retries >= 3) throw new SessionError(`${path} incomplete: missing part ${want}`);
      }
    }
  }

  /**
   * Serve a sync job: the OP-Z pulls syncjob.json, then requests each listed file unless the file already at its path
   * has the listed CRC (FUN_8012bb00). It ends when the OP-Z reports $33 127 and every file was sent, or `grace`
   * seconds after 127 for files it did not request. Refuses paths two folders deep (checkUploadPaths, Q-49).
   */
  async push(
    files: Array<[string, Uint8Array]>,
    removePlugIds: number[] | null = null,
    idleTimeout = 15.0,
    grace = 2.0,
    deadline: number | null = null,
  ): Promise<PushResult> {
    if (this.clientId === null) throw new SessionError("session not open");
    checkUploadPaths(files);
    const state = new UploadState(files, removePlugIds);
    const paths = new Map(state.descriptors.map((d) => [d.fileId, d.path] as [number, string]));
    const requested = new Set<number>();
    // A whole push may take this long. The OP-Z can keep asking for parts without the job ever finishing (H-IS1,
    // 2026-11-13: a 7-file push swept for minutes), and then the idle timeout never fires.
    const bytes = files.reduce((n, [, d]) => n + d.length, 0);
    const limit = deadline ?? 30 + bytes / 2000;
    const trace: string[] = [];
    const note = (t: number, what: string) => {
      trace.push(`${(t - started).toFixed(2)} s ${what}`);
      if (trace.length > 24) trace.splice(0, trace.length - 24);
    };
    this.active = 1;
    await this.keepalive(true);
    const started = this.clock();
    let lastActivity = started;
    let doneAt: number | null = null;
    for (;;) {
      const now = this.clock();
      const out = state.nextFrame(this.serverId, now);
      if (out !== null) {
        await this.t.send(out);
        lastActivity = now;
      }
      const msg = await this.recv(0.002);
      if (msg) {
        const [mid, p] = msg;
        if (mid === 0x35) {
          state.acceptRequest(p, this.clientId);
          if (state.activeFileId !== SYNC_JOB_ID && !paths.has(state.activeFileId!)) {
            // H-IS1 (2026-11-13): after a push that never finished, the OP-Z went on asking for that push's files in
            // the next sessions of the same client, until it restarted.
            this.active = 0;
            await this.keepalive(true);
            throw new StaleJobError(`the OP-Z asked for file ${state.activeFileId}, which is not in this push: it is `
              + "still working through an earlier sync job that never finished. Nothing of this push was sent; trying "
              + "again clears one of that job's files");
          }
          note(now, `$35 file ${state.activeFileId === SYNC_JOB_ID ? "syncjob" : (paths.get(state.activeFileId!) ?? state.activeFileId)} from part ${state.nextPart}`
            + ` (sent ${state.sentPartCount}, done ${state.clientCompletedPartCount ?? "-"})`);
          if (state.activeFileId !== null && paths.has(state.activeFileId)) requested.add(state.activeFileId);
          lastActivity = now;
          doneAt = null;
        } else if (mid === 0x33 && p.length >= 7 && readU16le(p, 0) === this.clientId) {
          const status = p[2];
          const value = readU32le(p, 3);
          if (status >= 1 && status <= 126) {
            state.observeCompletedParts(value);
            lastActivity = now;
          } else if (status === STATUS_DONE && doneAt === null) {
            note(now, "$33 done");
            doneAt = now;
            this.active = 0; // keep-alives must not start a second sync
            this.log("OP-Z reports sync complete");
          }
        }
      }
      if (doneAt !== null && state.activeFileId === null) {
        if (state.sentAllFiles || now - doneAt >= grace) break;
      }
      if (now - lastActivity > idleTimeout || now - started > limit) {
        this.active = 0;
        await this.keepalive(true);
        for (const line of trace) this.log("push trace: " + line);
        this.log(`push trace: sent ${state.sentPartCount} parts, the OP-Z reported ${state.clientCompletedPartCount ?? "none"} done`);
        throw new SessionError(now - lastActivity > idleTimeout ? `sync stalled for ${idleTimeout.toFixed(0)} s`
          : `sync not finished after ${limit.toFixed(0)} s (the OP-Z kept the transfer going)`);
      }
      await this.keepalive();
    }
    this.active = 0;
    await this.keepalive(true);
    const ids = [...paths.keys()];
    return {
      requested: ids.filter((i) => requested.has(i)).map((i) => paths.get(i)!),
      skipped: ids.filter((i) => !requested.has(i)).map((i) => paths.get(i)!),
    };
  }
}

/** `$0E`: one track's 18 sound-parameter bytes for the active pattern. */
export function soundFrame(track: number, values: readonly number[]): Uint8Array {
  if (!(Number.isInteger(track) && track >= 0 && track <= 15) || values.length !== 18
    || !values.every((v) => Number.isInteger(v) && v >= 0 && v <= 255)) {
    throw new RangeError("a sound is a track 0..15 and 18 values in 0..255");
  }
  return frame(SOUND, Uint8Array.of(track, ...values));
}

/** [track, values] from a decoded `$0E` payload, or null if it is not exactly track + 18 bytes. */
export function decodeSound(payload: Uint8Array): [number, number[]] | null {
  if (payload.length !== 19 || payload[0] > 15) return null;
  return [payload[0], [...payload.subarray(1)]];
}

/** `$0B`: the client has bank part `index` of `transfer` (TE's receiver sends `09 00 00 00`, transfer, index). */
export function bankAckFrame(transfer: number, index: number): Uint8Array {
  return frame(BANK_ACK, concat(Uint8Array.of(9, 0, 0, 0), u16le(transfer), u16le(index)));
}

/** `$08`: send the active bank again from part 0 (the OP-Z ignores the payload; TE's receiver sends FF FF FF FF). */
export function bankRequestFrame(): Uint8Array {
  return frame(BANK_REQUEST, Uint8Array.of(0xff, 0xff, 0xff, 0xff));
}

/** Every track's sound in the active pattern (address & 15) of an inflated bank. */
export function bankSounds(bank: Uint8Array, address: number): Map<number, number[]> {
  const at = (address & 0x0f) * PATTERN_SIZE + SOUND_AT;
  return new Map(Array.from({ length: 16 }, (_, t) => [t, [...bank.subarray(at + 18 * t, at + 18 * t + 18)]]));
}

/**
 * A StateSync session for `$0E` sound writes: a hello every second, each track's current sound (the restore points)
 * from the OP-Z's opening burst. Port of `StateSession` in tools/opz_session.py. Receives always use 0.25 s timeouts
 * (keeps the shared vectors exact).
 */
export class StateSession {
  readonly t: Transport;
  readonly clientId: number;
  readonly log: (message: string) => void;
  readonly clock: Clock;
  /** Per track: the sound at open (from the bank), or a later `$0E` the OP-Z sent. */
  readonly sounds = new Map<number, number[]>();
  acked = false;
  /** Message id -> count of every frame received. */
  readonly seen = new Map<number, number>();
  /** The first complete bank (the active project, at open) and its address: project × 16 + active pattern. */
  bank: Uint8Array | null = null;
  bankAddress: number | null = null;
  /** `$07` byte 17, the latest. */
  playAddress: number | null = null;
  private rx: { address: number; transfer: number; chunks: Uint8Array[] } | null = null;
  private requests = 0;
  private until = -1e9;
  private lastHello = -1e9;

  constructor(transport: Transport, options: { clientId?: number; log?: (message: string) => void; clock?: Clock } = {}) {
    this.t = transport;
    this.clientId = options.clientId ?? SERVER_ID;
    this.log = options.log ?? (() => {});
    this.clock = options.clock ?? monotonic;
  }

  private async hello(force = false): Promise<void> {
    const now = this.clock();
    if (force || now - this.lastHello >= 1.0) {
      await this.t.send(stateSyncClientHello(this.clientId));
      this.lastHello = now;
    }
  }

  private async requestBank(why: string): Promise<void> {
    this.rx = null;
    if (this.requests < BANK_REQUESTS) {
      this.requests += 1;
      this.log(`bank ${why}: asking for it again`);
      await this.t.send(bankRequestFrame());
    }
  }

  /** One `$09`/`$0A`: checked as TE's receiver does (0x2b67c), acked, and on the last part inflated. */
  private async bankPart(final: boolean, payload: Uint8Array): Promise<void> {
    if (payload.length < 7 || payload.length - 6 > BANK_CHUNK || (!final && payload.length - 6 !== BANK_CHUNK)) {
      return this.requestBank("part malformed");
    }
    const address = readU16le(payload, 0);
    const transfer = readU16le(payload, 2);
    const index = readU16le(payload, 4);
    if (index === 0) this.rx = { address, transfer, chunks: [] };
    else if (this.rx === null || this.rx.transfer !== transfer || this.rx.chunks.length !== index) {
      return this.requestBank(`part ${index} out of order`);
    }
    const rx = this.rx!;
    rx.chunks.push(payload.slice(6));
    this.until = Math.max(this.until, this.clock() + PART_WAIT);
    if (!final) {
      await this.t.send(bankAckFrame(transfer, index));
      return;
    }
    this.rx = null;
    const z = concat(...rx.chunks);
    let bank: Uint8Array;
    try {
      bank = decompress(z, BANK_SIZE);
    } catch (e) {
      if (!(e instanceof ZlibError)) throw e;
      bank = new Uint8Array(0);
    }
    if (bank.length !== BANK_SIZE) return this.requestBank(`did not inflate to ${BANK_SIZE} bytes`);
    if (this.bank !== null) return; // a later bank (after our own writes) is no restore point
    this.bank = bank;
    this.bankAddress = rx.address;
    this.log(`bank: project ${(rx.address >> 4) + 1}, pattern ${(rx.address & 0x0f) + 1} (${index + 1} parts, `
      + `${z.length} bytes)`);
    for (const [t, values] of bankSounds(bank, rx.address)) if (!this.sounds.has(t)) this.sounds.set(t, values);
  }

  private async take(timeoutMs = 250): Promise<[number, number[]] | null> {
    const raw = await this.t.receive(timeoutMs);
    if (raw === null) return null;
    let f;
    try {
      f = decodeFrame(raw);
    } catch (e) {
      if (e instanceof ProtocolError) return null;
      throw e;
    }
    this.seen.set(f.messageId, (this.seen.get(f.messageId) ?? 0) + 1);
    if (f.messageId === SESSION_ACK) this.acked = true;
    else if (f.messageId === SOUND) {
      const sound = decodeSound(f.payload);
      if (sound !== null) {
        this.sounds.set(sound[0], sound[1]);
        return sound;
      }
    } else if (f.messageId === BANK_PART || f.messageId === BANK_FINAL) {
      await this.bankPart(f.messageId === BANK_FINAL, f.payload);
    } else if (f.messageId === PLAY_STATE && f.payload.length === 20) {
      this.playAddress = f.payload[17];
    }
    return null;
  }

  private ready(tracks: readonly number[]): boolean {
    return this.acked && tracks.every((t) => this.sounds.has(t));
  }

  /** Until ready or `timeout` has passed; a bank transfer still arriving extends the wait part by part. */
  private async wait(tracks: readonly number[], timeout: number): Promise<void> {
    this.until = this.clock() + timeout;
    while (this.clock() < this.until && !this.ready(tracks)) {
      await this.hello();
      await this.take();
    }
  }

  /**
   * Start the session; wait for `$01` and each track's current sound in `tracks` (the restore points): from the bank,
   * which the OP-Z sends part by part as they are acked, or from a `$0E`. A session still live from an earlier client
   * sends no new burst: wait for it to expire, then start once more.
   */
  async open(tracks: readonly number[] = [], timeout = 3.0): Promise<void> {
    for (const attempt of [1, 2]) {
      await this.hello(true);
      await this.wait(tracks, timeout);
      if (!this.ready(tracks) && this.rx !== null) {
        await this.requestBank("transfer stalled");
        await this.wait(tracks, timeout);
      }
      if (this.ready(tracks)) {
        if (this.bankAddress !== null && this.playAddress !== null && this.playAddress !== this.bankAddress) {
          throw new SessionError(`the active pattern (${this.playAddress}) is not the bank's (${this.bankAddress}): `
            + "the restore points are not sure; nothing written");
        }
        this.log("state session open");
        return;
      }
      if (!this.acked) {
        throw new SessionError(`no $01 reply to the hello within ${timeout.toFixed(0)} s (is the OP-Z in normal mode, `
          + "and the official app closed?)");
      }
      if (attempt === 1) {
        if (this.seen.has(BANK_PART) || this.seen.has(BANK_FINAL)) {
          this.log(`the bank did not arrive whole; waiting ${STATE_EXPIRY.toFixed(1)} s for the session to expire`);
        } else {
          this.log(`no sound burst (a state session is still live); waiting ${STATE_EXPIRY.toFixed(1)} s for it to expire`);
        }
        const end = this.clock() + STATE_EXPIRY;
        while (this.clock() < end) await this.take();
        this.acked = false;
      }
    }
    const missing = tracks.filter((t) => !this.sounds.has(t));
    throw new SessionError(`the OP-Z sent no sound for track(s) [${missing.join(", ")}]; nothing written`);
  }

  /** Keep the session alive for `seconds`; returns the `$0E` records the OP-Z sent meanwhile, in order. */
  async pump(seconds: number): Promise<Array<[number, number[]]>> {
    const seen: Array<[number, number[]]> = [];
    const end = this.clock() + seconds;
    while (this.clock() < end) {
      await this.hello();
      const sound = await this.take();
      if (sound !== null) seen.push(sound);
    }
    return seen;
  }

  /**
   * One short step for live editing (a hello when due, one receive): an editor sends `$0E` between ticks, at most
   * about every 31 ms like TE's app does. Not used by the recorded flows.
   */
  async tick(timeoutMs = 31): Promise<[number, number[]] | null> {
    await this.hello();
    return this.take(timeoutMs);
  }

  /** Send `$0E`: the track's sound in the active pattern changes at once (no reply). */
  async set(track: number, values: readonly number[]): Promise<void> {
    await this.t.send(soundFrame(track, values));
    await this.hello();
  }

  close(): void {
    this.log("state session closed (the OP-Z ends it ~5 s after the last hello)");
  }
}
