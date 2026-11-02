"""Audio side of the device test runner (tools/opz_bench.py): capture the OP-Z's USB audio, read and write WAVs, and
measure segments of a recording. Python standard library only (no numpy): every per-sample loop runs over one
segment at a time, and the correlation sums use `map` so a three-minute run analyses in seconds.

Capture: ffmpeg's avfoundation input (macOS), audio device found by name, raw s16le on a pipe, so the runner sees
every sample as it arrives (live crash checks) and knows when each chunk came in. `start_time` is the clock time of
sample 0, estimated from the least-delayed chunk (arrival time minus the samples received so far); the runner then
refines the offset from the first note's onset.

Measurements (`analyse_segment`): RMS and peak (dBFS), clipped samples, silence where notes should sound, dropouts
(the level falling from above −40 dBFS to below −70 dBFS within 5 ms while a note is held; a voice that ends
after decaying, as the host frees it at its envelope's end, is quieter than that: a voice cut, a CPU overload or a crash),
clicks (a second-difference spike four times above anything in the preceding 45 ms and within 30 dB of the
segment's peak, i.e. a discontinuity that a
periodic waveform does not explain; those within 15 ms of a note-on are counted apart as the note's attack, which
attack 0 makes a step on purpose), the fundamental of held single notes (YIN, searched around the expected pitch)
and release tails (time to −60 dBFS).
"""

from __future__ import annotations

import math
import os
import re
import shutil
import subprocess
import threading
import time
import wave
from array import array

RATE = 44100
CHANNELS = 2
FULL = 32768.0
CLIP = 32700                 # |sample| at or above this counts as clipped
SILENT_DB = -60.0            # a note window quieter than this "did not sound"
DROP_FLOOR_DB = -70.0
DROP_STEP_DB = 30.0
FRAME = 0.005                # dropout frames
CLICK_BLOCK = 0.001
CLICK_LOOKBACK = 45          # blocks (ms) a click must stand out against: longer than C1's period (30.6 ms)
CLICK_RATIO = 4.0
CLICK_FLOOR = 0.004          # second-difference spikes smaller than this (−48 dBFS) are never clicks
CLICK_RELATIVE = 0.03        # nor those more than 30 dB under the segment's peak (an 8-bit voice ending)
ONSET_GUARD = 0.015          # clicks this close to a note-on are the note's attack
TAIL_DB = -60.0


class CaptureError(RuntimeError):
    pass


def db(x: float) -> float:
    return 20 * math.log10(x) if x > 1e-9 else -180.0


# ---------------------------------------------------------------------------------------------------------------
# WAV I/O (16-bit, interleaved stereo in an array('h'))
# ---------------------------------------------------------------------------------------------------------------

def write_wav(path: str, samples: array, rate: int = RATE, channels: int = CHANNELS) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(samples.tobytes())


def read_wav(path: str) -> tuple[array, int]:
    """(interleaved stereo int16, rate). Mono files are duplicated to two channels; only 16-bit PCM is read."""
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise CaptureError("%s: only 16-bit WAVs are read (got %d-bit)" % (path, 8 * w.getsampwidth()))
        rate, ch = w.getframerate(), w.getnchannels()
        data = array("h")
        data.frombytes(w.readframes(w.getnframes()))
    if ch == 1:
        st = array("h", bytes(4 * len(data)))
        st[0::2] = data
        st[1::2] = data
        data = st
    elif ch != 2:
        data = array("h", [v for i, v in enumerate(data) if i % ch < 2])
    return data, rate


