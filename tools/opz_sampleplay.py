"""What TE's sample engines do with a slice: the playback maths of `sampleplayergain` (drum tracks) and `synthsampler`
(synth tracks) on OS 1.2.45, as a reference model for kit builders and for sample engines of our own.

Pure functions, no device access. Evidence and the field table: research/samples.md §8 (Q-26). Sources [code]:
the OS loader that builds the slices (device `FUN_80105e94`, `FUN_80107648`/`FUN_801076d8` drum rate,
`FUN_80107760` synth rate), TE's engines on the device (`sampleplayergain.onNoteOn` at `0x81b69f20`) and their twins
in TE's desktop app, which carry symbols (`SamplePlayerGain::onNoteOn`/`render`, `SynthSampler::onNoteOn`/`render`,
`drumPitchToDetune`, `op1SynthBaseFreqToDetune`, `s_pitches`). Where the app and the device were both read they agree.

    python3 tools/opz_sampleplay.py table          # the field table as Markdown
    python3 tools/opz_sampleplay.py slices FILE    # the slices the OS would build from a pack

Units: knob values are the OP-1's 0..32767 (pitch -32768..32767); a "word" is a uint32 engine parameter (byte << 23,
sdk/docs/engine-abi.md); positions in TE's engines are Q10 frames.
"""

from __future__ import annotations

import math
import struct
import sys

import opz_samples as smp

# ---------------------------------------------------------------------------------------------------------------
# The slice words the OS loader writes (OpzSampleSlice, sdk/include/opz_engine.h)
# ---------------------------------------------------------------------------------------------------------------

RATE_UNITY = 1 << 27            # slice rate word for "no transposition": 1/16 in Q31
RATE_MIN = 1 << 23              # 1/256: the loader clamps the ratio/16 to [1/256, 1]
RATE_MAX = 0x7FFFFFFF           # 1.0 saturates
PITCH_PER_SEMITONE = 512        # drum `pitch` knob units per semitone (24,576 = 4,800 cents)
BASE_FREQ = 261.6255798339844   # synth `base_freq` default, C4 (float 0x4382D013)
PLAYMODE_THRESHOLDS = (11008, 21888)   # drum playmode: below = gate, below the second = trigger, else loop
REVERSE_FROM = 16384            # drum reverse knob from here plays backwards
VOLUME_UNITY = 8192             # drum/synth volume knob for gain 1.0 (gain = (volume / 8192) ** 2)


def f32(x: float) -> float:
    """Round to IEEE single precision, as the engines' float maths does after every operation."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def q31(x: float) -> int:
    """float -> Q31 as `fcvtzs #31`: truncate toward zero, saturate."""
    v = math.trunc(x * 2147483648.0)
    return max(-0x80000000, min(0x7FFFFFFF, v))


def _ratio_to_rate(ratio16: float) -> int:
    """ratio / 16 clamped to [2**-8, 1.0], as a Q31 word."""
    r = ratio16
    if r > 1.0:
        r = 1.0
    elif not r >= 2.0 ** -8:
        r = 2.0 ** -8
    return q31(r)


def drum_cents(pitch: int, drum_version: int = 2) -> int:
    """Cents of a drum key's pitch knob: 4,800 cents per 24,576 units, rounded away from zero (`drumPitchToCents`).
    Packs with drum_version < 3 lose the low 9 bits first, so they move in whole semitones."""
    p = pitch & ~0x1FF if drum_version < 3 else pitch
    c = math.ceil(f32(f32(abs(p) * 4800.0) / 24576.0))
    return c if p >= 0 else -c


def drum_rate(pitch: int, drum_version: int = 2) -> int:
    """Slice +0x0C for a drum key: Q31 of 2**(cents/1200) / 16 (unity RATE_UNITY), clamped to +-4 octaves."""
    ratio = f32(2.0 ** (drum_cents(pitch, drum_version) / 1200.0))
    return _ratio_to_rate(f32(ratio * 0.0625))


def synth_rate(base_freq: float = BASE_FREQ) -> int:
    """Slice +0x0C for a synth sample: Q31 of 261.626 / (16 * base_freq): C4 recordings get RATE_UNITY."""
    return _ratio_to_rate(f32(f32(BASE_FREQ) / f32(f32(base_freq) * 16.0)))


def drum_knobs_word(volume: int, playmode: int, reverse: int) -> int:
    """Slice +0x08 for a drum key: volume (16 bits) | playmode >> 7 << 16 | reverse >> 7 << 24."""
    return (volume & 0xFFFF) | ((playmode >> 7) & 0xFF) << 16 | ((reverse >> 7) & 0xFF) << 24


def synth_frame(knob_q31: int) -> int:
    """A synth knob position (Q31) in frames: a truncating Q31 multiply by 264,600 (6.0 s)."""
    return (knob_q31 * smp.SYNTH_MAX_FRAMES) >> 31


def synth_slice_fields(knobs: list[int], frames: int, base_freq: float = BASE_FREQ) -> dict:
    """The one slice of a synth sample. knobs[0] start, [3] end, [1] << 16 | [5] << 1 loop start, [2] << 16 | [6] << 1
    loop end, [4] play mode (top byte of +0x08), [7] volume (low 16 bits); clamped start <= loop start <= loop end <=
    end <= frames; loop points count from the slice's first frame."""
    k = [int(v) & 0xFFFF for v in knobs]
    n = min(frames, smp.SYNTH_MAX_FRAMES)
    start = min(synth_frame(k[0] << 16), n)
    end = min(synth_frame(k[3] << 16), n)
    ls = min(max(start, synth_frame(k[1] << 16 | k[5] << 1)), end)
    le = min(max(ls, synth_frame(k[2] << 16 | k[6] << 1)), end)
    return {"start": start, "frames": end - start, "knobs": (k[7] & 0xFFFF) | ((k[4] >> 7) & 0xFF) << 24,
            "rate": synth_rate(base_freq), "loopStart": ls - start, "loopEnd": le - start, "pitch": 0}


