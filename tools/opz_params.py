#!/usr/bin/env python3
"""The OS's parameter path, modelled: which words reach an engine's `params`, `pitch` and `pitchBend`, and when.

Reference for research/param-paths.md (evidence and addresses there) and sdk/docs/engine-abi.md §4 and §6.
Every model here is our own integer port of what the OS 1.2.45 code does [code, opz.ldr and the desktop app]:

* every source (knob turn, MIDI CC, step lock, preset, $0E, punch-in, arpeggio, step component) sets a parameter
  BYTE; the sequencer tick turns the bytes into targets b * 0x808080 + (b >> 1) (opz.ldr 0x80117808);
* params[0..11] glide to their targets once per block (smooth_step, opz.ldr 0x801183d8), params[12..17] jump;
* a note-on writes all 18 bytes' words straight into the smoothed array, so they jump (opz.ldr 0x801179d8);
* the track LFO then rewrites one word per block (Lfo, opz.ldr 0x8012d994) and clamps it to 0..0x7FFFFFFF;
* the voice computes `pitch` with portamento, tuning, pitch bend and the LFO's vibrato (opz.ldr 0x8012e2c0).

So every params word is in 0..0x7FFFFFFF (Q-23), but any value in that range, from one block to the next.

stream_cases() turns the worst of it into per-block word streams for the cycle driver (tools/cycles/driver.c,
BenchCase.stream); tools/opz_crashcheck.py runs them on every engine. Run as a script, it plays them on engines
and reports crashes, CPU and output level per case:

    python3 tools/opz_params.py engines/NAME [engines/NAME ...] [--all] [--full] [--json FILE]

Python 3 standard library only (the script part needs the Blackfin toolchain, as opz_cycles.py).
"""

import argparse
import json
import math
import os
import random
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import opz_tables  # noqa: E402

FS = 44100
Q31_MAX = 0x7FFFFFFF
NUM_PARAMS = 18
SMOOTHED = 12                     # params[0..11] glide to their targets, params[12..17] jump
STREAM_WORDS = 20                 # per block: 18 params, then pitch at the block's start and end (FX: pitch bend)
P_P1, P_P2, P_ATTACK, P_FILTER, P_RES, P_PAN, P_LEVEL, P_PORTA = 0, 1, 2, 8, 9, 10, 11, 12
P_LFO_DEPTH, P_LFO_SPEED, P_LFO_TARGET, P_LFO_SHAPE = 13, 14, 15, 16
PITCH_SLOT = 18                   # the LFO's "pitch" destination writes this extra word (0x3FFFFFFF = none)

# LFO target byte >> 5 -> the word it modulates (P1, P2, filter, resonance, attack, pitch, pan, level)
LFO_DESTINATIONS = (0, 1, 8, 9, 2, PITCH_SLOT, 10, 11)
# shape byte -> one of 12 user shapes (6 free, 6 note-triggered) -> the OS's waveform number, per target group:
# P1, P2, filter, resonance, attack and level get unipolar waves, pitch and pan bipolar ones
LFO_GROUP = (2, 2, 2, 2, 2, 1, 1, 0)
LFO_SHAPES = ((0, 2, 4, 6, 13, 15, 0, 2, 4, 6, 13, 10),
              (1, 3, 12, 7, 14, 16, 1, 3, 12, 7, 14, 10),
              (0, 2, 4, 6, 13, 15, 0, 2, 4, 6, 13, 10))
SHAPE_NAMES = {0: "sine", 1: "sine", 2: "triangle", 3: "triangle", 4: "clipped sine (square)", 12: "square",
               6: "ramp up", 7: "ramp", 13: "random", 14: "random", 15: "tilt", 16: "tilt", 10: "one-shot ramp"}
BIPOLAR_WAVES = {1, 3, 5, 7, 9, 12, 14, 15, 16, 17}
SYNC_RATES = (256, 128, 64, 32, 16, 8, 4, 2)    # LFO speed bytes 0..127: phase per block = rate * BPM * 6464


def s32(x):
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x & 0x80000000 else x


def u32(x):
    return x & 0xFFFFFFFF


def sat32(x):
    return max(-2 ** 31, min(Q31_MAX, x))


def frac_mul(a, b):
    """Blackfin Mult32 (T): (a * b) >> 31, saturating 0x80000000 * 0x80000000."""
    if a == -2 ** 31 and b == -2 ** 31:
        return Q31_MAX
    return (a * b) >> 31