def mono(samples: array, start: int, end: int) -> list[float]:
    """Frames [start, end) of interleaved stereo as mono floats in −1..1."""
    start, end = max(0, start), min(len(samples) // 2, end)
    if end <= start:
        return []
    left, right = samples[2 * start:2 * end:2], samples[2 * start + 1:2 * end:2]
    return [(a + b) / (2 * FULL) for a, b in zip(left, right)]


# ---------------------------------------------------------------------------------------------------------------
# Capture
# ---------------------------------------------------------------------------------------------------------------

def find_ffmpeg() -> str | None:
    for p in (shutil.which("ffmpeg"), "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg"):
        if p and os.path.exists(p):
            return p
    return None


def parse_avfoundation_devices(text: str) -> list[tuple[int, str]]:
    """The audio devices in `ffmpeg -f avfoundation -list_devices true -i ""` output: [(index, name)]."""
    found, audio = [], False
    for line in text.splitlines():
        if "audio devices:" in line:
            audio = True
            continue
        if "video devices:" in line:
            audio = False
            continue
        m = re.search(r"\]\s*\[(\d+)\]\s*(.+?)\s*$", line)
        if audio and m:
            found.append((int(m.group(1)), m.group(2)))
    return found


class FfmpegCapture:
    """Record one avfoundation audio device to memory through ffmpeg (s16le, 44.1 kHz stereo on stdout)."""

    PERMISSION_HINT = ("macOS may be blocking the microphone for this app: System Settings → Privacy & Security → "
                       "Microphone, allow the terminal (or Claude), then run again. The first capture shows the prompt.")

    def __init__(self, name: str = "OP-Z", ffmpeg: str | None = None, clock=time.monotonic,
                 run=subprocess.run, popen=subprocess.Popen):
        self.name = name
        self.ffmpeg = ffmpeg or find_ffmpeg()
        self.clock = clock
        self._run, self._popen = run, popen
        self.proc = None
        self.data = bytearray()
        self.start_time: float | None = None
        self.last_data: float | None = None
        self.device: tuple[int, str] | None = None
        self.stderr = b""
        self._lock = threading.Lock()
        self._thread = None
        self._eof = False

    def find_device(self) -> tuple[int, str]:
        if not self.ffmpeg:
            raise CaptureError("ffmpeg not found (brew install ffmpeg; expected /opt/homebrew/bin/ffmpeg)")
        r = self._run([self.ffmpeg, "-hide_banner", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
                      capture_output=True, timeout=20)
        text = (r.stderr or b"").decode("utf-8", "replace") + (r.stdout or b"").decode("utf-8", "replace")
        devices = parse_avfoundation_devices(text)
        matches = [d for d in devices if self.name.lower() in d[1].lower()]
        if not matches:
            raise CaptureError("no audio input device named %r (found: %s). Is the OP-Z connected by USB and on?"
                               % (self.name, ", ".join(n for _, n in devices) or "none"))
        if len(matches) > 1:
            raise CaptureError("more than one audio device matches %r: %s; pass a longer --audio-device"
                               % (self.name, ", ".join(n for _, n in matches)))
        self.device = matches[0]
        return self.device

    def start(self, timeout: float = 5.0) -> None:
        """Start ffmpeg and wait until audio flows; raises CaptureError (with ffmpeg's message) otherwise."""
        index, _ = self.device or self.find_device()
        cmd = [self.ffmpeg, "-hide_banner", "-nostdin", "-nostats", "-loglevel", "error", "-f", "avfoundation",
               "-i", ":%d" % index, "-ac", str(CHANNELS), "-ar", str(RATE), "-f", "s16le", "-acodec", "pcm_s16le", "-"]
        self.proc = self._popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()
        threading.Thread(target=self._err_reader, daemon=True).start()
        end = self.clock() + timeout
        while self.clock() < end:
            if self.last_data is not None:
                return
            if self._eof or self.proc.poll() is not None:
                break
            time.sleep(0.02)
        self.stop()
        msg = self.stderr.decode("utf-8", "replace").strip().splitlines()
        raise CaptureError("no audio from %r within %.0f s%s. %s" % (
            self.device[1], timeout, (": " + msg[-1]) if msg else "", self.PERMISSION_HINT))

    def _reader(self) -> None:
        out = self.proc.stdout
        while True:
            chunk = out.read1(8192) if hasattr(out, "read1") else out.read(8192)
            if not chunk:
                self._eof = True
                return
            now = self.clock()
            with self._lock:
                self.data.extend(chunk)
                frames = len(self.data) // (2 * CHANNELS)
                est = now - frames / RATE
                if self.start_time is None or est < self.start_time:
                    self.start_time = est
                self.last_data = now

    def _err_reader(self) -> None:
        try:
            self.stderr = self.proc.stderr.read() or b""
        except (OSError, ValueError):
            pass

    def alive(self) -> bool:
        return self.proc is not None and not self._eof and self.proc.poll() is None

    def data_age(self) -> float:
        return 0.0 if self.last_data is None else self.clock() - self.last_data

    def samples(self) -> array:
        with self._lock:
            raw = bytes(self.data[:len(self.data) // 4 * 4])
        a = array("h")
        a.frombytes(raw)
        return a

    def level_db(self, t0: float, t1: float) -> float:
        """RMS (dBFS) between two clock times of what has arrived so far."""
        if self.start_time is None:
            return -180.0
        with self._lock:
            n = len(self.data) // 4
            s, e = max(0, int((t0 - self.start_time) * RATE)), min(n, int((t1 - self.start_time) * RATE))
            if e <= s:
                return -180.0
            a = array("h")
            a.frombytes(bytes(self.data[4 * s:4 * e]))
        return rms_db(a)

    def stop(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if self._thread is not None:
            self._thread.join(timeout=2)


class FakeCapture:
    """The capture API over tools/fake_opz.FakeOpz's synthesised output (offline tests). `latency` is the true delay
    of sample 0 after start(); `skew` is the error of the start-time estimate, which the onset alignment must remove."""

    def __init__(self, opz, latency: float = 0.012, skew: float = 0.0):
        self.opz = opz
        self.latency, self.skew = latency, skew
        self.start_time = None
        self._t0 = None
        self._stopped_at = None
        self.device = (0, "fake OP-Z")

    def find_device(self):
        return self.device

    def start(self, timeout: float = 5.0) -> None:
        self._t0 = self.opz.now + self.latency
        self.start_time = self._t0 + self.skew

    def _end(self) -> float:
        end = self._stopped_at if self._stopped_at is not None else self.opz.now
        if self.opz.crash_at is not None:
            end = min(end, self.opz.crash_at)
        return end

    def alive(self) -> bool:
        return not self.opz.crashed()

    def data_age(self) -> float:
        return 0.0 if not self.opz.crashed() else self.opz.now - self.opz.crash_at

    def samples(self) -> array:
        return self.opz.render(self._t0, self._end())

    def level_db(self, t0: float, t1: float) -> float:
        t0, t1 = max(t0 - self.skew, self._t0), min(t1 - self.skew, self._end())
        return rms_db(self.opz.render(t0, t1, rate=4000)) if t1 > t0 else -180.0     # a level, not audio

    def stop(self) -> None:
        if self._stopped_at is None:
            self._stopped_at = self.opz.now


# ---------------------------------------------------------------------------------------------------------------
# Measurements
# ---------------------------------------------------------------------------------------------------------------

def rms_db(samples) -> float:
    if not len(samples):
        return -180.0
    if isinstance(samples, array):
        return db(math.sqrt(sum(v * v for v in samples) / len(samples)) / FULL)
    return db(math.sqrt(sum(x * x for x in samples) / len(samples)))


def frame_levels(x: list[float], rate: int, frame: float = FRAME) -> list[float]:
    n = max(1, int(rate * frame))
    out = []
    for i in range(0, len(x) - n + 1, n):
        seg = x[i:i + n]
        out.append(db(math.sqrt(sum(map(float.__mul__, seg, seg)) / n)))
    return out


def find_onset(x: list[float], rate: int, floor_db: float = -50.0) -> float | None:
    """Seconds into `x` of the first 2 ms frame above `floor_db`, or None."""
    for i, level in enumerate(frame_levels(x, rate, 0.002)):
        if level > floor_db:
            return i * 0.002
    return None


def dropouts(x: list[float], rate: int) -> list[float]:
    """Seconds into `x` where the level falls from DROP_STEP_DB above DROP_FLOOR_DB to below it within one frame."""
    levels = frame_levels(x, rate)
    return [i * FRAME for i in range(1, len(levels))
            if levels[i] < DROP_FLOOR_DB and levels[i - 1] >= DROP_FLOOR_DB + DROP_STEP_DB]


def clicks(x: list[float], rate: int) -> list[float]:
    """Seconds into `x` of discontinuities: 1 ms blocks whose largest second difference is CLICK_RATIO times the
    largest in the CLICK_LOOKBACK blocks before (a periodic waveform's corners recur and so never count)."""
    if len(x) < 3:
        return []
    d2 = [abs(a - b - b + c) for a, b, c in zip(x, x[1:], x[2:])]
    n = max(1, int(rate * CLICK_BLOCK))
    peaks = [max(d2[i:i + n]) for i in range(0, len(d2), n)]
    floor = max(CLICK_FLOOR, CLICK_RELATIVE * max(map(abs, x)))
    found, last = [], -10
    for b in range(CLICK_LOOKBACK, len(peaks)):
        before = max(peaks[b - CLICK_LOOKBACK:b])
        if peaks[b] > floor and peaks[b] > CLICK_RATIO * before:
            if b - last > 5:
                found.append(round(b * n / rate, 4))
            last = b
    return found


def pitch(x: list[float], rate: int, expected_hz: float) -> tuple[float, float] | None:
    """(fundamental Hz, aperiodicity 0..1) of `x` by YIN, searching periods within ±1.2 octaves of `expected_hz`."""
    period = rate / expected_hz
    decim = max(1, int(period // 64))
    if decim > 1:
        x = [sum(x[i:i + decim]) / decim for i in range(0, len(x) - decim + 1, decim)]
    r = rate / decim
    p = r / expected_hz
    tau_min, tau_max = max(2, int(p / 2.3)), int(p * 2.3) + 2
    w = min(max(1024, 3 * tau_max), len(x) - tau_max - 1)
    if w < 2 * tau_max:
        return None
    head = x[:w]
    e0 = sum(map(float.__mul__, head, head))
    sq = [0.0]
    for v in x[:w + tau_max + 1]:
        sq.append(sq[-1] + v * v)
    d = [0.0] * (tau_max + 2)
    for tau in range(1, tau_max + 2):
        cross = sum(map(float.__mul__, head, x[tau:tau + w]))
        d[tau] = e0 + (sq[tau + w] - sq[tau]) - 2 * cross
    if e0 < 1e-9:
        return None
    cmnd, running = [1.0] * (tau_max + 2), 0.0
    for tau in range(1, tau_max + 2):
        running += d[tau]
        cmnd[tau] = d[tau] * tau / running if running > 0 else 1.0
    best = None
    for tau in range(tau_min, tau_max + 1):
        if cmnd[tau] < 0.15 and cmnd[tau] <= cmnd[tau - 1] and cmnd[tau] <= cmnd[tau + 1]:
            best = tau
            break
    if best is None:
        best = min(range(tau_min, tau_max + 1), key=lambda t: cmnd[t])
    a, b, c = cmnd[best - 1], cmnd[best], cmnd[best + 1]
    shift = 0.5 * (a - c) / (a - 2 * b + c) if a - 2 * b + c > 0 else 0.0
    return r / (best + max(-0.5, min(0.5, shift))), max(0.0, min(1.0, b))


def note_hz(note: int) -> float:
    return 440.0 * 2 ** ((note - 69) / 12)


def tail_seconds(x: list[float], rate: int, from_s: float) -> float | None:
    """Seconds after `from_s` until the 5 ms level stays below TAIL_DB, or None if it never does."""
    levels = frame_levels(x, rate)
    start = int(from_s / FRAME)
    loud = [i for i in range(start, len(levels)) if levels[i] >= TAIL_DB]
    if not loud:
        return 0.0
    last = loud[-1]
    return None if last >= len(levels) - 2 else (last + 1) * FRAME - from_s


def fold_cents(c: float) -> float:
    """Cents to the nearest octave of the expected note (−600 … 600)."""
    return c - 1200 * round(c / 1200)


def analyse_segment(samples: array, rate: int, seg: dict, offset: float) -> dict:
    """Measure one plan segment in a recording. `offset` = recording seconds at plan time 0.

    seg: {"start", "end", "notes": [[on, off, track, note, vel], ...], "check": {...}} (plan times, seconds).
    Returns the measurements and a status (OK/WARN/FAIL) with reasons.
    """
    chk = seg.get("check", {})
    s, e = int((seg["start"] + offset) * rate), int((seg["end"] + offset) * rate)
    total = len(samples) // 2
    res = {"start": seg["start"], "end": seg["end"], "reasons": [], "status": "OK"}
    if s >= total or e <= s:
        res.update(status="FAIL", missing=True, reasons=["not recorded (the run stopped before this segment)"])
        return res
    partial = e > total
    e = min(e, total)
    inter = samples[2 * s:2 * e]
    x = mono(samples, s, e)
    res["rms_db"] = round(rms_db(x), 1)
    peak = max(map(abs, inter)) if len(inter) else 0
    res["peak_db"] = round(db(peak / FULL), 1)
    res["clipped"] = sum(1 for v in inter if v >= CLIP or v <= -CLIP)
    if partial:
        res["reasons"].append("cut short: the recording ends inside this segment")
    notes = seg.get("notes", [])
    rel = lambda t: t - seg["start"]                                         # noqa: E731

    if chk.get("silence"):
        res["noise_db"] = res["rms_db"]
        if res["rms_db"] > -60:
            res["reasons"].append("WARN: not silent (%.1f dBFS): is the sequencer playing?" % res["rms_db"])
    if notes and chk.get("sound", True):
        windows = [x[int(rel(on) * rate):int(rel(off) * rate)] for on, off, *_ in notes]
        joined = [v for w in windows for v in w]
        level = rms_db(joined) if joined else -180.0
        res["note_db"] = round(level, 1)
        if level < SILENT_DB:
            res["reasons"].append("FAIL: silent while notes play (%.1f dBFS)" % level)
    if notes and chk.get("dropouts", True):
        found = []
        for on, off, *_ in notes:
            a, b = rel(on) + 0.03, rel(off) - 0.01
            if b - a > 0.02:
                found += [round(a + t, 3) for t in dropouts(x[int(a * rate):int(b * rate)], rate)]
        res["dropouts"] = sorted(set(found))
        if found:
            res["reasons"].append("FAIL: %d dropout(s) while notes were held, at %s s" % (
                len(found), ", ".join("%.2f" % t for t in res["dropouts"][:5])))
    if chk.get("clicks", True):
        guard = [rel(on) for on, *_ in notes]
        found = clicks(x, rate)
        attack = [t for t in found if any(abs(t - g) <= ONSET_GUARD for g in guard)]
        other = [t for t in found if t not in attack]
        res["clicks"] = [round(t, 3) for t in other]
        res["onset_clicks"] = len(attack)
        if other:
            res["reasons"].append("WARN: %d click(s) at %s s" % (len(other), ", ".join("%.2f" % t for t in other[:5])))
    if chk.get("pitch") and notes:
        cents = []
        for on, off, _track, note, _vel in notes:
            if sum(1 for o in notes if o[0] < off and o[1] > on) > 1 or off - on < 0.3:
                continue                                                     # chords and short notes: no pitch
            a = rel(on) + min(0.15, (off - on) * 0.4)
            got = pitch(x[int(a * rate):int((a + 0.25) * rate)], rate, note_hz(note))
            if got and got[1] < 0.35:
                cents.append(round(1200 * math.log2(got[0] / note_hz(note)), 1))
        res["cents"] = cents
        if not cents:
            res["reasons"].append("WARN: no clear pitch")
    if chk.get("tail") and notes:
        last_off = max(off for _, off, *_ in notes)
        t = tail_seconds(x, rate, rel(last_off))
        res["tail_s"] = None if t is None else round(t, 2)
        if t is None:
            res["reasons"].append("WARN: still sounding at the end of the segment (tail > %.1f s)"
                                  % (seg["end"] - last_off))
    if res["clipped"]:
        res["reasons"].append("WARN: %d clipped samples" % res["clipped"])
    if any(r.startswith("FAIL") for r in res["reasons"]):
        res["status"] = "FAIL"
    elif res["reasons"]:
        res["status"] = "WARN"
    return res
