"""Live OP-Z file-service session over SysEx (USB or BLE MIDI).

Wraps the vendored `opzsysex` state machines in the session sequence documented and hardware-proven in
ioma8/opz-firmware-notes (refs/op-z-sysex/docs/clean-room-file-transport.md, toolkit/opzpull.py):

    host $00 hello (protocol 6) + $51 advertise  ->  OP-Z $34 (client id, state CRC)
    pull:  host $52 (sync-active 0), $35 request  ->  OP-Z $53 parts (auto-advancing)
    push:  host $52 (sync-active 1)  ->  OP-Z $35 for syncjob.json, then for each file it wants
           host $53 parts  <-  OP-Z $33 progress (1..126 completed parts, 127 done, 255 state CRC)
    close: host $52 (sync-active 0), $55

Both the app-protocol hello and the file-server session time out after ~6 s, so both are refreshed every second.

`StateSession` is the other half of the app protocol (research/presets.md, patterns.md §5): the `$00` hello
opens a StateSync session and the OP-Z answers every hello with `$01`. On firmware 1.2.45 the first hello of a session
brings `$07` (play state), `$0C` (global block) and part 0 of the active project's pattern bank, not the `$0E` per
track the notes repo describes [proven 2026-10-26]. The OP-Z sends each further part only after the client's `$0B`
ack of the part before; `$08` makes it start the bank over (TE's StateSync, app 0x29bdc / 0x2a268 / 0x2b7d4; both
are read-only). The session inflates the bank and takes each track's current sound (its restore point) from the
active pattern's sound block. A `$0E` the OP-Z sends (as the notes describe) is taken too. A host `$0E` (track + 18
parameter bytes) sets that track's sound in the active pattern (device-proven in the notes repo). The session ends
~5 s after the last hello.
"""

from __future__ import annotations

import os
import sys
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "third_party", "opzsysex"))
from opzsysex.fileprotocol import UploadState, download_file_id  # noqa: E402
from opzsysex.sysex import ProtocolError, decode_frame, frame, state_sync_client_hello  # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from opz_project import BANK_SIZE, PATTERN_SIZE, SOUND_AT  # noqa: E402

SERVER_ID = 0x4242
STATUS_DONE = 127
STATUS_STATE_CRC = 255
SOUND = 0x0E
SESSION_ACK = 0x01
STATE_EXPIRY = 6.5          # notes repo: 6.25 s without hellos reliably ends a StateSync session
PLAY_STATE = 0x07
BANK_REQUEST, BANK_PART, BANK_FINAL, BANK_ACK = 0x08, 0x09, 0x0A, 0x0B
BANK_CHUNK = 178            # compressed bytes per part; a non-final part carries exactly this many
PART_WAIT = 1.0             # a bank transfer in progress extends open()'s wait by this much per part
BANK_REQUESTS = 2           # `$08` re-requests per session (broken or stalled transfers)


class SessionError(RuntimeError):
    pass


def check_upload_paths(files: list[tuple[str, bytes]]) -> None:
    """Refuse a path two folders deep before anything is sent. The OP-Z stages each download in inbox/<path> and makes
    only its last folder (one mkdir, FUN_80129454 -> FUN_801288b8), so inbox/a/b/ fails unless inbox/a/ exists: the
    parts are taken, the sync reports complete, and finalising copies nothing (FUN_80127a24 "invalid file crc").
    Every path our tools write is one folder deep (engines/, settings/, projects/, samplepacks/) [code, device; Q-49]."""
    for path, _ in files:
        if path.count("/") > 1:
            raise SessionError("%s is two folders deep: the OP-Z's file service stages downloads in inbox/ with a "
                               "single mkdir and would drop it without an error (Q-49); nothing sent" % path)


def sound_frame(track: int, values) -> bytes:
    """`$0E`: one track's 18 sound-parameter bytes for the active pattern."""
    values = list(values)
    if not 0 <= track <= 15 or len(values) != 18 or not all(isinstance(v, int) and 0 <= v <= 255 for v in values):
        raise ValueError("a sound is a track 0..15 and 18 values in 0..255")
    return frame(SOUND, bytes([track] + values))