def word(b):
    """A parameter byte as the OS hands it on: b * 0x808080 + (b >> 1), 0..0x7FFFFFFF."""
    return (b & 255) * 0x808080 + ((b & 255) >> 1)


def smooth_step(cur, target):
    """One block of the OS's parameter smoothing (params[0..11]): 63/64 of the old word plus 1/64 of the target.
    It never leaves [min(cur, target), max(cur, target) + 1] and settles up to ~4,000 LSB below the target."""
    if cur == target:
        return cur
    return u32(63 * (cur >> 6) + (target >> 6) + (target >> 25))


def lfo_shape_byte(k):
    """The smallest shape byte that selects user shape k (0..11)."""
    return next(b for b in range(256) if ((word(b) >> 16) * 3) >> 13 == k)


def lfo_target_byte(dest_index):
    """A target byte for destination index 0..7 (P1, P2, filter, resonance, attack, pitch, pan, level)."""
    return dest_index * 32 + 16


_SINE = []


def _sine():
    if not _SINE:
        _SINE.extend(opz_tables.g_sine())
    return _SINE


class Lfo:
    """One track LFO: per block, copies the 18 words, rewrites its destination and advances its phase."""

    def __init__(self, seed=1):
        self.phase = 0
        self.rand = random.Random(seed)
        self.held = -0x70000000
        self.msb = 0
        self.tilt = 0                     # accelerometer reading, -0x7fff..0x7fff (shape "tilt")

    def wave(self, n):
        p = self.phase
        sine = _sine()
        if n == 0:
            return (sine[u32(p + 0xC0000000) >> 19] >> 1) + 0x3FFFFFFF
        if n == 1:
            return sine[p >> 19]
        if n in (2, 7):
            v = sat32(2 * s32(((p >> 1) ^ 0x40000000) + 0xC0000001))
            return (Q31_MAX if v == -2 ** 31 else abs(v)) if n == 2 else v
        if n == 3:
            u = u32(p + 0xC0000000)
            i = (u >> 1) + 0xC0000001 if u > 0x7FFFFFFD else 0x3FFFFFFF - (u >> 1)
            return sat32(4 * s32(i + 0xE0000001))
        if n == 4:
            return (sat32(4 * sine[u32(p + 0x0A5B4000) >> 19]) >> 1) + 0x3FFFFFFF
        if n == 5:
            return sat32(4 * sine[p >> 19])
        if n == 6:
            return p >> 1
        if n in (8, 10):
            return u32(~p) >> 1
        if n == 9:
            return s32(Q31_MAX - p)
        if n == 11:
            return Q31_MAX if s32(p) >= 0 else 0
        if n == 12:
            return Q31_MAX if s32(p) >= 0 else -2 ** 31
        if n in (13, 14):
            if p >> 31 != self.msb:
                self.msb = p >> 31
                self.held = self.rand.randrange(15) * 0x10000000 - 0x70000000
            return (self.held >> 1) + 0x3FFFFFFF if n == 13 else self.held
        if n in (15, 16):
            return s32(max(-0x7FFF, min(0x7FFF, self.tilt)) * 0x10002)
        return 0

    def render(self, words, bpm=120):
        """words: 18 smoothed parameter words. Returns the 19 words the engine (and the pitch) get this block."""
        out = list(words) + [0x3FFFFFFF]
        target = words[P_LFO_TARGET] >> 28
        dest = LFO_DESTINATIONS[target]
        n = LFO_SHAPES[LFO_GROUP[target]][((words[P_LFO_SHAPE] >> 16) * 3) >> 13]
        shape = self.wave(n)
        base = out[dest]
        depth = words[P_LFO_DEPTH]
        up = 2 * (depth - 0x41800000) if depth > 0x41800000 else 0
        down = 0x7C000000 - 2 * min(depth, 0x3E000000)
        if n in BIPOLAR_WAVES:
            a = sat32(up - down)
            if dest == PITCH_SLOT:
                a >>= 1
            v = sat32(base + frac_mul(frac_mul(a, abs(a)), shape))
        else:
            rest = sat32(Q31_MAX - base)
            lower = sat32(base - frac_mul(frac_mul(frac_mul(down, down), shape), base))
            v = sat32(lower + frac_mul(frac_mul(frac_mul(up, up), rest), shape))
        out[dest] = max(0, v)
        speed = words[P_LFO_SPEED]
        if speed < 0x3FFFFFFF:
            inc = u32(SYNC_RATES[speed >> 27] * bpm * 6464)
        else:
            x = u32(speed - 0x3FFFFFFF) >> 23
            inc = u32(2 * x ** 4 + 100000)
        self.phase = min(0xFFFFFFFF, self.phase + inc) if n == 10 else u32(self.phase + inc)
        return out