def slices(pack: dict) -> list[dict]:
    """The slices the loader builds from an opz_samples.parse() result: start (frame in the file), frames, knobs,
    rate, loopStart, loopEnd, pitch, editable."""
    meta = pack.get("meta") or {}
    editable = int(bool(meta.get("editable")))
    if pack["kind"] == "plain":
        return [{"start": 0, "frames": min(pack["frames"], smp.SYNTH_MAX_FRAMES), "knobs": 0, "rate": 0,
                 "loopStart": 0, "loopEnd": 0, "pitch": 0, "editable": 0}]
    if pack["kind"] == "synth":
        s = synth_slice_fields(meta.get("knobs", [0] * 8), pack["frames"], float(meta.get("base_freq", BASE_FREQ)))
        s["editable"] = editable
        return [s]
    version = int(meta.get("drum_version", 2))

    def knob(key, i, default):
        v = meta.get(key, [])
        return int(v[i]) if i < len(v) else default
    out = []
    for i, (a, b) in enumerate(pack["keys"]):
        pitch = knob("pitch", i, smp.DEFAULT_PITCH)
        out.append({"start": a, "frames": b - a,
                    "knobs": drum_knobs_word(knob("volume", i, smp.DEFAULT_VOLUME),
                                             knob("playmode", i, smp.DEFAULT_PLAYMODE),
                                             knob("reverse", i, smp.DEFAULT_REVERSE)),
                    "rate": drum_rate(pitch, version), "loopStart": 0, "loopEnd": 0,
                    "pitch": pitch & ~0x1FF if version < 3 else pitch, "editable": editable})
    return out


# ---------------------------------------------------------------------------------------------------------------
# sampleplayergain: drum tracks
# ---------------------------------------------------------------------------------------------------------------

def semitone_step(i: int) -> int:
    """Entry i of the engine's 64-entry P1 table (`s_pitches`): 65536 * 2**((i - 32) / 12), i = 32 is unity.
    TE's entries match this to +-1."""
    return int(round(65536 * 2.0 ** ((i - 32) / 12.0)))


def p1_semitones(p1_word: int) -> int:
    """P1 (pitch) in semitones: byte 0..123 -> -32..-1 (4 values each), 124..132 -> 0 (the centre detent),
    133..255 -> +1..+31."""
    v = (p1_word >> 23) & 0xFF
    if v > 123:
        return 0 if v < 133 else ((v - 133) >> 2) + 1
    return ((v + 3) >> 2) - 32


def octave(note: int) -> int:
    """Octave of a drum note: floor((note + 19) / 24) - 3 (MIDI 53..76 = octave 0)."""
    return (note + 19) // 24 - 3


