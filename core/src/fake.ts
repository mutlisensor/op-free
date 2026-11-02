/**
 * An in-memory OP-Z that speaks the device side of the SysEx file service. Port of tools/fake_opz.py, for offline
 * tests and a demo mode without hardware. It models what the documentation and the app's code say the firmware does,
 * not everything it does:
 *
 * - `$00` hello then `$51` -> `$34` (client id, state CRC); nothing is answered before the hello.
 * - Downloads: only settings/slotConfiguration.json (id 0) and settings/plugs.json (id 1) are served; one `$35`
 *   starts an auto-advancing `$53` stream.
 * - `$52` sync-active 1 -> requests syncjob.json (id 2), then every listed file unless the file already at that path
 *   has the listed CRC (a CRC of 0 is always requested; FUN_8012bb00), reports `$33` progress and finally `$33` 127.
 * - Each download is staged in inbox/<path>, and the OS makes only the last folder of that path (one mkdir,
 *   FUN_80129454): inbox/a/ must exist for a/b/x to stage, and it exists only after a one-level a/x was written
 *   (`inboxDirs`, kept across syncs). A file that can't be staged is still "downloaded" and the sync still ends with
 *   127, but finalising copies nothing ("invalid file crc", FUN_80127a24). At the end of the sync every staged file
 *   whose CRC matches is copied to its path (Q-49).
 * - After a sync: settings/patch.json is imported like PlugManager::importPatch (`auto_import` plugs skipped unless
 *   their version is newer; top-level `plugs[]` imported without a version check; `tracks` = zero-based track
 *   indices, assigned to the first free of 10 slots unless already on that track), then deleted (`keepPatch` keeps
 *   it, as TE's app code does, so an identical re-push is skipped). `remove` operations delete the plug and its sample
 *   but keep the engine file and engine definition (orphans), as observed on hardware. Any one-level path can be
 *   written (a sample pack in samplepacks/ too); `contentMode()` models the user-sample cleanup when content mode ends.
 * - The boot file settings/plugs_patch.json: imported at once after a sync that wrote it (the OS imports any path
 *   containing "patch.json") and kept; `restart()` imports it again and deletes it, even if it does not parse.
 * - Presets (research/presets.md): kept per plug in `presets` (not in plugs.json); re-importing a plug merges by
 *   slot/index; `restart()` drops them all, as a reboot does, before the boot file's import. `applyPreset()` models
 *   choosing the plug on a track.
 * - StateSync as on firmware 1.2.45 (research/patterns.md §5): every `$00` hello gets `$01`; the first hello of
 *   a session also gets `$07` (play state: byte 17 = the active pattern address, project × 16 + pattern; byte 19 = the
 *   project), `$0C` (the global block) and part 0 of the active project's bank (`$09`, or `$0A` if it is the only
 *   part). Each further part goes out only after the host's `$0B` ack of the part before; `$08` starts the bank over.
 *   The bank is the project file in `files` (else the OS's empty project) with `sounds` as the active pattern's sound
 *   block, compressed by `bankStream`. `soundBurst` models the notes repo's description instead (one `$0E` per track
 *   in the burst and on a preset change), which 1.2.45 does not do. A host `$0E` of track + 18 bytes sets that
 *   track's sound while a session is live. The session ends 5 s after the last hello. Time is the fake's own: like the
 *   vector recorder's clock, a receive() that returns a frame advances it by 1 ms and one that returns nothing by its
 *   timeout (`elapse()` for callers that wait themselves).
 */

import { concat, readU16le, readU32le, u16le, u32le, utf8 } from "./bytes.ts";
import { BOOT_PATCH, type Doc } from "./plan.ts";
import { Project, projectPath } from "./project.ts";
import { dumps, loads, pyInt, pyRepr } from "./pyjson.ts";
import type { Transport } from "./session.ts";
import { compress, decodeFrame, decompress, frame } from "./sysex.ts";
import { adler32, crc32 } from "./zlib.ts";