def vibrato_cents(out):
    """The LFO's pitch word -> cents (the OS: up to about +-191 cents at full depth)."""
    v = sat32(2 * (out[PITCH_SLOT] - 0x3FFFFFFF))
    return (v * 400) >> 31


def bend_cents(bend):
    """A pitch bend word (Q31, bipolar) -> cents: +-200."""
    return (bend * 200) >> 31


def note_pitch(note, tuning_cents=0, drum=False):
    """The OS's target pitch word: the note's phase increment (drum tracks: 1024) times the global tuning (+-1200
    cents, 1-cent steps), even (& ~1)."""
    base = 1024 if drum else 440.0 * 2.0 ** ((note - 69) / 12.0) / FS * 2 ** 32
    return int(base * 2.0 ** (tuning_cents / 1200.0)) & 0xFFFFFFFE


def detune(pitch, cents):
    return u32(int(pitch * 2.0 ** (cents / 1200.0)))


def portamento_rate(porta_word):
    """Glide multiplier per block: 32 (portamento 255, ~2 s for any interval) .. 1670 (portamento 1, ~38 ms)."""
    return (Q31_MAX - porta_word) // 0x140000 + 32


def glide(start, target, porta_word, blocks):
    """The OS's portamento: a fixed step (target - start) / 44100 * rate per block, stopping at the target."""
    step = (s32(target) - s32(start)) // 44100 if target >= start else -((s32(start) - s32(target)) // 44100)
    cur, out = start, []
    for _ in range(blocks):
        nxt = s32(cur) + step * portamento_rate(porta_word)
        cur = u32(min(nxt, s32(target)) if step > 0 else max(nxt, s32(target)))
        out.append(cur)
    return out


# --------------------------------------------------------------------------- streams for the cycle driver

def row(words, pitch=(0, 0)):
    """One block of a stream: 18 params, then (pitch at the start, pitch at the end) or (FX pitch bend, 0)."""
    return [u32(w) for w in words] + [u32(pitch[0]), u32(pitch[1])]


def lfo_stream(base_bytes, dest_index, depth_byte, shapes, blocks_per_shape, speed_byte=255, notes_pitch=None,
               tilt=False):
    """Per-block words of the OS LFO on one destination, each user shape in turn. notes_pitch: the voices' pitch
    word, to turn the pitch destination into (start, end) pitch pairs."""
    b = list(base_bytes)
    b[P_LFO_DEPTH], b[P_LFO_SPEED], b[P_LFO_TARGET] = depth_byte, speed_byte, lfo_target_byte(dest_index)
    rows, lfo, prev = [], Lfo(), None
    for k in shapes:
        b[P_LFO_SHAPE] = lfo_shape_byte(k)
        words = [word(x) for x in b]
        for i in range(blocks_per_shape):
            if tilt:                        # a hand tilting the OP-Z back and forth, full range, in 16 blocks
                lfo.tilt = int(0x7FFF * math.sin(2 * math.pi * i / 16))
            out = lfo.render(words)
            pitch = (0, 0)
            if notes_pitch:
                end = detune(notes_pitch, vibrato_cents(out))
                pitch = (prev if prev is not None else end, end)
                prev = end
            rows.append(row(out[:NUM_PARAMS], pitch))
    return rows


def extremes_stream(blocks):
    """Every word at the ends of the range and the knob range's centre crossings, each param out of phase."""
    vals = (0, Q31_MAX, 1, Q31_MAX - 1, 0x3FFFFFFF, 0x40000000, 0x40404040, 0x3FBFBFBF)
    return [row([vals[(i + 3 * p) % len(vals)] for p in range(NUM_PARAMS)]) for i in range(blocks)]


