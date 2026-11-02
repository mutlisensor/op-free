"""An in-memory OP-Z that speaks the device side of the SysEx file service, for offline tests.

It models what the documentation and the app's code say the firmware does, not everything it does:

- `$00` hello then `$51` -> `$34` (client id, state CRC); nothing is answered before the hello.
- Downloads: only `settings/slotConfiguration.json` (id 0), `settings/plugs.json` (id 1) and the ten projects
  `projects/project01.opz`..`project10.opz` (ids 3-12; a project not in `files` is served as the OS's empty project)
  are served; one `$35` starts an auto-advancing `$53` stream.
- `$52` sync-active 1 -> requests `syncjob.json` (id 2), then every listed file unless the file already at that path
  has the listed CRC (a CRC of 0 is always requested; FUN_8012bb00), reports `$33` progress and finally `$33` 127.
- Each download is staged in `inbox/<path>` first, and the OS makes only the last folder of that path (one mkdir,
  FUN_80129454): `inbox/a/` must exist for `a/b/x` to stage, and it exists only after a one-level `a/x` was written
  (`inbox_dirs`, kept across syncs). A file that can't be staged is still "downloaded" and the sync still ends with
  127, but finalising finds no file with the listed CRC and copies nothing ("invalid file crc", FUN_80127a24). At the
  end of the sync every staged file whose CRC matches is copied to its path (Q-49).
- After a sync: `settings/patch.json` is imported like `PlugManager::importPatch` (`auto_import` plugs skipped
  unless their version is newer; top-level `plugs[]` imported without a version check; `tracks` = zero-based track
  indices, assigned to the first free of 10 slots unless already on that track), then deleted (`keep_patch=True`
  keeps it, as TE's app code does, so an identical re-push is skipped). `remove` operations delete the plug and its
  sample but keep the engine file and engine definition (orphans), as observed on hardware.
- The boot file `settings/plugs_patch.json`: imported at once after a sync that wrote it (the OS imports any path
  containing "patch.json") and kept; `restart()` imports it again and deletes it, even if it does not parse.
- Any one-level path can be written (a sample pack in samplepacks/ too); `content_mode()` models the user-sample
  cleanup when content mode ends. A written path containing "projects/project" is reloaded into RAM slot NN-1 like
  the OS's FileClient (`reloaded`), with no bounds check: a project00 or project11+ write is logged as corrupting RAM.
- Presets (research/presets.md): kept per plug in `presets` (not in plugs.json); re-importing a plug merges by
  `slot`/`index`; `restart()` drops them all, as a reboot does, before the boot file's import. `apply_preset()` models
  choosing the plug on a track.
- StateSync as on firmware 1.2.45 (research/patterns.md §5): every `$00` hello gets `$01`; the first hello of a
  session also gets `$07` (play state: byte 17 = the active pattern address, project × 16 + pattern; byte 19 = the
  project), `$0C` (the global block) and part 0 of the active project's bank (`$09`, or `$0A` if it is the only
  part). Each further part goes out only after the host's `$0B` ack of the part before (`09 00 00 00`, transfer id,
  part index); `$08` starts the bank over. The bank is the project file in `files` (else the OS's empty project)
  with `sounds` as the active pattern's sound block (pattern `pattern` of project `project`, zero-based; default
  project 9 pattern 3 as on the user's unit). It is compressed by `bank_stream` (fixed-Huffman deflate, simple
  enough for core to reproduce byte for byte; the OP-Z's own compressor need not be matched, any valid zlib
  stream inflates the same). `sound_burst=True` models the notes repo's description instead (one `$0E` per track in
  the burst and on a preset change), which 1.2.45 does not do [proven 2026-10-26]. A host `$0E` of track + 18 bytes
  sets that track's sound while a session is live. The session ends 5 s after the last hello. Time is the fake's
  own: like the vector recorder's clock, a receive() that returns a frame advances it by 1 ms and one that returns
  nothing by its timeout.
- MIDI notes (for tools/opz_bench.py): `send_short()` takes note on/off (track = channel, as the OP-Z's default map)
  and `render()` synthesises them as a test tone: a sine at the note's pitch, level by velocity, 5 ms attack and
  30 ms release; drum tracks (0–3) decay in 0.15 s. Faults for the runner's checks: `crash_at` (from then on no
  replies, no ports, no audio), `faults["dropouts"]` [(t0, t1)] of silence, `faults["clicks"]` [t] one-sample
  steps, `faults["detune_cents"]`.
"""