const PART = 200;
const SERVED: Record<number, string> = { 0: "settings/slotConfiguration.json", 1: "settings/plugs.json" };
const LATENCY = 0.001;
const STATE_TIMEOUT = 5.0;
const OPZ_SESSION = 0x95f5;
const BANK_CHUNK = 178;
const BANK_TRANSFER = 0; // the transfer id the user's OP-Z sent (2026-10-26)
/** A neutral sound per track for the fake's active pattern: P1, P2 centred, sustain full, filter open, pan centre. */
export const DEFAULT_SOUND = [128, 128, 0, 64, 255, 64, 0, 0, 255, 0, 128, 200, 0, 0, 128, 0, 0, 0];

/** A preset as the OP-Z keeps it (after readPresetSettings). */
export interface DevicePreset {
  slot: number;
  index: number;
  name: string;
  defaultOctave: number;
  values: number[];
}

/** FakeOpz's state besides its files, JSON-ready (the shared test vectors carry it). */
export interface FakeState {
  now: number;
  last_hello: number | null;
  sounds: number[][];
  presets: Record<string, DevicePreset[]>;
}

// Fixed Huffman codes (RFC 1951 3.2.6) and the length / distance code tables
const LENGTH_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163,
  195, 227, 258];
const LENGTH_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
  4097, 6145, 8193, 12289, 16385, 24577];
const DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];

/**
 * A zlib stream of `data`: one fixed-Huffman block, greedy matches against the latest earlier position of the same
 * three bytes (window 32 KiB, length up to 258). Byte for byte tools/fake_opz.py `bank_stream`; zlib's own deflate is
 * reproduced here only up to 16 KB, and the OP-Z's own compressor need not be matched (any valid stream inflates the
 * same).
 */
export function bankStream(data: Uint8Array): Uint8Array {
  const out: number[] = [0x78, 0x01];
  let acc = 0;
  let nbits = 0;
  const bits = (value: number, n: number): void => { // LSB first
    acc |= value << nbits;
    nbits += n;
    while (nbits >= 8) {
      out.push(acc & 0xff);
      acc >>>= 8;
      nbits -= 8;
    }
  };
  const reversed = (c: number, n: number): number => {
    let r = 0;
    for (let k = 0; k < n; k++) r = (r << 1) | ((c >> k) & 1);
    return r;
  };
  const code = (sym: number): void => { // fixed Huffman code, written MSB first
    if (sym < 144) bits(reversed(0x30 + sym, 8), 8);
    else if (sym < 256) bits(reversed(0x190 + sym - 144, 9), 9);
    else if (sym < 280) bits(reversed(sym - 256, 7), 7);
    else bits(reversed(0xc0 + sym - 280, 8), 8);
  };
  const key = (j: number): number => (data[j] << 16) | (data[j + 1] << 8) | data[j + 2];

  bits(1, 1); // BFINAL
  bits(1, 2); // BTYPE 01: fixed Huffman
  const head = new Map<number, number>();
  const n = data.length;
  let i = 0;
  while (i < n) {
    let length = 0;
    let cand: number | undefined;
    if (i + 3 <= n) {
      cand = head.get(key(i));
      if (cand !== undefined && i - cand <= 32768) {
        const most = Math.min(258, n - i);
        while (length < most && data[cand + length] === data[i + length]) length++;
      }
    }
    if (length >= 3) {
      let lc = 28;
      while (LENGTH_BASE[lc] > length) lc--;
      code(257 + lc);
      bits(length - LENGTH_BASE[lc], LENGTH_EXTRA[lc]);
      const dist = i - cand!;
      let dc = 29;
      while (DIST_BASE[dc] > dist) dc--;
      bits(reversed(dc, 5), 5);
      bits(dist - DIST_BASE[dc], DIST_EXTRA[dc]);
    } else {
      code(data[i]);
      length = 1;
    }
    for (let j = i; j < Math.min(i + length, n - 2); j++) head.set(key(j), j);
    i += length;
  }
  code(256);
  if (nbits) out.push(acc & 0xff);
  const a = adler32(data);
  out.push(a >>> 24, (a >>> 16) & 0xff, (a >>> 8) & 0xff, a & 0xff);
  return Uint8Array.from(out);
}

function isInt(v: unknown): v is number {
  return typeof v === "number" && Number.isInteger(v);
}