def decode_sound(payload: bytes) -> tuple[int, list[int]] | None:
    """(track, values) from a decoded `$0E` payload, or None if it is not exactly track + 18 bytes."""
    if len(payload) != 19 or payload[0] > 15:
        return None
    return payload[0], list(payload[1:])


def bank_ack_frame(transfer: int, index: int) -> bytes:
    """`$0B`: the client has bank part `index` of `transfer` (TE's receiver sends `09 00 00 00`, transfer, index)."""
    return frame(BANK_ACK, b"\x09\x00\x00\x00" + transfer.to_bytes(2, "little") + index.to_bytes(2, "little"))


def bank_request_frame() -> bytes:
    """`$08`: send the active bank again from part 0 (the OP-Z ignores the payload; TE's receiver sends FF FF FF FF)."""
    return frame(BANK_REQUEST, b"\xff\xff\xff\xff")


def bank_sounds(bank: bytes, address: int) -> dict[int, list[int]]:
    """Every track's sound in the active pattern (address & 15) of an inflated bank."""
    at = (address & 0x0F) * PATTERN_SIZE + SOUND_AT
    return {t: list(bank[at + 18 * t:at + 18 * t + 18]) for t in range(16)}


class MidoTransport:
    """Real device: the vendored opzsysex mido Device, with receive() returning None on timeout."""

    def __init__(self, match: str = "OP-Z"):
        from opzsysex.device import Device
        self._device = Device(match)

    def __enter__(self):
        self._device.__enter__()
        return self

    def __exit__(self, *exc):
        self._device.__exit__(*exc)

    def send(self, sysex: bytes) -> None:
        self._device.send(sysex)

    def send_short(self, data: bytes) -> None:
        """A channel message (note on/off for the device test runner)."""
        self._device.output.send(self._device._mido.Message.from_bytes(list(data)))

    def present(self) -> bool:
        """Both MIDI ports are still listed (they vanish when the OP-Z restarts or crashes)."""
        try:
            mido = self._device._mido
            return (self._device.input_name in mido.get_input_names()
                    and self._device.output_name in mido.get_output_names())
        except Exception:
            return False

    def receive(self, timeout: float) -> bytes | None:
        try:
            return self._device.receive(timeout)
        except Exception:
            return None