def drum_step(p1_word: int, rate: int, note: int) -> int:
    """The engine's per-note step (state +0x04 on the device): s_pitches[P1] * rate >> 27, low 4 bits cleared, shifted
    by the octave. 65536 = one frame per sample at the drum tracks' unity pitch."""
    s = (semitone_step(p1_semitones(p1_word) + 32) * rate >> 27) & ~0xF
    o = octave(note)
    return s << o if o >= 0 else s >> -o


DRUM_UNITY_PITCH = 1024         # render's pitch argument on drum tracks at standard tuning (1024 x tune >> 30)


def drum_increment_q10(step: int, pitch: int = DRUM_UNITY_PITCH) -> int:
    """Per-sample position increment in Q10 frames: step * pitch >> 16 (render, `add w9, w9, w11, asr #10` with
    w11 = step * pitch >> 6). So frames per sample = step / 65536 * pitch / 1024."""
    return (step * pitch) >> 16


def drum_speed(p1_word: int, pitch_knob: int, note: int, drum_version: int = 2,
               pitch: int = DRUM_UNITY_PITCH) -> float:
    """Frames of the sample per output sample for a key: 1.0 = original speed."""
    return drum_increment_q10(drum_step(p1_word, drum_rate(pitch_knob, drum_version), note), pitch) / 1024.0


NOTE_STYLES = ("retrig", "mono", "gate", "loop")


def playmode_style(playmode: int) -> str:
    """What a drum key's playmode knob selects: 3 * (playmode >> 7) < 256 gate, < 512 trigger, else loop."""
    m = 3 * ((playmode >> 7) & 0xFF)
    return "gate" if m < 256 else "trigger" if m < 512 else "loop"


def drum_style(note_style: int, editable: bool, playmode: int) -> str:
    """How a drum note plays. The track's note style (params[17] >> 29: 0 retrig, 1 mono, 2 gate, 3 loop) wins,
    except that an editable pack's playmode decides on tracks set to retrig."""
    if editable and note_style == 0:
        return {"gate": "gate", "trigger": "retrig", "loop": "loop"}[playmode_style(playmode)]
    return NOTE_STYLES[note_style & 3]


def drum_reversed(p2_word: int, reverse: int) -> bool:
    """Backwards when P2 is past its centre (> 0x40000000), or when the key's reverse knob is >= 16384."""
    return p2_word > 0x40000000 or ((reverse >> 7) & 0xFF) > 127


def volume_gain(volume: int) -> float:
    """Linear gain of the volume knob (both engines): Q31(volume << 16) squared, times 16 -> (volume / 8192) ** 2,
    saturating. 8192 = unity, 16384 = +12 dB."""
    return (volume / 8192.0) ** 2


def drum_regions(frames: int, start: int, attack: int, hold: int, decay: int) -> dict:
    """onNoteOn's playback points from the envelope words (params[2..5], shown as attack / decay / sustain /
    release): start frame, attack end, hold end (= loop end in loop style; the loop starts at attack end) and decay
    end. Mirrors SamplePlayerGain::onNoteOn."""
    q = lambda a, b: (a * b) >> 31                                       # noqa: E731
    first = min((frames - 512) & 0xFFFFFFFF, q(start, frames))           # unsigned: short keys start at 0..frames
    if start & ~0x7FFF and attack < 1 << 22:
        attack = 1 << 22                                                 # some attack once start is moved
    rest = frames - first
    a = q(rest >> 1, attack)
    rest -= a
    h = max(q(rest, hold), 512)
    d = q(min(min(decay, 1 << 30) << 1, 0x7FFFFFFF), rest - h)
    return {"start": first, "attack_end": min(first + a, frames - 192), "hold_end": min(first + a + h, frames - 64),
            "decay_end": first + a + h + d}


# ---------------------------------------------------------------------------------------------------------------
# synthsampler: synth tracks
# ---------------------------------------------------------------------------------------------------------------

def note_pitch(note: int, cents: float = 0.0) -> int:
    """The host's pitch for a synth note: Q32 phase increment at 44.1 kHz (engine-abi.md), A4 = MIDI 69 = 440 Hz."""
    return int(round(440.0 * 2.0 ** ((note - 69) / 12.0 + cents / 1200.0) * 2 ** 32 / smp.SAMPLE_RATE))


def synth_increment_q10(pitch: int, rate: int) -> int:
    """Per-sample increment in Q10 frames: step = pitch * rate >> 27 (low 4 bits cleared), increment = step *
    0x15122B6B >> 43, at least 1 (SynthSampler::render). A C4 recording played at C4 moves one frame per sample."""
    step = ((pitch * rate) >> 27) & ~0xF
    return max(1, (step * 0x15122B6B) >> 43) if step >= 24882 else 1