/** The fake's factory state: three TE plugs, lead track full. Serialised as Python's json.dumps does. */
export function defaultFiles(): Map<string, Uint8Array> {
  const plugs = {
    engines: [{ memoryslot: 27, path: "engines/analoga.engine", version: 2 },
      { memoryslot: 10, path: "engines/sampleplayergain.engine", version: 2 },
      { memoryslot: 79, path: "engines/crush.engine", version: 1 }],
    plugs: [{ engine: "engines/analoga.engine", flags: 20, id: 10, samples: "", version: 25 },
      { engine: "engines/sampleplayergain.engine", flags: 0, id: 121, samples: "samplepacks/TeKicks.aif", version: 1 },
      { engine: "engines/crush.engine", flags: 1, id: 109, samples: "", version: 1 }],
  };
  const slots = {
    tracks: [{ index: 0, packs: [{ id: 121, slot: 1 }] },
      { index: 4, packs: [{ id: 10, slot: 1 }] },
      { index: 5, packs: Array.from({ length: 10 }, (_, i) => ({ id: 10, slot: i + 1 })) }, // lead is full
      { index: 8, packs: [{ id: 109, slot: 1 }] },
      { index: 9, packs: [{ id: 109, slot: 1 }] }],
  };
  return new Map([
    ["settings/plugs.json", utf8(dumps(plugs))],
    ["settings/slotConfiguration.json", utf8(dumps(slots))],
    ["samplepacks/TeKicks.aif", utf8("FORM-fake")],
  ]);
}

/** Transport-compatible: the host calls send() with its frames and receive() for the OP-Z's frames. */
export class FakeOpz implements Transport {
  files: Map<string, Uint8Array>;
  readonly clientId: number;
  readonly out: Uint8Array[] = [];
  hello = false;
  serverId: number | null = null;
  syncing = false;
  incoming = new Map<number, Uint8Array[]>();
  manifest: Doc[] = [];
  queue: Doc[] = [];
  partsReceived = 0;
  readonly log: string[] = [];
  syncCount = 0;
  written = new Set<string>();
  /** Folders under inbox/ (one level made per download, kept across syncs). */
  inboxDirs = new Set<string>();
  /** This sync's downloads, by path, and the paths it requested. */
  staged = new Map<string, Uint8Array>();
  requested = new Set<string>();
  keepPatch: boolean;
  /** The notes repo's burst (one `$0E` per track) instead of 1.2.45's bank. */
  soundBurst: boolean;
  /** The active project and pattern, zero-based: project 9 pattern 3, as on the user's unit. */
  project = 8;
  pattern = 2;
  /** The bank transfer in progress. */
  bankOut: { chunks: Uint8Array[]; next: number } | null = null;
  banksSent = 0;
  now = 0;
  lastHello: number | null = null;
  sounds: number[][] = Array.from({ length: 16 }, () => [...DEFAULT_SOUND]);
  /** plug id -> "slot/index" -> preset. */
  presets = new Map<number, Map<string, DevicePreset>>();

  constructor(files?: Map<string, Uint8Array> | Record<string, Uint8Array>, clientId = 21899, keepPatch = false,
    soundBurst = false) {
    const source = files === undefined ? defaultFiles() : files instanceof Map ? files : new Map(Object.entries(files));
    this.files = new Map(source);
    this.clientId = clientId;
    this.keepPatch = keepPatch;
    this.soundBurst = soundBurst;
  }

  // transport API --------------------------------------------------------------------------------------------
  send(raw: Uint8Array): void {
    const f = decodeFrame(raw);
    switch (f.messageId) {
      case 0x00: return this.on00(f.payload);
      case 0x08: return this.on08(f.payload);
      case 0x0b: return this.on0b(f.payload);
      case 0x0e: return this.on0e(f.payload);
      case 0x51: return this.on51(f.payload);
      case 0x52: return this.on52(f.payload);
      case 0x55: return this.on55(f.payload);
      case 0x35: return this.on35(f.payload);
      case 0x53: return this.on53(f.payload);
    }
  }

  async receive(timeoutMs: number): Promise<Uint8Array | null> {
    const raw = this.out.shift();
    if (raw !== undefined) {
      this.now += LATENCY;
      return raw;
    }
    this.now += timeoutMs / 1000;
    return null;
  }

  /** Let time pass without receiving (for transports that do their own waiting). */
  elapse(seconds: number): void {
    this.now += seconds;
  }