from __future__ import annotations

import json
import math
import os
import sys
import zlib
from array import array
from collections import deque

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "third_party", "opzsysex"))
from opzsysex.sysex import decode_frame, frame  # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import opz_project  # noqa: E402

PART = 200
SERVED = {0: "settings/slotConfiguration.json", 1: "settings/plugs.json"}
BOOT_PATCH = "settings/plugs_patch.json"
LATENCY = 0.001
STATE_TIMEOUT = 5.0
OPZ_SESSION = 0x95F5
BANK_PART, BANK_FINAL, BANK_ACK, BANK_REQUEST = 0x09, 0x0A, 0x0B, 0x08
BANK_CHUNK = 178
BANK_TRANSFER = 0                    # the transfer id the user's OP-Z sent (2026-10-26)
# A neutral sound per track for the fake's active pattern: P1, P2 centred, sustain full, filter open, pan centre.
DEFAULT_SOUND = [128, 128, 0, 64, 255, 64, 0, 0, 255, 0, 128, 200, 0, 0, 128, 0, 0, 0]


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def file_id(path: str) -> int:
    return zlib.crc32(path.lower().encode()) & 0xFFFFFFFF


# Fixed Huffman codes (RFC 1951 3.2.6) and the length / distance code tables
_LENGTH_BASE = (3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163,
                195, 227, 258)
_LENGTH_EXTRA = (0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0)
_DIST_BASE = (1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
              4097, 6145, 8193, 12289, 16385, 24577)
_DIST_EXTRA = (0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13)


def bank_stream(data: bytes) -> bytes:
    """A zlib stream of `data`: one fixed-Huffman block, greedy matches against the latest earlier position of the
    same three bytes (window 32 KiB, length up to 258). Valid for any inflater and simple enough for
    core/src/fake.ts to reproduce byte for byte, unlike zlib's own deflate for inputs this size."""
    out = bytearray(b"\x78\x01")
    acc = nbits = 0

    def bits(value: int, n: int) -> None:            # LSB first
        nonlocal acc, nbits
        acc |= value << nbits
        nbits += n
        while nbits >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nbits -= 8

    def code(sym: int) -> None:                      # fixed Huffman code, written MSB first
        if sym < 144:
            c, n = 0x30 + sym, 8
        elif sym < 256:
            c, n = 0x190 + sym - 144, 9
        elif sym < 280:
            c, n = sym - 256, 7
        else:
            c, n = 0xC0 + sym - 280, 8
        bits(int(format(c, "0%db" % n)[::-1], 2), n)

    bits(1, 1)                                       # BFINAL
    bits(1, 2)                                       # BTYPE 01: fixed Huffman
    head: dict[int, int] = {}
    n, i = len(data), 0
    while i < n:
        length = 0
        if i + 3 <= n:
            key = data[i] << 16 | data[i + 1] << 8 | data[i + 2]
            cand = head.get(key)
            if cand is not None and i - cand <= 32768:
                most = min(258, n - i)
                while length < most and data[cand + length] == data[i + length]:
                    length += 1
        if length >= 3:
            lc = next(k for k in range(28, -1, -1) if _LENGTH_BASE[k] <= length)
            code(257 + lc)
            bits(length - _LENGTH_BASE[lc], _LENGTH_EXTRA[lc])
            dist = i - cand
            dc = next(k for k in range(29, -1, -1) if _DIST_BASE[k] <= dist)
            bits(int(format(dc, "05b")[::-1], 2), 5)
            bits(dist - _DIST_BASE[dc], _DIST_EXTRA[dc])
        else:
            code(data[i])
            length = 1
        for j in range(i, min(i + length, n - 2)):
            head[data[j] << 16 | data[j + 1] << 8 | data[j + 2]] = j
        i += length
    code(256)
    if nbits:
        out.append(acc & 0xFF)
    return bytes(out) + (zlib.adler32(data) & 0xFFFFFFFF).to_bytes(4, "big")