class FileSession:
    def __init__(self, transport, server_id: int = SERVER_ID, log=None, clock=time.monotonic):
        self.t = transport
        self.server_id = server_id
        self.log = log or (lambda message: None)
        self.clock = clock
        self.client_id: int | None = None
        self.state_crc: int | None = None
        self._last_keepalive = -1e9
        self._active = 0

    # -- framing helpers ------------------------------------------------------------------------------
    def _sid(self) -> bytes:
        return self.server_id.to_bytes(2, "little")

    def _recv(self, timeout: float):
        raw = self.t.receive(timeout)
        if raw is None:
            return None
        try:
            f = decode_frame(raw)
        except ProtocolError:
            return None
        return f.message_id, f.payload

    def _keepalive(self, force: bool = False) -> None:
        now = self.clock()
        if force or now - self._last_keepalive >= 1.0:
            self.t.send(state_sync_client_hello(self.server_id))
            self.t.send(frame(0x52, self._sid() + bytes((self._active,))))
            self._last_keepalive = now

    # -- session ----------------------------------------------------------------------------------------
    def open(self, timeout: float = 30.0) -> tuple[int, int]:
        self.t.send(state_sync_client_hello(self.server_id))
        self.t.send(frame(0x51, self._sid()))
        start = last_advert = self.clock()
        while self.clock() - start < timeout:
            if self.clock() - last_advert > 1.5:
                self.t.send(frame(0x51, self._sid()))
                last_advert = self.clock()
            msg = self._recv(0.25)
            if msg and msg[0] == 0x34 and len(msg[1]) >= 6:
                self.client_id = int.from_bytes(msg[1][:2], "little")
                self.state_crc = int.from_bytes(msg[1][2:6], "little")
                self.log("session open: client %d, state CRC %08x" % (self.client_id, self.state_crc))
                self._active = 0
                self._keepalive(force=True)
                return self.client_id, self.state_crc
        raise SessionError("no $34 reply from the OP-Z within %.0f s (is it in normal mode, and the official app closed?)" % timeout)

    def close(self) -> None:
        self._active = 0
        self.t.send(frame(0x52, self._sid() + b"\x00"))
        self.t.send(frame(0x55, self._sid()))
        self.log("session closed")

    def pull(self, path: str, part_timeout: float = 2.5, cap: int = 12_000_000, file_id: int | None = None) -> bytes:
        """Download one served file. Raises SessionError if it is not served or incomplete. `file_id` overrides the
        id derived from the path (projects are served as ids 3-12, opz_project.project_file_id)."""
        if self.client_id is None:
            raise SessionError("session not open")
        self._active = 0
        fid = download_file_id(path) if file_id is None else file_id
        got: dict[int, bytes] = {}
        want, last_part, retries = 0, None, 0
        self._keepalive(force=True)
        while True:
            self.t.send(frame(0x35, self._sid() + fid.to_bytes(4, "little") + want.to_bytes(2, "little")))
            deadline = self.clock() + part_timeout
            progressed = False
            last_rx = None
            while self.clock() < deadline:
                self._keepalive()
                if progressed and last_rx is not None and self.clock() - last_rx > 0.3:
                    break                       # stream paused: ask again from the cursor
                msg = self._recv(0.05)
                if not msg or msg[0] != 0x53 or len(msg[1]) < 9:
                    continue
                p = msg[1]
                if int.from_bytes(p[:2], "little") != self.client_id or int.from_bytes(p[2:6], "little") != fid:
                    continue
                part = int.from_bytes(p[6:8], "little")
                if part < want:
                    continue
                try:
                    got[part] = zlib.decompress(p[9:])
                except zlib.error:
                    continue
                if p[8]:
                    last_part = part
                last_rx = self.clock()
                if part == want:
                    progressed = True
                    retries = 0
                    while want in got:
                        want += 1
                    if sum(len(v) for v in got.values()) > cap:
                        raise SessionError("%s exceeds %d bytes" % (path, cap))
                if last_part is not None and want > last_part:
                    data = b"".join(got[i] for i in range(last_part + 1))
                    self.log("pulled %s: %d bytes in %d parts" % (path, len(data), last_part + 1))
                    return data
            if not got:
                raise SessionError("%s is not served by the file service" % path)
            if not progressed:
                retries += 1
                if retries >= 3:
                    raise SessionError("%s incomplete: missing part %d" % (path, want))

    def push(self, files: list[tuple[str, bytes]], remove_plug_ids: list[int] | None = None,
             idle_timeout: float = 15.0, grace: float = 2.0) -> dict:
        """Serve a sync job: the OP-Z pulls syncjob.json, then requests each listed file unless the file already at
        /yaffs2/user/<path> has the listed CRC (FUN_8012bb00: crc32 of the file on flash, any path, no CRC 0).

        Returns {"requested": [...paths], "skipped": [...paths the OP-Z did not request]}. A skip proves the OP-Z
        holds exactly those bytes; a request proves nothing about the earlier copy. Refuses paths two folders deep,
        which the OP-Z drops without an error (Q-49).
        """
        if self.client_id is None:
            raise SessionError("session not open")
        check_upload_paths(files)
        state = UploadState(files, remove_plug_ids)
        paths = {d.file_id: d.path for d in state.descriptors}
        requested: set[int] = set()
        self._active = 1
        self._keepalive(force=True)
        last_activity = self.clock()
        done_at = None
        while True:
            now = self.clock()
            out = state.next_frame(self.server_id, now=now)
            if out is not None:
                self.t.send(out)
                last_activity = now
            msg = self._recv(0.002)
            if msg:
                mid, p = msg
                if mid == 0x35:
                    state.accept_request(p, self.client_id)
                    if state.active_file_id in paths:
                        requested.add(state.active_file_id)
                    last_activity = now
                    done_at = None
                elif mid == 0x33 and len(p) >= 7 and int.from_bytes(p[:2], "little") == self.client_id:
                    status, value = p[2], int.from_bytes(p[3:7], "little")
                    if 1 <= status <= 126:
                        state.observe_completed_parts(value)
                        last_activity = now
                    elif status == STATUS_DONE and done_at is None:
                        done_at = now
                        self._active = 0            # keep-alives must not start a second sync
                        self.log("OP-Z reports sync complete")
            if done_at is not None and state.active_file_id is None:
                if state.sent_all_files or now - done_at >= grace:
                    break
            if now - last_activity > idle_timeout:
                self._active = 0
                self._keepalive(force=True)
                raise SessionError("sync stalled for %.0f s" % idle_timeout)
            self._keepalive()
        self._active = 0
        self._keepalive(force=True)
        skipped = [paths[i] for i in paths if i not in requested]
        return {"requested": [paths[i] for i in paths if i in requested], "skipped": skipped}