  state(): FakeState {
    const presets: Record<string, DevicePreset[]> = {};
    for (const pid of [...this.presets.keys()].sort((a, b) => a - b)) {
      const ps = [...this.presets.get(pid)!.values()].sort((a, b) => a.slot - b.slot || a.index - b.index);
      presets[String(pid)] = ps.map((p) => ({ ...p, values: [...p.values] }));
    }
    return { now: this.now, last_hello: this.lastHello, sounds: this.sounds.map((v) => [...v]), presets };
  }

  loadState(state: FakeState): void {
    this.now = state.now;
    this.lastHello = state.last_hello;
    this.sounds = state.sounds.map((v) => [...v]);
    this.presets = new Map(Object.entries(state.presets).map(([pid, ps]) => [
      Number(pid), new Map(ps.map((p) => [`${p.slot}/${p.index}`, { ...p, values: [...p.values] }])),
    ]));
  }

  // device-side actions (no host involved) ---------------------------------------------------------------------
  stateLive(): boolean {
    return this.lastHello !== null && this.now - this.lastHello <= STATE_TIMEOUT;
  }

  /**
   * A reboot: imported presets are gone (plugs.json has none), any StateSync session ends; then the boot file, if there
   * is one, is imported and deleted (boot init 0x801007c2: import, delete, then the preset caches).
   */
  restart(): void {
    this.presets = new Map();
    this.lastHello = null;
    this.log.push("restart");
    const raw = this.files.get(BOOT_PATCH);
    if (raw === undefined) return;
    this.files.delete(BOOT_PATCH);
    let doc: Doc;
    try {
      doc = loads(raw);
    } catch {
      this.log.push("Failed to import update file");
      return;
    }
    this.log.push(`boot import ${BOOT_PATCH}`);
    this.importPatch(doc);
  }

  /**
   * Leaving content mode (research/samples.md, FUN_80038cd8): user-sample plugs (flag 2) whose file is missing or
   * that sit in no slot are dropped (FUN_800ffc88), then every file in samplepacks/user/ that no plug names is deleted
   * (FUN_800364c4). Returns the deleted paths.
   */
  contentMode(): string[] {
    const [plugs, slots] = this.load();
    const assigned = new Set<number>(slots.tracks.flatMap((t: Doc) => t.packs.map((p: Doc) => p.id)));
    const keep: Doc[] = [];
    for (const p of plugs.plugs) {
      if ((p.flags ?? 0) & 2 && (!this.files.has(p.samples ?? "") || !assigned.has(p.id))) {
        this.log.push(`found unassigned or dangling user sample ${p.id}, removing`);
        this.presets.delete(p.id);
      } else keep.push(p);
    }
    plugs.plugs = keep;
    const named = new Set(keep.map((p) => p.samples));
    const gone = [...this.files.keys()].filter((f) => f.startsWith("samplepacks/user/") && !named.has(f)).sort();
    for (const f of gone) {
      this.files.delete(f);
      this.log.push(`Remove unused sample pack ${f}`);
    }
    this.save(plugs, slots);
    return gone;
  }

  /**
   * The OP-Z choosing a preset (index 0 when a track switches to the plug): slot = track − 4 on tracks 4–7, else 0;
   * index 0 if `index` is missing; keeps pan (and on the arp track the LFO). False if there is none.
   */
  applyPreset(track: number, plugId: number, index = 0): boolean {
    const slot = track >= 4 && track <= 7 ? track - 4 : 0;
    const presets = this.presets.get(plugId) ?? new Map<string, DevicePreset>();
    const p = presets.get(`${slot}/${index}`) ?? presets.get(`${slot}/0`);
    if (p === undefined) return false;
    const keep = new Set(track === 6 ? [10, 13, 14, 15, 16] : [10]);
    this.sounds[track] = p.values.map((v, i) => (keep.has(i) ? this.sounds[track][i] : v));
    this.log.push(`preset ${p.slot}/${p.index} ${pyRepr(p.name)} on track ${track}`);
    if (this.soundBurst && this.stateLive()) this.out.push(frame(0x0e, Uint8Array.of(track, ...this.sounds[track])));
    return true;
  }

  // helpers ----------------------------------------------------------------------------------------------------
  protected cid(): Uint8Array {
    return u16le(this.clientId);
  }