def default_files() -> dict[str, bytes]:
    plugs = {
        "engines": [{"memoryslot": 27, "path": "engines/analoga.engine", "version": 2},
                    {"memoryslot": 10, "path": "engines/sampleplayergain.engine", "version": 2},
                    {"memoryslot": 79, "path": "engines/crush.engine", "version": 1}],
        "plugs": [{"engine": "engines/analoga.engine", "flags": 20, "id": 10, "samples": "", "version": 25},
                  {"engine": "engines/sampleplayergain.engine", "flags": 0, "id": 121, "samples": "samplepacks/TeKicks.aif", "version": 1},
                  {"engine": "engines/crush.engine", "flags": 1, "id": 109, "samples": "", "version": 1}],
    }
    slots = {"tracks": [{"index": 0, "packs": [{"id": 121, "slot": 1}]},
                        {"index": 4, "packs": [{"id": 10, "slot": 1}]},
                        {"index": 5, "packs": [{"id": 10, "slot": s} for s in range(1, 11)]},   # lead is full
                        {"index": 8, "packs": [{"id": 109, "slot": 1}]},
                        {"index": 9, "packs": [{"id": 109, "slot": 1}]}]}
    return {"settings/plugs.json": json.dumps(plugs).encode(),
            "settings/slotConfiguration.json": json.dumps(slots).encode(),
            "samplepacks/TeKicks.aif": b"FORM-fake"}


