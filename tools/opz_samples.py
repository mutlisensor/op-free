"""OP-Z sample packs: the OP-1 AIFF format the OS loads, a kit builder, and the rules that keep the user's samples safe.

Pure helpers, no device access (tools/opz.py does the I/O). Findings and evidence: research/samples.md.

Format, as the OS 1.2.45 SampleLoader reads it [code, device FUN_8010576c / FUN_80105e94]:

- An AIFF or AIFF-C file of **16-bit mono PCM at 44,100 Hz** (TE writes AIFF-C `sowt`, little-endian samples).
- An `APPL` chunk holding `op-1` + a JSON object; `"type": "drum"` (24 keys) or `"type": "sampler"` (one synth sample,
  exactly 8 `knobs`). Without it the file is a plain sample (capped at 6 s).
- Drum keys: `start[i]`/`end[i]` are positions where 2**31 = 529,200 frames (12.0 s); the OS converts them with a Q31
  multiply, `frame = pos * 529200 / 2**31`. `volume`, `pitch`, `playmode`, `reverse` are OP-1 knob values (0..32767),
  padded to 24 entries. A key plays `samples[start .. end)`.
- A pack is at most 529,200 frames (12.0 s, 1,058,400 bytes): one slot of the OS's 14-slot sample cache.
"""

from __future__ import annotations

import array
import json
import math
import os
import re
import struct
import sys
import wave
import zlib

SAMPLE_RATE = 44100
KEYS = 24
DRUM_MAX_FRAMES = 0x81330            # 529,200 frames = 12.0 s: the OS's per-pack limit and drum position scale
SYNTH_MAX_FRAMES = 0x40998           # 264,600 frames = 6.0 s: synth-sample position scale (manual: 6 s)
POS_FULL = 1 << 31                   # a drum position of 2**31 is DRUM_MAX_FRAMES frames
CACHE_SLOTS = 14                     # packs resident at once (14 x 0x102660 bytes from 0x82700000)
USER_STORAGE_BYTES = 24 * 1024 * 1024  # the OS's limit for files in samplepacks/user/ (0x1800000)
SAMPLES_PATH_MAX = 95                # the plug record keeps 96 bytes of the samples path, NUL included
USER_FLAG = 0x02                     # plug flag of samples made on the OP-Z or imported by TE's app/content mode

DRUM_ENGINE = "engines/sampleplayergain.engine"
SYNTH_ENGINE = "engines/synthsampler.engine"
USER_DIR = "samplepacks/user/"
# Every pack our tools write lives here, and only there: one folder deep, like TE's factory packs. The file service
# can't write two folders deep (it makes the inbox folder with a single mkdir, FUN_80129454), so a pack in
# samplepacks/user/ never lands (Q-49, research/samples.md section 6).
OUR_PREFIX = "samplepacks/opzk_"
LEGACY_PREFIX = USER_DIR + "opzk_"   # where H-S1's first run tried to write (2026-11-01); nothing can be there
OUR_PLUG_IDS = range(61400, 61500)   # plug ids for sample plugs made by our tools (sdk/docs/memory-slots.md)
USER_PLUG_MIN = 60000

# Knob defaults for a key (OP-1 convention, TE's packs use the same scale) [inferred: meaning of each value].
DEFAULT_VOLUME = 8192
DEFAULT_PITCH = 0
DEFAULT_PLAYMODE = 8192
DEFAULT_REVERSE = 8192               # below 16384 plays forward (sampleplayergain tests reverse >> 24 > 127) [code]

_FVER_AIFC = 0xA2805140
_SOWT_NAME = b"Signed integer (little-endian) linear PCM"


class PackError(ValueError):
    pass


# ---------------------------------------------------------------------------------------------------------------
# Positions
# ---------------------------------------------------------------------------------------------------------------

def frame_from_pos(pos: int) -> int:
    """What the OS computes: a Q31 multiply by 529,200 (rounded; truncating gives the same for our positions)."""
    return (pos * DRUM_MAX_FRAMES * 2 + (1 << 31)) >> 32