  stateCrc(): number {
    return crc32(concat(this.files.get("settings/plugs.json")!, this.files.get("settings/slotConfiguration.json")!));
  }

  /** The active pattern's address: project × 16 + pattern (StateSync's `$07` byte 17 and `$09` header). */
  address(): number {
    return (this.project << 4) | this.pattern;
  }

  /** The active project in RAM: its file (or the OS's empty project) with `sounds` in the active pattern. */
  activeProject(): Project {
    const data = this.files.get(projectPath(this.project + 1));
    let pr: Project;
    try {
      pr = data !== undefined ? Project.parse(data) : new Project();
    } catch {
      pr = new Project();
    }
    this.sounds.forEach((values, track) => pr.setSound(this.pattern, track, values));
    return pr;
  }

  protected playState(): Uint8Array {
    const state = new Uint8Array(20);
    state[0] = this.pattern; // the live chain: the active pattern only
    state.set([1, this.address(), 0, this.project], 16);
    return state;
  }

  protected sendBank(): void {
    const z = bankStream(this.activeProject().bank);
    const chunks: Uint8Array[] = [];
    for (let i = 0; i < z.length; i += BANK_CHUNK) chunks.push(z.subarray(i, i + BANK_CHUNK));
    this.bankOut = { chunks, next: 0 };
    this.banksSent += 1;
    this.sendBankPart();
  }

  protected sendBankPart(): void {
    const tx = this.bankOut!;
    const index = tx.next;
    const last = index === tx.chunks.length - 1;
    this.out.push(frame(last ? 0x0a : 0x09, concat(u16le(this.address()), u16le(BANK_TRANSFER), u16le(index),
      tx.chunks[index])));
    tx.next += 1;
    if (last) this.bankOut = null;
  }

  protected request(fid: number): void {
    this.out.push(frame(0x35, concat(this.cid(), u32le(fid), u16le(0))));
  }

  // handlers ---------------------------------------------------------------------------------------------------
  protected on00(p: Uint8Array): void {
    this.hello = true;
    const fresh = !this.stateLive();
    this.lastHello = this.now;
    this.out.push(frame(0x01, concat(Uint8Array.of(OPZ_SESSION >> 8, OPZ_SESSION & 0xff), p.subarray(0, 2))));
    if (fresh && this.soundBurst) {
      this.sounds.forEach((values, track) => this.out.push(frame(0x0e, Uint8Array.of(track, ...values))));
    } else if (fresh) {
      this.out.push(frame(0x07, this.playState()));
      this.out.push(frame(0x0c, compress(this.activeProject().glob)));
      this.sendBank();
    }
  }

  /** The host asks for the bank again (any payload). */
  protected on08(_p: Uint8Array): void {
    if (this.stateLive() && !this.soundBurst) {
      this.log.push("bank requested again");
      this.sendBank();
    }
  }

  /** The host acks a bank part. */
  protected on0b(p: Uint8Array): void {
    const tx = this.bankOut;
    if (!this.stateLive() || tx === null || p.length !== 8 || readU16le(p, 4) !== BANK_TRANSFER
      || readU16le(p, 6) !== tx.next - 1) return;
    this.sendBankPart();
  }

  protected on0e(p: Uint8Array): void {
    if (!this.stateLive() || p.length !== 19 || p[0] > 15) return;
    this.sounds[p[0]] = [...p.subarray(1)];
    this.log.push(`sound track ${p[0]} set`);
  }

  protected on51(p: Uint8Array): void {
    if (this.hello) {
      this.serverId = readU16le(p, 0);
      this.out.push(frame(0x34, concat(this.cid(), u32le(this.stateCrc()))));
    }
  }

  protected on52(p: Uint8Array): void {
    if (p.length >= 3 && p[2] === 1 && !this.syncing && this.serverId !== null) {
      this.syncing = true;
      this.written = new Set();
      this.staged = new Map();
      this.partsReceived = 0;
      this.incoming = new Map();
      this.request(2);
    }
  }

  protected on55(_p: Uint8Array): void {
    this.serverId = null;
  }