def synth_speed(note: int, base_freq: float = BASE_FREQ) -> float:
    return synth_increment_q10(note_pitch(note), synth_rate(base_freq)) / 1024.0


def synth_reversed(editable: bool, knobs_word: int) -> bool:
    """Backwards when the sample is editable and its play mode knob (knobs[4]) is >= 16384."""
    return bool(editable) and knobs_word >= 1 << 31


def synth_loops(loop_start: int, loop_end: int, frames: int) -> bool:
    """The loop runs while the note is held when loopStart < loopEnd < frames; note-off lets it play to the end."""
    return loop_start < loop_end < frames


# ---------------------------------------------------------------------------------------------------------------
# Knob values for kit builders
# ---------------------------------------------------------------------------------------------------------------

def pitch_knob(semitones: int) -> int:
    """Drum pitch knob for a transposition in semitones (drum_version 2 keeps whole semitones), +-48 max."""
    if not -48 <= semitones <= 48:
        raise ValueError("drum pitch is clamped to +-4 octaves")
    return max(-32768, min(32767, semitones * PITCH_PER_SEMITONE))


def playmode_knob(style: str) -> int:
    """A playmode value in the middle of each band."""
    return {"gate": 5440, "trigger": 16384, "loop": 27264}[style]


def volume_knob(gain: float) -> int:
    """Volume knob for a linear gain (1.0 = 8192)."""
    return max(0, min(32767, int(round(VOLUME_UNITY * math.sqrt(gain)))))


FIELDS = [
    ("+0x00 data", "first frame of the key / of the synth sample's start", "both: the start of playback"),
    ("+0x04 frames", "drum: end - start; synth: end - start (knobs[3] - knobs[0])", "both: playback stops (or loops) "
     "before the end; sampleplayergain keeps 64-192 frames of margin"),
    ("+0x08 bits 0-15", "drum: `volume`; synth: knobs[7]", "gain (volume / 8192)^2: 8192 unity, 16384 +12 dB"),
    ("+0x08 bits 16-23", "drum: `playmode` >> 7", "editable packs on retrig tracks only: < 11008 gate, < 21888 trigger "
     "(one-shot), else loop"),
    ("+0x08 bits 24-31", "drum: `reverse` >> 7; synth: knobs[4] >> 7", "drum: >= 16384 backwards (P2 past centre "
     "too); synth: >= 16384 backwards if editable"),
    ("+0x0C rate", "drum: 2^(cents/1200)/16 from `pitch` (512 per semitone); synth: 261.626/(16 base_freq)",
     "speed: 2^27 = unity; clamped to +-4 octaves"),
    ("+0x10 / +0x14", "synth: loop start / end in frames from +0x00 (knobs[1]<<16|knobs[5]<<1, knobs[2]<<16|knobs[6]<<1)",
     "synth: loops while held when start < end < frames"),
    ("+0x18 pitch", "drum: the `pitch` knob (low 9 bits cleared below drum_version 3)", "not read by TE's engines"),
    ("+0x1C editable", "the pack's `editable` flag (user samples made on the OP-Z)", "enables playmode (drum) and "
     "reverse (synth); sample mode edits these packs live"),
]


def table() -> str:
    lines = ["| Slice field | From the pack | What TE's engines do with it |", "|---|---|---|"]
    lines += ["| %s | %s | %s |" % f for f in FIELDS]
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    if argv[:1] == ["table"]:
        print(table())
        return 0
    if argv[:1] == ["slices"] and len(argv) == 2:
        with open(argv[1], "rb") as f:
            pack = smp.parse(f.read())
        for e in pack["errors"]:
            print("error: " + e)
        if pack["errors"]:
            return 1
        print("key  start    frames  volume playmode  reverse  rate        speed  loop")
        for i, s in enumerate(slices(pack)):
            k = s["knobs"]
            print("%3d %7d %8d %7d %-8s %-8s 0x%08x %6.3f  %s" % (
                i + 1, s["start"], s["frames"], k & 0xFFFF, playmode_style(((k >> 16) & 0xFF) << 7),
                "back" if (k >> 24) > 127 else "fwd", s["rate"], s["rate"] / RATE_UNITY,
                "%d-%d" % (s["loopStart"], s["loopEnd"]) if s["loopEnd"] else "-"))
        return 0
    print(__doc__.strip().split("\n\n")[2])
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