class FakeOpz:
    """Transport-compatible: the host calls send() with its frames and receive() for the OP-Z's frames."""

    def __init__(self, files: dict[str, bytes] | None = None, client_id: int = 21899, keep_patch: bool = False,
                 inbox_dirs: set[str] | None = None, sound_burst: bool = False):
        self.files = dict(default_files() if files is None else files)
        self.inbox_dirs = set(inbox_dirs or ())                 # folders under inbox/ (one level made per download)
        self.staged: dict[str, bytes] = {}                      # this sync's downloads, by path
        self.requested: set[str] = set()
        self.keep_patch = keep_patch
        self.sound_burst = sound_burst
        self.project = 8                                       # active project and pattern, zero-based
        self.pattern = 2
        self.bank_out: dict | None = None                      # the bank transfer in progress
        self.banks_sent = 0
        self.now = 0.0
        self.last_hello: float | None = None
        self.sounds = [list(DEFAULT_SOUND) for _ in range(16)]
        self.presets: dict[int, dict[tuple[int, int], dict]] = {}
        self.client_id = client_id
        self.out: deque[bytes] = deque()
        self.hello = False
        self.server_id: int | None = None
        self.syncing = False
        self.incoming: dict[int, bytearray] = {}
        self.manifest: list[dict] = []
        self.queue: list[dict] = []
        self.parts_received = 0
        self.log: list[str] = []
        self.sync_count = 0
        self.written: set[str] = set()
        self.reloaded: list[int] = []                         # RAM project slots reloaded from a written file
        self.midi: list[tuple[float, int, int, int]] = []       # (time, status, data1, data2)
        self.crash_at: float | None = None
        self.faults: dict = {"dropouts": [], "clicks": [], "detune_cents": 0.0}

    # transport API ----------------------------------------------------------------------------------------
    def crashed(self) -> bool:
        return self.crash_at is not None and self.now >= self.crash_at

    def present(self) -> bool:
        """The MIDI ports are there (gone after a crash: the OP-Z reboots)."""
        return not self.crashed()

    def send_short(self, data: bytes) -> None:
        if not self.crashed():
            self.midi.append((self.now, data[0], data[1], data[2] if len(data) > 2 else 0))

    def send(self, raw: bytes) -> None:
        if self.crashed():
            return
        f = decode_frame(raw)
        handler = getattr(self, "_on_%02x" % f.message_id, None)
        if handler:
            handler(f.payload)

    def receive(self, timeout: float) -> bytes | None:
        if self.crashed():
            self.out.clear()
        if self.out:
            self.now += LATENCY
            return self.out.popleft()
        self.now += timeout
        return None

    def state(self) -> dict:
        """Everything but the files, JSON-ready (the shared test vectors carry it)."""
        return {"now": self.now, "last_hello": self.last_hello, "sounds": [list(v) for v in self.sounds],
                "presets": {str(pid): [dict(self.presets[pid][k]) for k in sorted(self.presets[pid])]
                            for pid in sorted(self.presets)}}

    def load_state(self, state: dict) -> None:
        self.now = state["now"]
        self.last_hello = state["last_hello"]
        self.sounds = [list(v) for v in state["sounds"]]
        self.presets = {int(pid): {(p["slot"], p["index"]): dict(p) for p in ps} for pid, ps in state["presets"].items()}

    # audio (MIDI notes as a test tone) --------------------------------------------------------------------------
    def notes(self) -> list[tuple[float, float, int, int, int]]:
        """(on, off, track, note, velocity) from the MIDI log; a note still held ends at infinity."""
        held, out = {}, []
        for t, status, d1, d2 in self.midi:
            kind, ch = status & 0xF0, status & 0x0F
            if kind == 0x90 and d2 > 0:
                held[(ch, d1)] = (t, d2)
            elif kind in (0x80, 0x90) and (ch, d1) in held:
                on, vel = held.pop((ch, d1))
                out.append((on, t, ch, d1, vel))
        out += [(on, math.inf, ch, n, vel) for (ch, n), (on, vel) in held.items()]
        return sorted(out)

    def render(self, t0: float, t1: float, rate: int = 44100) -> array:
        """Interleaved 16-bit stereo of the fake's output between two of its clock times."""
        n = max(0, int(round((t1 - t0) * rate)))
        buf = [0.0] * n
        detune = 2 ** (self.faults.get("detune_cents", 0.0) / 1200)
        end = self.crash_at if self.crash_at is not None else math.inf
        for on, off, track, note, vel in self.notes():
            hz = 440.0 * 2 ** ((note - 69) / 12) * detune
            amp = 0.3 * vel / 127
            drum = track < 4
            stop = min(off + 0.03, end, on + 0.6 if drum else math.inf, t1)
            if stop <= t0:
                continue
            a, b = max(0, int((on - t0) * rate)), min(n, int((stop - t0) * rate))
            w = 2 * math.pi * hz / rate
            for i in range(a, b):
                t = t0 + i / rate - on
                env = min(1.0, t / 0.005)
                if drum:
                    env *= math.exp(-t / 0.15)
                if t0 + i / rate > off:
                    env *= max(0.0, 1 - (t0 + i / rate - off) / 0.03)
                buf[i] += amp * env * math.sin(w * (i + t0 * rate - on * rate))
        for c in self.faults.get("clicks", []):
            i = int((c - t0) * rate)
            if 0 <= i < n:
                buf[i] += 0.3
        for d0, d1 in self.faults.get("dropouts", []):
            for i in range(max(0, int((d0 - t0) * rate)), min(n, int((d1 - t0) * rate))):
                buf[i] = 0.0
        if self.crash_at is not None:
            for i in range(max(0, int((self.crash_at - t0) * rate)), n):
                buf[i] = 0.0
        out = array("h", bytes(4 * n))
        mono = array("h", (max(-32768, min(32767, int(v * 32767))) for v in buf))
        out[0::2] = mono
        out[1::2] = mono
        return out

    # device-side actions (no host involved) -----------------------------------------------------------------------
    def state_live(self) -> bool:
        return self.last_hello is not None and self.now - self.last_hello <= STATE_TIMEOUT

    def restart(self) -> None:
        """A reboot: imported presets are gone (plugs.json has none), any StateSync session ends; then the boot file,
        if there is one, is imported and deleted (boot init 0x801007c2: import, delete, then the preset caches)."""
        self.presets = {}
        self.last_hello = None
        self.log.append("restart")
        raw = self.files.pop(BOOT_PATCH, None)
        if raw is not None:
            try:
                doc = json.loads(raw)
            except ValueError:
                self.log.append("Failed to import update file")
                return
            self.log.append("boot import %s" % BOOT_PATCH)
            self._import_patch(doc)

    def content_mode(self) -> list[str]:
        """Leaving content mode (research/samples.md, FUN_80038cd8): user-sample plugs (flag 2) whose file is
        missing or that sit in no slot are dropped (FUN_800ffc88), then every file in samplepacks/user/ that no plug
        names is deleted (FUN_800364c4). Returns the deleted paths."""
        plugs, slots = self._load()
        assigned = {p["id"] for t in slots["tracks"] for p in t["packs"]}
        keep = []
        for p in plugs["plugs"]:
            if p.get("flags", 0) & 2 and (p.get("samples", "") not in self.files or p["id"] not in assigned):
                self.log.append("found unassigned or dangling user sample %d, removing" % p["id"])
                self.presets.pop(p["id"], None)
            else:
                keep.append(p)
        plugs["plugs"] = keep
        named = {p.get("samples") for p in keep}
        gone = sorted(f for f in self.files if f.startswith("samplepacks/user/") and f not in named)
        for f in gone:
            del self.files[f]
            self.log.append("Remove unused sample pack %s" % f)
        self._save(plugs, slots)
        return gone

    def apply_preset(self, track: int, plug_id: int, index: int = 0) -> bool:
        """The OP-Z choosing a preset (index 0 when a track switches to the plug): slot = track − 4 on tracks 4–7,
        else 0; index 0 if `index` is missing; keeps pan (and on the arp track the LFO). False if there is none."""
        slot = track - 4 if 4 <= track <= 7 else 0
        presets = self.presets.get(plug_id, {})
        p = presets.get((slot, index)) or presets.get((slot, 0))
        if p is None:
            return False
        keep = {10} | ({13, 14, 15, 16} if track == 6 else set())
        self.sounds[track] = [self.sounds[track][i] if i in keep else v for i, v in enumerate(p["values"])]
        self.log.append("preset %d/%d %r on track %d" % (p["slot"], p["index"], p["name"], track))
        if self.sound_burst and self.state_live():
            self.out.append(frame(0x0E, bytes([track] + self.sounds[track])))
        return True

    # helpers ----------------------------------------------------------------------------------------------
    def _cid(self) -> bytes:
        return self.client_id.to_bytes(2, "little")

    def state_crc(self) -> int:
        return crc32(self.files["settings/plugs.json"] + self.files["settings/slotConfiguration.json"])

    def address(self) -> int:
        """The active pattern's address: project × 16 + pattern (StateSync's `$07` byte 17 and `$09` header)."""
        return self.project << 4 | self.pattern

    def active_project(self) -> opz_project.Project:
        """The active project in RAM: its file (or the OS's empty project) with `sounds` in the active pattern."""
        data = self.files.get(opz_project.project_path(self.project + 1))
        try:
            pr = opz_project.Project.parse(data) if data is not None else opz_project.Project()
        except opz_project.FormatError:
            pr = opz_project.Project()
        for track, values in enumerate(self.sounds):
            pr.set_sound(self.pattern, track, values)
        return pr

    def _play_state(self) -> bytes:
        state = bytearray(20)
        state[0] = self.pattern                              # the live chain: the active pattern only
        state[16:20] = bytes((1, self.address(), 0, self.project))
        return bytes(state)

    def _send_bank(self) -> None:
        z = bank_stream(bytes(self.active_project().bank))
        self.bank_out = {"chunks": [z[i:i + BANK_CHUNK] for i in range(0, len(z), BANK_CHUNK)], "next": 0}
        self.banks_sent += 1
        self._send_bank_part()

    def _send_bank_part(self) -> None:
        tx = self.bank_out
        index, chunks = tx["next"], tx["chunks"]
        final = index == len(chunks) - 1
        self.out.append(frame(BANK_FINAL if final else BANK_PART, self.address().to_bytes(2, "little")
                              + BANK_TRANSFER.to_bytes(2, "little") + index.to_bytes(2, "little") + chunks[index]))
        tx["next"] += 1
        if final:
            self.bank_out = None

    def _request(self, fid: int) -> None:
        self.out.append(frame(0x35, self._cid() + fid.to_bytes(4, "little") + (0).to_bytes(2, "little")))

    # handlers ---------------------------------------------------------------------------------------------
    def _on_00(self, p: bytes) -> None:
        self.hello = True
        fresh = not self.state_live()
        self.last_hello = self.now
        self.out.append(frame(0x01, OPZ_SESSION.to_bytes(2, "big") + p[:2]))
        if fresh and self.sound_burst:
            for track, values in enumerate(self.sounds):
                self.out.append(frame(0x0E, bytes([track] + values)))
        elif fresh:
            self.out.append(frame(0x07, self._play_state()))
            self.out.append(frame(0x0C, zlib.compress(bytes(self.active_project().glob))))
            self._send_bank()

    def _on_08(self, p: bytes) -> None:                      # host asks for the bank again (any payload)
        if self.state_live() and not self.sound_burst:
            self.log.append("bank requested again")
            self._send_bank()

    def _on_0b(self, p: bytes) -> None:                      # host acks a bank part
        tx = self.bank_out
        if (not self.state_live() or tx is None or len(p) != 8
                or int.from_bytes(p[4:6], "little") != BANK_TRANSFER or int.from_bytes(p[6:8], "little") != tx["next"] - 1):
            return
        self._send_bank_part()

    def _on_0e(self, p: bytes) -> None:
        if not self.state_live() or len(p) != 19 or p[0] > 15:
            return
        self.sounds[p[0]] = list(p[1:])
        self.log.append("sound track %d set" % p[0])

    def _on_51(self, p: bytes) -> None:
        if self.hello:
            self.server_id = int.from_bytes(p[:2], "little")
            self.out.append(frame(0x34, self._cid() + self.state_crc().to_bytes(4, "little")))

    def _on_52(self, p: bytes) -> None:
        if len(p) >= 3 and p[2] == 1 and not self.syncing and self.server_id is not None:
            self.syncing = True
            self.written = set()
            self.staged = {}
            self.parts_received = 0
            self.incoming = {}
            self._request(2)

    def _on_55(self, p: bytes) -> None:
        self.server_id = None

    def _on_35(self, p: bytes) -> None:                      # host requests a download
        fid, part = int.from_bytes(p[2:6], "little"), int.from_bytes(p[6:8], "little")
        path = SERVED.get(fid)
        if path is None and 3 <= fid <= 12:
            path = opz_project.project_path(fid - 2)
        if path is None:
            return                                           # not served: no reply at all
        data = self.files[path] if path in self.files else opz_project.Project().to_bytes()
        last = max(0, (len(data) - 1) // PART)
        for i in range(part, last + 1):
            chunk = data[i * PART:(i + 1) * PART]
            self.out.append(frame(0x53, self._cid() + fid.to_bytes(4, "little") + i.to_bytes(2, "little")
                                  + bytes((int(i == last),)) + zlib.compress(chunk)))

    def _on_53(self, p: bytes) -> None:                      # host serves a file part
        if not self.syncing:
            return
        fid, part, last = int.from_bytes(p[2:6], "little"), int.from_bytes(p[6:8], "little"), p[8]
        buf = self.incoming.setdefault(fid, bytearray())
        if part * PART != len(buf):
            return                                           # out of order: ignore (host must resend)
        buf.extend(zlib.decompress(p[9:]))
        self.parts_received += 1
        if not last:
            self.out.append(frame(0x33, self._cid() + bytes((1,)) + self.parts_received.to_bytes(4, "little")))
            return
        data = bytes(self.incoming.pop(fid))
        if fid == 2:
            self.manifest = json.loads(data)["files"]
            self.queue = [e for e in self.manifest if "fileId" in e and self._differs(e)]
            self.requested = {e["path"] for e in self.queue}
            self.log.append("manifest: %d entries, %d to fetch" % (len(self.manifest), len(self.queue)))
        else:
            entry = self.queue.pop(0)
            if entry["fileId"] == fid and self._stage(entry["path"]):
                self.staged[entry["path"]] = data
        if self.queue:
            self._request(self.queue[0]["fileId"])
        else:
            self._finish_sync()

    def _differs(self, entry: dict) -> bool:
        current = self.files.get(entry["path"])
        return entry["crc"] == 0 or current is None or crc32(current) != entry["crc"]

    def _stage(self, path: str) -> bool:
        """Make inbox/<folder of path> with one mkdir, as FUN_80129454 does; False when its parent is missing."""
        folder = path.rsplit("/", 1)[0] if "/" in path else ""
        parent = folder.rsplit("/", 1)[0] if "/" in folder else ""
        if folder and parent and parent not in self.inbox_dirs:
            self.log.append("failed to write file inbox/%s (no folder inbox/%s)" % (path, parent))
            return False
        if folder:
            self.inbox_dirs.add(folder)
        return True

    # post-sync effects ------------------------------------------------------------------------------------
    def _finish_sync(self) -> None:
        for e in self.manifest:                               # FUN_80127a24: copy each staged file in place
            if "fileId" not in e or e.get("operation", "write") not in ("write",):
                continue
            data = self.staged.get(e["path"])
            if data is None or crc32(data) != e["crc"] or len(data) != e["size"]:
                if e["path"] in self.requested:
                    self.log.append("invalid file crc, can't finalize %s: not written" % e["path"])
                continue
            self.files[e["path"]] = data
            self.written.add(e["path"])
            self.log.append("wrote %s (%d bytes)" % (e["path"], len(data)))
        for e in self.manifest:
            if e.get("operation") == "remove":
                self._remove_plug(int(e["plugId"]))
        if "settings/patch.json" in self.written:
            raw = self.files["settings/patch.json"] if self.keep_patch else self.files.pop("settings/patch.json")
            self._import_patch(json.loads(raw))
        if BOOT_PATCH in self.written:
            self._import_patch(json.loads(self.files[BOOT_PATCH]))      # kept until the next start
        for path in sorted(self.written):
            at = path.find("projects/project")
            if at >= 0 and path[at + 16:at + 18].isdigit() and path[at + 18:] == ".opz":
                slot = int(path[at + 16:at + 18]) - 1
                if not 0 <= slot < opz_project.PROJECTS:
                    self.log.append("RAM CORRUPTED: %s reloaded into project slot %d" % (path, slot))
                self.reloaded.append(slot)
        self.syncing = False
        self.sync_count += 1
        self.out.append(frame(0x33, self._cid() + bytes((127,)) + (0).to_bytes(4, "little")))

    def _load(self):
        return (json.loads(self.files["settings/plugs.json"]), json.loads(self.files["settings/slotConfiguration.json"]))

    def _save(self, plugs, slots) -> None:
        plugs["engines"].sort(key=lambda e: e["path"])
        plugs["plugs"].sort(key=lambda p: p["id"])
        self.files["settings/plugs.json"] = json.dumps(plugs).encode()
        self.files["settings/slotConfiguration.json"] = json.dumps(slots).encode()

    def _import_patch(self, doc: dict) -> None:
        plugs, slots = self._load()

        def engine(e):
            plugs["engines"] = [x for x in plugs["engines"] if x["path"] != e["path"]]
            plugs["engines"].append({"memoryslot": e["memoryslot"], "path": e["path"], "version": e["version"]})

        def plug(pl, check_version=True):
            old = next((x for x in plugs["plugs"] if x["id"] == pl["id"]), None)
            if check_version and old is not None and old["version"] >= pl["version"]:
                self.log.append("plug %d skipped (version %d <= %d)" % (pl["id"], pl["version"], old["version"]))
                return False
            plugs["plugs"] = [x for x in plugs["plugs"] if x["id"] != pl["id"]]
            plugs["plugs"].append({"engine": pl["engine"], "flags": pl.get("flags", 0), "id": pl["id"],
                                   "samples": pl.get("samples", ""), "version": pl["version"]})
            self._read_presets(pl["id"], pl.get("presets", []), merge=old is not None)
            return True

        for entry in doc.get("auto_import", []):
            engine(entry["engine"])
            plug(entry["plug"])
            for t in entry.get("tracks", []):
                self._assign_first_free(slots, int(t), entry["plug"]["id"])
        for e in doc.get("engines", []):
            engine(e)
        for pl in doc.get("plugs", []):
            plug(pl, check_version=False)
        self._save(plugs, slots)

    def _read_presets(self, plug_id: int, presets, merge: bool) -> None:
        """readPresetSettings: index 0–13, string name, array values; slot clamped 0–3; 11-character names;
        defaultOctave −1 if missing, clamped −4…4. A new plug starts empty; an existing one merges."""
        store = self.presets.setdefault(plug_id, {})
        if not merge:
            store.clear()
        for p in presets if isinstance(presets, list) else []:
            index = p.get("index")
            if not isinstance(index, int) or isinstance(index, bool) or not 0 <= index <= 13:
                continue
            if not isinstance(p.get("name"), str) or not isinstance(p.get("values"), list):
                continue
            slot = p.get("slot", 0)
            slot = min(max(slot if isinstance(slot, int) else 0, 0), 3)
            octave = p.get("defaultOctave", -1)
            octave = min(max(octave if isinstance(octave, int) else -1, -4), 4)
            values = [(v & 0xFF) if isinstance(v, int) and not isinstance(v, bool) else 0 for v in p["values"][:18]]
            values += [0] * (18 - len(values))
            store[(slot, index)] = {"slot": slot, "index": index, "name": p["name"][:11], "defaultOctave": octave,
                                    "values": values}
        self.log.append("plug %d: %d presets" % (plug_id, len(store)))

    def _assign_first_free(self, slots, track: int, plug_id: int) -> None:
        tr = next((t for t in slots["tracks"] if t["index"] == track), None)
        if tr is None:
            tr = {"index": track, "packs": []}
            slots["tracks"].append(tr)
        if any(p["id"] == plug_id for p in tr["packs"]):
            self.log.append("plug already exist on track")
            return
        used = {p["slot"] for p in tr["packs"]}
        free = next((s for s in range(1, 11) if s not in used), None)
        if free is None:
            self.log.append("No free slots to assign plug to")
            return
        tr["packs"].append({"id": plug_id, "slot": free})
        tr["packs"].sort(key=lambda p: p["slot"])

    def _remove_plug(self, plug_id: int) -> None:
        plugs, slots = self._load()
        pl = next((x for x in plugs["plugs"] if x["id"] == plug_id), None)
        if pl is None:
            return
        plugs["plugs"].remove(pl)
        self.presets.pop(plug_id, None)
        if pl.get("samples"):
            self.files.pop(pl["samples"], None)
        self.log.append("remove user plug %d" % plug_id)
        self._save(plugs, slots)