  /** Host requests a download. */
  protected on35(p: Uint8Array): void {
    const fid = readU32le(p, 2);
    const part = readU16le(p, 6);
    const path = SERVED[fid];
    if (path === undefined) return; // not served: no reply at all
    const data = this.files.get(path)!;
    const last = Math.max(0, Math.floor((data.length - 1) / PART));
    for (let i = part; i <= last; i++) {
      const chunk = data.subarray(i * PART, (i + 1) * PART);
      this.out.push(frame(0x53, concat(this.cid(), u32le(fid), u16le(i), Uint8Array.of(i === last ? 1 : 0),
        compress(chunk))));
    }
  }

  /** Host serves a file part. */
  protected on53(p: Uint8Array): void {
    if (!this.syncing) return;
    const fid = readU32le(p, 2);
    const part = readU16le(p, 6);
    const last = p[8];
    if (!this.incoming.has(fid)) this.incoming.set(fid, []);
    const buf = this.incoming.get(fid)!;
    if (part * PART !== buf.reduce((n, b) => n + b.length, 0)) return; // out of order: ignore (host must resend)
    buf.push(decompress(p.subarray(9)));
    this.partsReceived += 1;
    if (!last) {
      this.out.push(frame(0x33, concat(this.cid(), Uint8Array.of(1), u32le(this.partsReceived))));
      return;
    }
    const data = concat(...buf);
    this.incoming.delete(fid);
    if (fid === 2) {
      this.manifest = loads(data).files;
      this.queue = this.manifest.filter((e) => "fileId" in e && this.differs(e));
      this.requested = new Set(this.queue.map((e) => e.path));
      this.log.push(`manifest: ${this.manifest.length} entries, ${this.queue.length} to fetch`);
    } else {
      const entry = this.queue.shift();
      if (entry === undefined) throw new Error("pop from empty list");
      if (entry.fileId === fid && this.stage(entry.path)) this.staged.set(entry.path, data);
    }
    if (this.queue.length) this.request(this.queue[0].fileId);
    else this.finishSync();
  }

  protected differs(entry: Doc): boolean {
    const current = this.files.get(entry.path);
    return entry.crc === 0 || current === undefined || crc32(current) !== entry.crc;
  }

  /** Make inbox/<folder of path> with one mkdir, as FUN_80129454 does; false when its parent is missing. */
  protected stage(path: string): boolean {
    const folder = path.includes("/") ? path.slice(0, path.lastIndexOf("/")) : "";
    const parent = folder.includes("/") ? folder.slice(0, folder.lastIndexOf("/")) : "";
    if (folder && parent && !this.inboxDirs.has(parent)) {
      this.log.push(`failed to write file inbox/${path} (no folder inbox/${parent})`);
      return false;
    }
    if (folder) this.inboxDirs.add(folder);
    return true;
  }

  // post-sync effects ------------------------------------------------------------------------------------------
  protected finishSync(): void {
    for (const e of this.manifest) { // FUN_80127a24: copy each staged file in place
      if (!("fileId" in e) || (e.operation ?? "write") !== "write") continue;
      const data = this.staged.get(e.path);
      if (data === undefined || crc32(data) !== e.crc || data.length !== e.size) {
        if (this.requested.has(e.path)) this.log.push(`invalid file crc, can't finalize ${e.path}: not written`);
        continue;
      }
      this.files.set(e.path, data);
      this.written.add(e.path);
      this.log.push(`wrote ${e.path} (${data.length} bytes)`);
    }
    for (const e of this.manifest) if (e.operation === "remove") this.removePlug(pyInt(e.plugId));
    const patch = this.files.get("settings/patch.json");
    if (patch !== undefined && this.written.has("settings/patch.json")) {
      if (!this.keepPatch) this.files.delete("settings/patch.json");
      this.importPatch(loads(patch));
    }
    const boot = this.files.get(BOOT_PATCH);
    if (boot !== undefined && this.written.has(BOOT_PATCH)) this.importPatch(loads(boot)); // kept until the next start
    this.syncing = false;
    this.syncCount += 1;
    this.out.push(frame(0x33, concat(this.cid(), Uint8Array.of(127), u32le(0))));
  }

  protected load(): [Doc, Doc] {
    return [loads(this.files.get("settings/plugs.json")!), loads(this.files.get("settings/slotConfiguration.json")!)];
  }