def jump_stream(blocks, every=1):
    """P1 and P2 jumping between 0 and 0x7FFFFFFF (opposite phases) every `every` blocks, as note-on latches or
    a full-depth LFO edge can make them; the other words stay at the case's bytes (filled in by the caller)."""
    out = []
    for i in range(blocks):
        hi = (i // every) % 2
        out.append((Q31_MAX if hi else 0, 0 if hi else Q31_MAX))
    return out


def step_lock_stream(blocks, every, seed=23):
    """Step locks on every parameter: new random bytes each `every` blocks (the note of the step latches them),
    smoothed in between as the OS does for params[0..11]."""
    rnd = random.Random(seed)
    cur = [word(128)] * NUM_PARAMS
    target = list(cur)
    out = []
    for i in range(blocks):
        if i % every == 0:
            bytes_ = [rnd.randrange(256) for _ in range(NUM_PARAMS)]
            bytes_[P_LFO_DEPTH] = 128                   # LFO off: the words are the locks themselves
            target = [word(x) for x in bytes_]
            cur = list(target)                          # the note-on latch
        else:
            cur = [smooth_step(c, t) if p < SMOOTHED else t for p, (c, t) in enumerate(zip(cur, target))]
        out.append(row(cur))
    return out


def knob_jump_stream(blocks):
    """A knob flicked 0 -> 255 -> 0 -> 128 on P1, P2, filter and level, smoothed by the OS (non-lattice words)."""
    cur = [word(b) for b in DEFAULT_BYTES]
    out = []
    for i in range(blocks):
        t = (255, 0, 128, 0)[(i // max(1, blocks // 4)) % 4]
        for p in (P_P1, P_P2, P_FILTER, P_LEVEL):
            cur[p] = smooth_step(cur[p], word(t if p != P_P2 else 255 - t))
        out.append(row(cur))
    return out


def pitch_jump_stream(base_bytes, blocks, every=4, tuning=1199):
    """Legato with no portamento between the lowest and the highest pitch: note 0 at -tuning cents, bend -2,
    vibrato -1.9, and note 127 at +tuning, bend +2, vibrato +1.9 (with the OS's +-12 semitones tuning: 0x4D8C7 and
    0xB66E2BE5, above 0x80000000; at standard tuning 0x9B463 and 0x5B371..). A jump reaches the engine as one
    block's pitchSlope."""
    lo = detune(note_pitch(0, -tuning), -391)
    hi = detune(note_pitch(127, tuning), 391)
    words = [word(b) for b in base_bytes]
    out, cur = [], lo
    for i in range(blocks):
        nxt = hi if (i // every) % 2 else lo
        out.append(row(words, (cur, nxt)))
        cur = nxt
    return out


def portamento_stream(base_bytes, blocks, porta_byte=1, tuning=1199):
    """The fastest portamento from note 0 to note 127 at +tuning cents and back (with +12 semitones the words go
    above 0x7FFFFFFF)."""
    words = [word(b) for b in base_bytes]
    words[P_PORTA] = word(porta_byte)
    lo, hi = note_pitch(0, 0), note_pitch(127, tuning)
    path = glide(lo, hi, words[P_PORTA], blocks // 2) + glide(hi, lo, words[P_PORTA], blocks - blocks // 2)
    out, prev = [], lo
    for p in path:
        out.append(row(words, (prev, p)))
        prev = p
    return out


def bend_stream(base_bytes, blocks):
    """FX pitch bend at its ends: 0x80000000 (MIDI bend 0), 0x7FFFFFFF, 0, alternating every block."""
    words = [word(b) for b in base_bytes]
    vals = (-2 ** 31, Q31_MAX, 0, -2 ** 31, 0x40000000)
    return [row(words, (vals[i % len(vals)], 0)) for i in range(blocks)]


#                 P1   P2  att  dec  sus  rel fx1 fx2 filt res  pan  lvl por ldp lsp ltg lsh sty
DEFAULT_BYTES = [128, 128,  0,  64, 200,  64,  0,  0, 128,  0, 128, 200,  0, 128, 64,  0,  0,  0]


def stream_cases(is_fx, drum, quick=True):
    """The OS's worst parameter streams as cycle-driver cases (opz_cycles.case dicts): the LFO at full depth and
    its fastest rate on P1, on P2 (negative depth) and on attack/pitch, every word at its extremes, P1/P2 jumping
    0 <-> 0x7FFFFFFF with a note each time, step locks on all 18 parameters, knob flicks through the OS smoother,
    pitch extremes (instruments) or pitch bend extremes (FX)."""
    import opz_cycles                                       # here: opz_cycles imports nothing from this module
    per = 8 if quick else 24                                # blocks per LFO shape
    blocks = 32 if quick else 96
    if is_fx:
        notes, track = [255], 8
    elif drum:
        notes, track = [53, 57, 60, 64], 0
    else:
        notes, track = list(opz_cycles.CHORD), 4
    pitches = [note_pitch(0, drum=True)] * len(notes) if drum else None
    base = list(DEFAULT_BYTES)
    warm = 8 if is_fx else 0

    def mk(name, rows, resend=0, nts=None, extra=0):
        n = nts or notes
        return opz_cycles.case("OS stream: " + name, base, n, track, len(rows) + warm + extra, warmup=warm,
                               resend=resend, group="param streams", stream=rows,
                               pitch=(pitches[:len(n)] if pitches and not is_fx else None))

    free = range(6)
    out = [mk("LFO on P1 from 0, depth 255, fastest rate, each free shape",
              lfo_stream([0] + base[1:], 0, 255, free, per)),
           mk("LFO on P2, depth 0 (full negative), fastest rate, P2 at 255, each free shape",
              lfo_stream(base[:1] + [255] + base[2:], 1, 0, free, per)),
           mk("LFO on P1, tilt shape, the unit tilted through its full range",
              lfo_stream(base, 0, 255, [5], 2 * per, tilt=True)),
           mk("LFO on attack, depth 255, fastest rate, note-triggered shapes", lfo_stream(base, 4, 255, range(6, 12),
                                                                                       per)),
           mk("every word at its extremes, changing every block", extremes_stream(blocks))]
    jumps = [row([a, b] + [word(x) for x in base[2:]]) for a, b in jump_stream(blocks)]
    out.append(mk("P1/P2 jump 0 <-> 0x7FFFFFFF every block, a new note every 2 blocks", jumps, resend=2))
    out.append(mk("step locks on all 18 parameters, a new step every 2 blocks", step_lock_stream(blocks, 2),
                  resend=2))
    out.append(mk("knob flicks through the OS smoother", knob_jump_stream(blocks)))
    if is_fx:
        out.append(mk("pitch bend 0x80000000 / 0x7FFFFFFF / 0, every block", bend_stream(base, blocks)))
    elif not drum:
        out.append(mk("LFO on pitch, depth 255, fastest rate, each free shape",
                      lfo_stream(base, 5, 255, free, per, notes_pitch=note_pitch(60))))
        out.append(mk("pitch jumps note 0 <-> 127, bend and vibrato at full, standard tuning (legato)",
                      pitch_jump_stream(base, blocks, tuning=0)))
        out.append(mk("pitch jumps as above at +-12 semitones tuning: words above 0x80000000",
                      pitch_jump_stream(base, blocks)))
        out.append(mk("fastest portamento, note 0 <-> note 127, standard tuning",
                      portamento_stream(base, blocks, tuning=0)))
        out.append(mk("fastest portamento, note 0 <-> note 127 at +12 semitones tuning",
                      portamento_stream(base, blocks)))
    else:
        out.append(mk("LFO on pitch at a drum track's unity rate", lfo_stream(base, 5, 255, free, per,
                                                                             notes_pitch=note_pitch(0, drum=True))))
    return out


# --------------------------------------------------------------------------- script: play them on engines

def engine_kind(engine_dir):
    with open(os.path.join(engine_dir, "plug.json")) as f:
        mask = int(json.load(f)["plug"]["trackmask"])
    is_fx = not (mask & 0xFF)
    return is_fx, (not is_fx and not (mask & 0xF0))


def peak_dbfs(samples):
    peak = max((abs(s) for s in samples), default=0)
    return round(20 * math.log10(peak / 2 ** 31), 1) if peak else None


def check(engine_dir, toolchain, out_root, quick=True):
    """Play the engine's own benchmark cases (the baseline) and the stream cases: crash findings, worst cycles per
    sample and the output peak per case."""
    import opz_cycles
    is_fx, drum = engine_kind(engine_dir)
    name = os.path.basename(os.path.abspath(engine_dir).rstrip("/"))
    base = opz_cycles.cases_for(name, is_fx, quick, drum)
    for c in base:
        c["group"] = "baseline"
    cases = base + stream_cases(is_fx, drum, quick)
    os.makedirs(out_root, exist_ok=True)
    dump = tempfile.mkdtemp(prefix="opz_params_", dir=out_root)
    try:
        rep = opz_cycles.measure(engine_dir, toolchain, out_root, cases=cases, quiet=True, dump=dump, torn=True)
    except opz_cycles.SimulatorCrash as e:
        return {"engine": name, "crash": e.info["message"], "cases": []}
    dumps = opz_cycles.load_dump(os.path.join(dump, name))
    res = {"engine": name, "fx": is_fx, "findings": rep["checks"]["findings"], "cases": []}
    for c, s in zip(cases, rep["cases"]):
        res["cases"].append({"case": c["name"], "baseline": c["group"] == "baseline",
                             "per_sample_max": s["per_sample_max"], "per_sample_mean": s["per_sample_mean"],
                             "peak_dbfs": peak_dbfs(dumps[c["name"]]), "verdict": s["verdict"]})
    return res


VERDICT_RANK = {"ok": 0, "tight": 1, "over budget": 2, "over FX target": 2}
FX_INPUT_DBFS = -11.5                           # the cycle driver's FX input: a -12 dBFS saw plus a little noise


def problems(res):
    """What looks wrong in check()'s result, against the engine's own benchmark cases: a crash or finding; an
    output at full scale that no benchmark case reaches; an output 6 dB above the loudest benchmark case (effects:
    or above their input); a worse CPU verdict (ok / tight / over budget) than the benchmark's worst, or a block
    over the budget that costs 1.5x the benchmark's worst."""
    if res.get("crash"):
        return [res["crash"]]
    out = ["%s in %s: %s" % (f["check"], f["case"], f["detail"]) for f in res["findings"]]
    base = [c for c in res["cases"] if c["baseline"]]
    peaks = [c["peak_dbfs"] for c in base if c["peak_dbfs"] is not None]
    loud = max(peaks) if peaks else None
    worst = max(c["per_sample_max"] for c in base)
    rank = max(VERDICT_RANK.get(c["verdict"], 2) for c in base)
    if res.get("fx") and loud is not None:
        loud = max(loud, FX_INPUT_DBFS)         # an effect passing its input dry is not too loud
    for c in res["cases"]:
        if c["baseline"]:
            continue
        p = c["peak_dbfs"]
        if p is not None and p >= -0.1 and (loud is None or loud < -0.1):
            out.append("%s: output at full scale (%.1f dBFS; loudest benchmark case %s dBFS)" % (c["case"], p, loud))
        elif p is not None and loud is not None and p >= loud + 6:
            out.append("%s: output %.1f dBFS, loudest benchmark case %.1f dBFS" % (c["case"], p, loud))
        r = VERDICT_RANK.get(c["verdict"], 2)
        if r > rank or (r and c["per_sample_max"] >= 1.5 * worst):
            out.append("%s: %.1f cycles/sample worst (benchmark worst %.1f), %s" % (
                c["case"], c["per_sample_max"], worst, c["verdict"]))
    return out


def main(argv=None):
    import opz_cycles
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("engines", nargs="*")
    ap.add_argument("--all", action="store_true", help="every engine under engines/")
    ap.add_argument("--full", action="store_true", help="longer streams (default: quick)")
    ap.add_argument("--json", help="write the results here")
    ap.add_argument("--toolchain", default=os.path.expanduser("~/opt/bfin-elf"))
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "build"))
    a = ap.parse_args(argv)
    dirs = list(a.engines)
    if a.all:
        top = os.path.join(ROOT, "engines")
        dirs += [os.path.join(top, d) for d in sorted(os.listdir(top))
                 if os.path.exists(os.path.join(top, d, "plug.json"))]
    if not dirs:
        ap.error("name an engine or pass --all")
    if not opz_cycles.available(a.toolchain):
        sys.stderr.write("opz_params: bfin-elf toolchain not found in %s/bin (research/toolchain-setup.md)\n"
                         % a.toolchain)
        return 2
    results, bad = [], 0
    for d in dirs:
        r = check(d, a.toolchain, a.out, quick=not a.full)
        results.append(r)
        p = problems(r)
        bad += bool(p)
        print("%-22s %s" % (r["engine"], "ok" if not p else "PROBLEMS"))
        for line in p:
            print("    " + line)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(results, f, indent=1)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