class StateSession:
    """A StateSync session for `$0E` sound writes: a hello every second, each track's current sound (the restore points)
    from the OP-Z's opening burst.

    Receives always use 0.25 s timeouts (fixed values keep the shared test vectors exact).
    """

    def __init__(self, transport, client_id: int = SERVER_ID, log=None, clock=time.monotonic):
        self.t = transport
        self.client_id = client_id
        self.log = log or (lambda message: None)
        self.clock = clock
        self.sounds: dict[int, list[int]] = {}      # per track: the sound at open (from the bank), or a later `$0E`
        self.acked = False
        self.last_ack: float | None = None          # clock time of the latest `$01`
        self.seen: dict[int, int] = {}              # message id -> count of every frame received (research)
        self.bank: bytes | None = None              # the first complete bank (the active project, at open)
        self.bank_address: int | None = None        # its address: project × 16 + active pattern
        self.play_address: int | None = None        # `$07` byte 17, the latest
        self._rx: dict | None = None                # the bank transfer in progress
        self._requests = 0
        self._until = -1e9
        self._last_hello = -1e9

    def _hello(self, force: bool = False) -> None:
        now = self.clock()
        if force or now - self._last_hello >= 1.0:
            self.t.send(state_sync_client_hello(self.client_id))
            self._last_hello = now

    def _request_bank(self, why: str) -> None:
        self._rx = None
        if self._requests < BANK_REQUESTS:
            self._requests += 1
            self.log("bank %s: asking for it again" % why)
            self.t.send(bank_request_frame())

    def _bank_part(self, final: bool, payload: bytes) -> None:
        """One `$09`/`$0A`: checked as TE's receiver does (0x2b67c), acked, and on the last part inflated."""
        if len(payload) < 7 or len(payload) - 6 > BANK_CHUNK or (not final and len(payload) - 6 != BANK_CHUNK):
            return self._request_bank("part malformed")
        address = int.from_bytes(payload[0:2], "little")
        transfer = int.from_bytes(payload[2:4], "little")
        index = int.from_bytes(payload[4:6], "little")
        if index == 0:
            self._rx = {"address": address, "transfer": transfer, "chunks": []}
        elif self._rx is None or self._rx["transfer"] != transfer or len(self._rx["chunks"]) != index:
            return self._request_bank("part %d out of order" % index)
        rx = self._rx
        rx["chunks"].append(payload[6:])
        self._until = max(self._until, self.clock() + PART_WAIT)
        if not final:
            self.t.send(bank_ack_frame(transfer, index))
            return
        self._rx = None
        z = b"".join(rx["chunks"])
        inflater = zlib.decompressobj()
        try:
            bank = inflater.decompress(z, BANK_SIZE + 1)
        except zlib.error:
            bank = b""
        if len(bank) != BANK_SIZE:
            return self._request_bank("did not inflate to %d bytes" % BANK_SIZE)
        if self.bank is not None:
            return                                  # a later bank (after our own writes) is no restore point
        self.bank, self.bank_address = bank, rx["address"]
        self.log("bank: project %d, pattern %d (%d parts, %d bytes)"
                 % ((rx["address"] >> 4) + 1, (rx["address"] & 0x0F) + 1, index + 1, len(z)))
        for t, values in bank_sounds(bank, rx["address"]).items():
            self.sounds.setdefault(t, values)

    def _take(self, timeout: float = 0.25) -> tuple[int, list[int]] | None:
        raw = self.t.receive(timeout)
        if raw is None:
            return None
        try:
            f = decode_frame(raw)
        except ProtocolError:
            return None
        self.seen[f.message_id] = self.seen.get(f.message_id, 0) + 1
        if f.message_id == SESSION_ACK:
            self.acked = True
            self.last_ack = self.clock()
        elif f.message_id == SOUND:
            sound = decode_sound(f.payload)
            if sound is not None:
                self.sounds[sound[0]] = sound[1]
                return sound
        elif f.message_id in (BANK_PART, BANK_FINAL):
            self._bank_part(f.message_id == BANK_FINAL, f.payload)
        elif f.message_id == PLAY_STATE and len(f.payload) == 20:
            self.play_address = f.payload[17]
        return None

    def _ready(self, tracks) -> bool:
        return self.acked and all(t in self.sounds for t in tracks)

    def _wait(self, tracks, timeout: float) -> None:
        """Until ready or `timeout` has passed; a bank transfer still arriving extends the wait part by part."""
        self._until = self.clock() + timeout
        while self.clock() < self._until and not self._ready(tracks):
            self._hello()
            self._take()

    def open(self, tracks=(), timeout: float = 3.0) -> None:
        """Start the session; wait for `$01` and each track's current sound in `tracks` (the restore points): from the
        bank, which the OP-Z sends part by part as they are acked, or from a `$0E`. A session still live from an
        earlier client sends no new burst: wait for it to expire, then start once more."""
        for attempt in (1, 2):
            self._hello(force=True)
            self._wait(tracks, timeout)
            if not self._ready(tracks) and self._rx is not None:
                self._request_bank("transfer stalled")
                self._wait(tracks, timeout)
            if self._ready(tracks):
                if (self.bank_address is not None and self.play_address is not None
                        and self.play_address != self.bank_address):
                    raise SessionError("the active pattern (%d) is not the bank's (%d): the restore points are not "
                                       "sure; nothing written" % (self.play_address, self.bank_address))
                self.log("state session open")
                return
            if not self.acked:
                raise SessionError("no $01 reply to the hello within %.0f s (is the OP-Z in normal mode, and the "
                                   "official app closed?)" % timeout)
            if attempt == 1:
                if BANK_PART in self.seen or BANK_FINAL in self.seen:
                    self.log("the bank did not arrive whole; waiting %.1f s for the session to expire" % STATE_EXPIRY)
                else:
                    self.log("no sound burst (a state session is still live); waiting %.1f s for it to expire"
                             % STATE_EXPIRY)
                end = self.clock() + STATE_EXPIRY
                while self.clock() < end:
                    self._take()
                self.acked = False
        missing = [t for t in tracks if t not in self.sounds]
        raise SessionError("the OP-Z sent no sound for track(s) %s; nothing written" % missing)

    def pump(self, seconds: float) -> list[tuple[int, list[int]]]:
        """Keep the session alive for `seconds`; returns the `$0E` records the OP-Z sent meanwhile, in order."""
        seen = []
        end = self.clock() + seconds
        while self.clock() < end:
            self._hello()
            sound = self._take()
            if sound is not None:
                seen.append(sound)
        return seen

    def tick(self, timeout: float = 0.031) -> tuple[int, list[int]] | None:
        """One short step for live editing (a hello when due, one receive): an editor sends `$0E` between ticks, at
        most about every 31 ms like TE's app does. Not used by the recorded flows."""
        self._hello()
        return self._take(timeout)

    def set(self, track: int, values) -> None:
        """Send `$0E`: the track's sound in the active pattern changes at once (no reply)."""
        self.t.send(sound_frame(track, values))
        self._hello()

    def close(self) -> None:
        self.log("state session closed (the OP-Z ends it ~5 s after the last hello)")