  protected save(plugs: Doc, slots: Doc): void {
    plugs.engines.sort((a: Doc, b: Doc) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
    plugs.plugs.sort((a: Doc, b: Doc) => a.id - b.id);
    this.files.set("settings/plugs.json", utf8(dumps(plugs)));
    this.files.set("settings/slotConfiguration.json", utf8(dumps(slots)));
  }

  importPatch(doc: Doc): void {
    const [plugs, slots] = this.load();
    const engine = (e: Doc) => {
      plugs.engines = plugs.engines.filter((x: Doc) => x.path !== e.path);
      plugs.engines.push({ memoryslot: e.memoryslot, path: e.path, version: e.version });
    };
    const plug = (pl: Doc, checkVersion = true): boolean => {
      const old = plugs.plugs.find((x: Doc) => x.id === pl.id);
      if (checkVersion && old !== undefined && old.version >= pl.version) {
        this.log.push(`plug ${pl.id} skipped (version ${pl.version} <= ${old.version})`);
        return false;
      }
      plugs.plugs = plugs.plugs.filter((x: Doc) => x.id !== pl.id);
      plugs.plugs.push({ engine: pl.engine, flags: pl.flags ?? 0, id: pl.id, samples: pl.samples ?? "", version: pl.version });
      this.readPresets(pl.id, pl.presets ?? [], old !== undefined);
      return true;
    };
    for (const entry of doc.auto_import ?? []) {
      engine(entry.engine);
      plug(entry.plug);
      for (const t of entry.tracks ?? []) this.assignFirstFree(slots, pyInt(t), entry.plug.id);
    }
    for (const e of doc.engines ?? []) engine(e);
    for (const pl of doc.plugs ?? []) plug(pl, false);
    this.save(plugs, slots);
  }

  /**
   * readPresetSettings: index 0–13, string name, array values; slot clamped 0–3; 11-character names; defaultOctave −1
   * if missing, clamped −4…4. A new plug starts empty; an existing one merges.
   */
  protected readPresets(plugId: number, presets: unknown, merge: boolean): void {
    let store = this.presets.get(plugId);
    if (store === undefined) this.presets.set(plugId, store = new Map());
    if (!merge) store.clear();
    for (const p of Array.isArray(presets) ? presets as Doc[] : []) {
      const index = p.index;
      if (!isInt(index) || index < 0 || index > 13) continue;
      if (typeof p.name !== "string" || !Array.isArray(p.values)) continue;
      const slot = Math.min(Math.max(isInt(p.slot) ? p.slot : 0, 0), 3);
      const octave = Math.min(Math.max("defaultOctave" in p && isInt(p.defaultOctave) ? p.defaultOctave : -1, -4), 4);
      const values = (p.values as unknown[]).slice(0, 18).map((v) => (isInt(v) ? v & 0xff : 0));
      while (values.length < 18) values.push(0);
      store.set(`${slot}/${index}`, { slot, index, name: [...p.name].slice(0, 11).join(""), defaultOctave: octave, values });
    }
    this.log.push(`plug ${plugId}: ${store.size} presets`);
  }

  protected assignFirstFree(slots: Doc, track: number, plugId: number): void {
    let tr = slots.tracks.find((t: Doc) => t.index === track);
    if (tr === undefined) {
      tr = { index: track, packs: [] };
      slots.tracks.push(tr);
    }
    if (tr.packs.some((p: Doc) => p.id === plugId)) {
      this.log.push("plug already exist on track");
      return;
    }
    const used = new Set(tr.packs.map((p: Doc) => p.slot));
    let free: number | null = null;
    for (let s = 1; s < 11; s++) if (!used.has(s)) {
      free = s;
      break;
    }
    if (free === null) {
      this.log.push("No free slots to assign plug to");
      return;
    }
    tr.packs.push({ id: plugId, slot: free });
    tr.packs.sort((a: Doc, b: Doc) => a.slot - b.slot);
  }

  protected removePlug(plugId: number): void {
    const [plugs, slots] = this.load();
    const index = plugs.plugs.findIndex((x: Doc) => x.id === plugId);
    if (index < 0) return;
    const [pl] = plugs.plugs.splice(index, 1);
    this.presets.delete(plugId);
    if (pl.samples) this.files.delete(pl.samples);
    this.log.push(`remove user plug ${plugId}`);
    this.save(plugs, slots);
  }
}