def pos_from_frame(frame: int) -> int:
    """The smallest position that maps back to `frame` under both rounding and truncation."""
    if not 0 <= frame <= DRUM_MAX_FRAMES:
        raise PackError("frame %d outside 0..%d" % (frame, DRUM_MAX_FRAMES))
    return min(-(-frame * POS_FULL // DRUM_MAX_FRAMES), 0x7FFFFFFF)


# ---------------------------------------------------------------------------------------------------------------
# AIFF reading and writing
# ---------------------------------------------------------------------------------------------------------------

def _ext80(rate: float) -> bytes:
    """IEEE 754 80-bit extended, as AIFF's COMM chunk stores the sample rate."""
    if rate <= 0:
        raise PackError("sample rate must be positive")
    exp = int(math.floor(math.log2(rate)))
    mant = int(round(rate / 2 ** exp * (1 << 63)))
    if mant >> 64:
        mant >>= 1
        exp += 1
    return struct.pack(">HQ", exp + 16383, mant)


def _from_ext80(b: bytes) -> float:
    se, mant = struct.unpack(">HQ", b)
    if se & 0x7FFF == 0 and mant == 0:
        return 0.0
    v = mant * 2.0 ** ((se & 0x7FFF) - 16383 - 63)
    return -v if se & 0x8000 else v


def _chunk(cid: bytes, data: bytes) -> bytes:
    return cid + struct.pack(">I", len(data)) + data + (b"\0" if len(data) & 1 else b"")


def drum_meta(name: str, starts: list[int], ends: list[int], volume=None, pitch=None, playmode=None,
              reverse=None) -> dict:
    """The APPL JSON for a drum pack. Lists are per key (24); starts/ends are frames, written as positions."""
    if len(starts) != KEYS or len(ends) != KEYS:
        raise PackError("a drum pack needs %d starts and ends" % KEYS)

    def knob(values, default):
        values = [default] * KEYS if values is None else list(values)
        if len(values) != KEYS or not all(isinstance(v, int) and -32768 <= v <= 32767 for v in values):
            raise PackError("per-key knob lists need %d integers" % KEYS)
        return values

    return {
        "drum_version": 2, "type": "drum", "name": str(name)[:32], "octave": 0,
        "start": [pos_from_frame(s) for s in starts], "end": [pos_from_frame(e) for e in ends],
        "volume": knob(volume, DEFAULT_VOLUME), "pitch": knob(pitch, DEFAULT_PITCH),
        "playmode": knob(playmode, DEFAULT_PLAYMODE), "reverse": knob(reverse, DEFAULT_REVERSE),
        "dyna_env": [0, 8192, 0, 8192, 0, 0, 0, 0],
        "fx_active": False, "fx_type": "delay", "fx_params": [8000] * 8,
        "lfo_active": False, "lfo_type": "tremolo", "lfo_params": [16000, 16000, 16000, 16000, 0, 0, 0, 0],
    }


def write_aiff(samples, meta: dict | None) -> bytes:
    """AIFF-C, 16-bit mono 44.1 kHz little-endian (`sowt`) like TE's packs, with an `op-1` APPL chunk if meta."""
    pcm = array.array("h", samples)
    if sys.byteorder != "little":
        pcm.byteswap()
    comm = struct.pack(">hIh", 1, len(pcm), 16) + _ext80(SAMPLE_RATE) + b"sowt" \
        + bytes([len(_SOWT_NAME)]) + _SOWT_NAME
    if len(comm) & 1:
        comm += b"\0"
    body = b"AIFC" + _chunk(b"FVER", struct.pack(">I", _FVER_AIFC)) + _chunk(b"COMM", comm)
    if meta is not None:
        appl = b"op-1" + json.dumps(meta, sort_keys=True, separators=(",", ":")).encode() + b"\n"
        if len(appl) & 1:
            appl += b" "
        body += _chunk(b"APPL", appl)
    body += _chunk(b"SSND", struct.pack(">II", 0, 0) + pcm.tobytes())
    return b"FORM" + struct.pack(">I", len(body)) + body


def parse(data: bytes) -> dict:
    """Read a pack and check it the way the OS does. Returns {kind, frames, channels, bits, rate, meta, keys,
    errors, warnings}; kind is 'drum', 'synth' or 'plain'. keys = [(start_frame, end_frame)] for drum packs."""
    out = {"kind": None, "frames": 0, "channels": 0, "bits": 0, "rate": 0.0, "meta": None, "keys": [],
           "errors": [], "warnings": [], "samples": None}
    err, warn = out["errors"], out["warnings"]
    if len(data) < 12 or data[:4] != b"FORM" or data[8:12] not in (b"AIFF", b"AIFC"):
        err.append("not an AIFF file")
        return out
    aifc = data[8:12] == b"AIFC"
    chunks, p = {}, 12
    while p + 8 <= len(data):
        cid, n = data[p:p + 4], struct.unpack(">I", data[p + 4:p + 8])[0]
        chunks.setdefault(cid, data[p + 8:p + 8 + n])
        p += 8 + n + (n & 1)
    comm = chunks.get(b"COMM")
    if comm is None or len(comm) < 18:
        err.append("no COMM chunk")
        return out
    ch, frames, bits = struct.unpack(">hIh", comm[:8])
    rate = _from_ext80(comm[8:18])
    comp = comm[18:22] if aifc and len(comm) >= 22 else b"NONE"
    out.update(channels=ch, frames=frames, bits=bits, rate=rate)
    if rate != SAMPLE_RATE:
        err.append("unsupported sample rate %g (the OS needs 44100)" % rate)
    if ch != 1:
        err.append("%d channels: packs are mono" % ch)
    if bits != 16:
        err.append("%d-bit: packs are 16-bit" % bits)
    if comp not in (b"NONE", b"sowt"):
        err.append("compression %r is not supported" % comp.decode("latin-1"))
    if frames > DRUM_MAX_FRAMES:
        err.append("pack is too big: %d frames > %d (12 s)" % (frames, DRUM_MAX_FRAMES))
    ssnd = chunks.get(b"SSND")
    if ssnd is None or len(ssnd) < 8:
        err.append("no SSND chunk")
    elif ch == 1 and bits == 16:
        offset = struct.unpack(">I", ssnd[:4])[0]
        raw = ssnd[8 + offset:8 + offset + 2 * frames]
        if len(raw) < 2 * frames:
            err.append("SSND holds %d frames, COMM says %d" % (len(raw) // 2, frames))
        pcm = array.array("h", raw[:len(raw) // 2 * 2])
        if (comp == b"sowt") != (sys.byteorder == "little"):
            pcm.byteswap()
        out["samples"] = pcm
    appl = chunks.get(b"APPL")
    if appl is None or not appl.startswith(b"op-1"):
        out["kind"] = "plain"
        warn.append("no op-1 metadata: the OS treats it as one plain sample, capped at 6 s")
        return out
    try:
        meta = json.loads(appl[4:].decode("utf-8").strip("\0 \n"))
    except (UnicodeDecodeError, ValueError):
        err.append("op-1 metadata is not readable JSON")
        return out
    out["meta"] = meta
    kind = meta.get("type")
    if kind == "drum":
        out["kind"] = "drum"
        starts, ends = meta.get("start", []), meta.get("end", [])
        n = min(len(starts), len(ends), KEYS)
        if n == 0:
            err.append("drum pack without start/end positions")
        elif n < KEYS:
            warn.append("%d keys defined: the OS plays the last one on keys %d-%d" % (n, n + 1, KEYS))
        for i in range(n):
            s, e = frame_from_pos(int(starts[i])), frame_from_pos(int(ends[i]))
            if not 0 <= s <= e <= frames:
                err.append("key %d: start %d / end %d outside 0..%d frames" % (i + 1, s, e, frames))
            out["keys"].append((s, e))
    elif kind == "sampler":
        out["kind"] = "synth"
        if len(meta.get("knobs", [])) != 8:
            err.append("synth sample needs exactly 8 knobs")
        if frames > SYNTH_MAX_FRAMES:
            err.append("synth sample is %d frames: only the first 6 s (%d) are reachable" % (frames, SYNTH_MAX_FRAMES))
    else:
        err.append("type %r is neither drum nor sampler" % kind)
    return out


# ---------------------------------------------------------------------------------------------------------------
# Building a kit
# ---------------------------------------------------------------------------------------------------------------

def to_int16(values) -> array.array:
    return array.array("h", (max(-32768, min(32767, int(round(v)))) for v in values))


def read_wav(path: str) -> tuple[array.array, list[str]]:
    """A WAV as int16 mono at 44.1 kHz: channels averaged, other rates resampled linearly (with a warning)."""
    notes = []
    with wave.open(path, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width not in (1, 2, 3, 4):
        raise PackError("%s: %d-byte samples are not supported" % (path, width))
    scale = 1 << (8 * width - 16) if width > 2 else 1
    vals = []
    for i in range(0, len(raw) - len(raw) % (width * ch), width * ch):
        acc = 0
        for c in range(ch):
            b = raw[i + c * width:i + (c + 1) * width]
            v = b[0] - 128 if width == 1 else int.from_bytes(b, "little", signed=True)
            acc += v * 256 if width == 1 else v / scale if width > 2 else v
        vals.append(acc / ch)
    if ch > 1:
        notes.append("%s: %d channels mixed to mono" % (os.path.basename(path), ch))
    if rate != SAMPLE_RATE and vals:
        notes.append("%s: resampled %d Hz -> 44100 Hz (linear)" % (os.path.basename(path), rate))
        out_n = max(1, int(len(vals) * SAMPLE_RATE / rate))
        step = rate / SAMPLE_RATE
        res = []
        for j in range(out_n):
            x = j * step
            i = int(x)
            f = x - i
            a = vals[min(i, len(vals) - 1)]
            b = vals[min(i + 1, len(vals) - 1)]
            res.append(a + (b - a) * f)
        vals = res
    return to_int16(vals), notes


def build_drum_pack(sounds: list, name: str, volume=None, pitch=None, playmode=None, reverse=None) -> bytes:
    """Concatenate up to 24 sounds (int16 sequences) into one drum pack; with fewer, keys repeat the sounds in order
    (key i plays sound i mod n), so every key makes a sound."""
    if not sounds:
        raise PackError("no sounds")
    if len(sounds) > KEYS:
        raise PackError("%d sounds: a drum pack has %d keys" % (len(sounds), KEYS))
    pcm = array.array("h")
    spans = []
    for s in sounds:
        s = array.array("h", s)
        if not s:
            raise PackError("empty sound")
        spans.append((len(pcm), len(pcm) + len(s)))
        pcm.extend(s)
    if len(pcm) > DRUM_MAX_FRAMES:
        raise PackError("the sounds add up to %.2f s; a pack holds 12.0 s" % (len(pcm) / SAMPLE_RATE))
    starts = [spans[i % len(spans)][0] for i in range(KEYS)]
    ends = [spans[i % len(spans)][1] for i in range(KEYS)]
    return write_aiff(pcm, drum_meta(name, starts, ends, volume, pitch, playmode, reverse))


def test_kit_sounds() -> list[array.array]:
    """24 short sounds synthesised here (no recordings): kicks, snare, clap, hats, six toms, rim, cowbell, and ten
    rising blips (C5 upwards) on the top keys so a listener can check the key mapping. About 3.3 s in all."""
    sr = SAMPLE_RATE
    seed = [0x1234567]

    def noise():
        seed[0] = (seed[0] * 1103515245 + 12345) & 0x7FFFFFFF
        return seed[0] / 0x3FFFFFFF - 1.0

    def render(seconds, fn, level=0.7):
        n = int(seconds * sr)
        fade = min(64, n)
        out = []
        for i in range(n):
            v = fn(i / sr, i)
            if i >= n - fade:
                v *= (n - i) / fade
            out.append(v * level * 32767)
        return to_int16(out)

    def sweep(f0, f1, decay, seconds, click=0.0):
        phase = [0.0]

        def fn(t, i):
            f = f1 + (f0 - f1) * math.exp(-t * 30)
            phase[0] += 2 * math.pi * f / sr
            return math.sin(phase[0]) * math.exp(-t / decay) + (click * noise() if i < 40 else 0.0)
        return render(seconds, fn, 0.8)

    def hat(seconds, decay):
        last = [0.0]

        def fn(t, i):
            x = noise()
            y, last[0] = x - last[0], x          # first difference: bright, hat-like noise
            return y * 0.5 * math.exp(-t / decay)
        return render(seconds, fn)

    def snare(t, i):
        return 0.6 * noise() * math.exp(-t / 0.05) + 0.5 * math.sin(2 * math.pi * 185 * t) * math.exp(-t / 0.04)

    def clap(t, i):
        env = sum(math.exp(-(t - d) / 0.012) for d in (0.0, 0.011, 0.022) if t >= d) + 0.4 * math.exp(-t / 0.06)
        return 0.5 * noise() * min(env, 1.5)

    def rim(t, i):
        return math.sin(2 * math.pi * 1700 * t) * math.exp(-t / 0.006)

    def cowbell(t, i):
        sq = (1 if math.sin(2 * math.pi * 540 * t) >= 0 else -1) + (1 if math.sin(2 * math.pi * 800 * t) >= 0 else -1)
        return 0.35 * sq * math.exp(-t / 0.05)

    def blip(f):
        return render(0.08, lambda t, i: math.sin(2 * math.pi * f * t) * math.exp(-t / 0.03), 0.6)

    sounds = [sweep(160, 45, 0.18, 0.25, 0.3), sweep(220, 55, 0.08, 0.15, 0.5), render(0.18, snare), render(0.18, clap),
              hat(0.05, 0.012), hat(0.2, 0.07)]
    sounds += [sweep(f * 1.6, f, 0.09, 0.2) for f in (220, 185, 155, 130, 110, 90)]
    sounds += [render(0.03, rim), render(0.15, cowbell)]
    sounds += [blip(523.25 * 2 ** (k / 12)) for k in range(10)]
    return sounds


def tiny_kit_sounds() -> list[array.array]:
    """24 sine blips of 40 ms, one semitone apart from C5 (key 1) upwards: a ladder that shows the key mapping, about
    85 KB as a pack (a third of the full test kit), so the hardware check's upload is short. Integer-free but
    deterministic (same floats on every machine)."""
    out = []
    n = SAMPLE_RATE // 25
    for k in range(KEYS):
        f = 523.25 * 2 ** (k / 12)
        out.append(to_int16([0.5 * 32767 * math.sin(2 * math.pi * f * i / SAMPLE_RATE) * (1 - i / n) for i in range(n)]))
    return out


# ---------------------------------------------------------------------------------------------------------------
# Paths, ids and the rules that protect the user's samples
# ---------------------------------------------------------------------------------------------------------------

def pack_id(path: str) -> int:
    """The OS's sample-pack id: crc32 of the plug's `samples` path (PlugManager makeHash). TE's app and content mode
    also use it as the plug id of a user sample (12 of 12 on the user's OP-Z match)."""
    return zlib.crc32(path.encode("utf-8")) & 0xFFFFFFFF if path else 0


def upload_path_error(path: str) -> str | None:
    """Why the file service can't write `path`, or None. The OP-Z stages a download in inbox/<path>, making only the
    last folder (one mkdir, FUN_80129454 -> FUN_801288b8); inbox/a/ exists only if a one-level file a/x was ever
    written. So a/b/x fails unseen: no file to finalise ("invalid file crc"), nothing written, the sync still reports
    complete [code, device]."""
    if path.count("/") > 1:
        return ("%s is two folders deep: the OP-Z's file service can only write files one folder deep (it stages "
                "them in inbox/ with a single mkdir) and would drop it without an error" % path)
    return None


def pack_path(name: str) -> str:
    slug = re.sub(r"[^a-z0-9_-]+", "-", name.lower()).strip("-")
    if not slug:
        raise PackError("pack name %r has no usable characters" % name)
    path = OUR_PREFIX + slug + ".aif"
    if len(path) > SAMPLES_PATH_MAX:
        raise PackError("%s is longer than the %d characters a plug record keeps" % (path, SAMPLES_PATH_MAX))
    return path


def is_user_sample(plug: dict) -> bool:
    """A sample made on the OP-Z, by TE's app or through content mode: flag 2, any id."""
    return bool(int(plug.get("flags", 0)) & USER_FLAG)


def is_our_sample(plug: dict) -> bool:
    return (int(plug["id"]) in OUR_PLUG_IDS and str(plug.get("samples", "")).startswith(OUR_PREFIX)
            and not is_user_sample(plug))


def removal_block(plug: dict, plugs: dict) -> str | None:
    """Why removing this plug would harm content we don't own, or None. The OS deletes a removed plug's sample file
    (FUN_800fe700), so a plug with a sample may only go if the sample is ours and no other plug uses it."""
    samples = str(plug.get("samples", ""))
    if is_user_sample(plug):
        return "plug %d is a user sample (%s); removing it deletes the sample file from the OP-Z" % (plug["id"], samples)
    if samples and not is_our_sample(plug):
        return "plug %d uses %s, which our tools did not install; removing it deletes that file" % (plug["id"], samples)
    if samples:
        others = [p["id"] for p in plugs.get("plugs", []) if p.get("samples") == samples and p["id"] != plug["id"]]
        if others:
            return "plugs %s also use %s; removing plug %d would delete it under them" % (others, samples, plug["id"])
    return None


def assignments(slots: dict) -> dict[int, list[tuple[int, int]]]:
    out: dict[int, list[tuple[int, int]]] = {}
    for t in slots.get("tracks", []):
        for p in t.get("packs", []):
            out.setdefault(p["id"], []).append((t["index"], p["slot"]))
    return out


def orphaned_user_samples(plugs: dict, before: dict, after: dict) -> list[dict]:
    """User samples assigned in `before` and nowhere in `after`. Content mode removes unassigned user-sample plugs
    and then deletes every file in samplepacks/user/ that no plug names (FUN_80038cd8 -> FUN_800ffc88, FUN_800364c4),
    so leaving one unassigned puts the user's sample at risk."""
    a, b = assignments(before), assignments(after)
    return [p for p in plugs.get("plugs", []) if is_user_sample(p) and a.get(p["id"]) and not b.get(p["id"])]


def unassigned_user_samples(plugs: dict, slots: dict) -> list[dict]:
    a = assignments(slots)
    return [p for p in plugs.get("plugs", []) if is_user_sample(p) and not a.get(p["id"])]


def classify(plug: dict) -> str:
    if not plug.get("samples"):
        return "engine"
    if is_user_sample(plug):
        return "user"
    if is_our_sample(plug):
        return "ours"
    if int(plug["id"]) < USER_PLUG_MIN:
        return "factory"
    return "other"
