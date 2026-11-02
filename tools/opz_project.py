#!/usr/bin/env python3
"""OP-Z projects and patterns: `projects/projectNN.opz`, the pattern bank and the project-global block.

Layout and evidence: research/patterns.md. In short (all little-endian):

    file      u32 major (73) | global 568 | bank 342,272 = 16 patterns x 21,392 | u32 minor (7)
              (saveProject, app 0x7a8b8, device 0x801117bc; files of version 71/72 have no minor word)
    pattern   0 track records 16 x 12 | 192 notes 880 x 8 (16 steps x 55 slots) | 7,232 steps 256 x 54
              | 21,056 sound 16 x 18 | 21,344 mute groups 10 x u32 | 21,384 tape mask u16 | 21,386 master mask u16
              | 21,388 active mute group | 21,389 3 bytes
    global    0 chains 16 x 32 (15 saved + the live one) | 512 levels drum, synth, punch, master | 516 tempo u16
              | 561 swing | 562 metronome level | 563 metronome sound | 564 selected chain (FF none)

The same 568 and 342,272 bytes travel live over StateSync as `$0C` and `$09`/`$0A` (third_party/opzsysex), and the
whole file over the normal-mode file service (download ids 3-12, write = reload). Pure functions and a small CLI;
nothing here talks to a device.

    python3 tools/opz_project.py info projects/project01.opz [--pattern 1]
    python3 tools/opz_project.py check FILE [FILE ...]
    python3 tools/opz_project.py diff A B
    python3 tools/opz_project.py empty OUT
    python3 tools/opz_project.py rhythm IN OUT --pattern 1 --track kick --hits 4 [--steps 16] [--note 53]
    python3 tools/opz_project.py vectors [--check]      # core/test-vectors/project.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "third_party", "opzsysex"))
from opzsysex.sysex import pattern_upload_frames  # noqa: E402

MAJOR = 73
MINOR = 7
KNOWN_MAJORS = (70, 71, 72, 73)          # loadProject reads patterns only for these (device 0x80112574)
GLOBAL_SIZE = 568
PATTERN_SIZE = 21_392
PATTERNS = 16
TRACKS = 16
STEPS = 16
BANK_SIZE = PATTERN_SIZE * PATTERNS      # 342,272
FILE_SIZE = 4 + GLOBAL_SIZE + BANK_SIZE + 4
OLD_FILE_SIZE = FILE_SIZE - 4            # 71/72 files written without the minor word
PROJECTS = 10                            # project01.opz ... project10.opz

TRACK_NAMES = ("kick", "snare", "perc", "sample", "bass", "lead", "arp", "chord", "fx1", "fx2", "tape", "master",
               "perform", "module", "lights", "motion")
NOTES_PER_STEP = (2, 2, 2, 2, 4, 4, 8, 4, 1, 1, 1, 4, 6, 6, 4, 4)
NOTE_OFFSETS = (0, 2, 4, 6, 8, 12, 16, 24, 28, 29, 30, 31, 35, 41, 47, 51)
SLOTS_PER_STEP = 55
PARAMS = ("p1", "p2", "attack", "decay", "sustain", "release", "fx1", "fx2", "filter", "resonance", "pan", "level",
          "portamento", "lfo_depth", "lfo_speed", "lfo_value", "lfo_shape", "note_style")
COMPONENTS = ("pulse", "spark", "jump", "velocity", "ramp_up", "ramp_down", "random", "glide", "multiply", "sweep",
              "pulse_hold", "tonality", "trigger_spark", "component_spark")
COMPONENT_DEFAULTS = (4, 2, 4, 5, 4, 4, 4, 4, 2, 2, 4, 4, 2, 2)   # PatternData::PatternData, app 0x3ee1c table
STEP_UNITS = 6200                        # note duration units per step
TICKS_PER_STEP = 24                      # microtiming raw = ticks * 8, ticks -12..+11

NOTES_AT = 192
STEPS_AT = 7_232
SOUND_AT = 21_056
MUTES_AT = 21_344
TAPE_AT = 21_384
MASTER_AT = 21_386
MUTE_GROUP_AT = 21_388
STEP_RECORD = 54
TRACK_RECORD = 12

CHAINS = 16                              # 15 saved chain slots + record 16, the live chain
CHAIN_RECORD = 32
CHAIN_MAX = 31                           # the firmware plays at most 31 entries
TEMPO_AT = 516
SWING_AT = 561
METRONOME_LEVEL_AT = 562
METRONOME_SOUND_AT = 563
SELECTED_CHAIN_AT = 564
LEVELS = ("drum", "synth", "punch", "master")

STEP_LENGTH_MAX = 10                     # loadProject resets 0 or > 10 to 1 (device 0x80112682)

# $09/$0A full-bank upload: 178-byte compressed chunks; the receiver keeps non-final chunks at (index * 178) & 0xFFFF
# (device 0x8011445c, app 0x2b818), so a compressed stream past 368 chunks would overwrite its own start.
CHUNK = 178
MAX_BANK_COMPRESSED = CHUNK * (0x10000 // CHUNK)   # 65,504


class FormatError(ValueError):
    pass


# --- defaults (the OS's constructors) -------------------------------------------------------------------------

def default_global() -> bytearray:
    """GlobalData::GlobalData (app 0x4784): chains FF (the constructor fills 16 of each 32 bytes; we fill all 32),
    levels 80 80 00 00, 120 BPM, swing 7F, metronome 100, no chain selected."""
    g = bytearray(GLOBAL_SIZE)
    g[0:CHAINS * CHAIN_RECORD] = b"\xff" * (CHAINS * CHAIN_RECORD)
    g[512:516] = bytes((0x80, 0x80, 0, 0))
    struct.pack_into("<H", g, TEMPO_AT, 120)
    g[SWING_AT] = 0x7F
    g[METRONOME_LEVEL_AT] = 100
    g[SELECTED_CHAIN_AT] = 0xFF
    return g


def default_pattern() -> bytearray:
    """PatternData::PatternData (app 0x3eda0): no plug, 16 steps, every note slot unused (FF, velocity 100), no
    locks or components (component values at their defaults), sounds 0, tape mask 00FF, master mask 00F0."""
    p = bytearray(PATTERN_SIZE)
    for t in range(TRACKS):
        p[t * TRACK_RECORD:(t + 1) * TRACK_RECORD] = bytes((0, 0, 0, 0, 16, 5, 1, 0, 0, 0x40, 60, 0))
    note = struct.pack("<iBBbB", 2560, 0xFF, 100, 0, 0)
    for n in range(STEPS * SLOTS_PER_STEP):
        p[NOTES_AT + n * 8:NOTES_AT + n * 8 + 8] = note
    for r in range(TRACKS * STEPS):
        at = STEPS_AT + r * STEP_RECORD
        p[at + 2:at + 2 + len(COMPONENT_DEFAULTS)] = bytes(COMPONENT_DEFAULTS)
    p[19_330:19_346] = bytes(16)        # the constructor clears these two words (lights track, step 0) last
    struct.pack_into("<HH", p, TAPE_AT, 0x00FF, 0x00F0)
    return p


# --- the project -----------------------------------------------------------------------------------------------

class Project:
    """A project file in memory: version, the 568-byte global block and the 342,272-byte bank, kept as raw bytes so
    that unknown fields survive. `to_bytes()` reproduces the parsed file exactly."""

    def __init__(self, glob: bytes | None = None, bank: bytes | None = None, major: int = MAJOR,
                 minor: int | None = MINOR):
        self.glob = bytearray(default_global() if glob is None else glob)
        self.bank = bytearray(bytes(default_pattern()) * PATTERNS if bank is None else bank)
        if len(self.glob) != GLOBAL_SIZE or len(self.bank) != BANK_SIZE:
            raise FormatError("global block must be %d bytes and the bank %d" % (GLOBAL_SIZE, BANK_SIZE))
        self.major = major
        self.minor = minor

    @classmethod
    def parse(cls, data: bytes) -> "Project":
        if len(data) not in (FILE_SIZE, OLD_FILE_SIZE):
            raise FormatError("a project file is %d bytes (%d before version 73), this one is %d"
                              % (FILE_SIZE, OLD_FILE_SIZE, len(data)))
        major = struct.unpack_from("<I", data, 0)[0]
        if major not in KNOWN_MAJORS:
            raise FormatError("unknown project version %d (the OS would load default global data and no patterns)"
                              % major)
        minor = struct.unpack_from("<I", data, 4 + GLOBAL_SIZE + BANK_SIZE)[0] if len(data) == FILE_SIZE else None
        if major == MAJOR and minor is None:
            raise FormatError("a version 73 file ends with its minor version word")
        return cls(data[4:4 + GLOBAL_SIZE], data[4 + GLOBAL_SIZE:4 + GLOBAL_SIZE + BANK_SIZE], major, minor)

    def to_bytes(self) -> bytes:
        tail = b"" if self.minor is None else struct.pack("<I", self.minor)
        return struct.pack("<I", self.major) + bytes(self.glob) + bytes(self.bank) + tail

    @property
    def version(self) -> str:
        return "%d.%d" % (self.major, self.minor or 0)

    def copy(self) -> "Project":
        return Project(self.glob, self.bank, self.major, self.minor)

    # pattern bank: reading ------------------------------------------------------------------------------------
    def pattern(self, p: int) -> bytes:
        _check(p=p)
        return bytes(self.bank[p * PATTERN_SIZE:(p + 1) * PATTERN_SIZE])

    def track(self, p: int, t: int) -> dict:
        _check(p=p, t=t)
        r = self.bank[p * PATTERN_SIZE + t * TRACK_RECORD:][:TRACK_RECORD]
        return {"plug": struct.unpack_from("<I", r, 0)[0], "step_count": r[4], "timing": r[5], "step_length": r[6],
                "quantize": r[7], "byte8": r[8], "note_length": r[9], "last_note": r[10], "byte11": r[11]}

    def notes(self, p: int, t: int, s: int) -> list[dict]:
        """The used note slots of one step (note byte not FF), in slot order."""
        out = []
        for slot in range(NOTES_PER_STEP[_check(p=p, t=t, s=s)]):
            duration, note, velocity, micro, age = struct.unpack_from("<iBBbB", self.bank, _note_at(p, t, s, slot))
            if note != 0xFF:
                out.append({"note": note, "velocity": velocity, "duration": duration, "micro": micro, "age": age})
        return out

    def step(self, p: int, t: int, s: int) -> dict:
        """Enabled step components {name: value} and parameter locks {param: value} of one step."""
        at = _step_at(p, t, s)
        mask = struct.unpack_from("<H", self.bank, at)[0]
        comps = {COMPONENTS[i]: self.bank[at + 2 + i] for i in range(len(COMPONENTS)) if mask >> i & 1}
        locks = {PARAMS[i]: self.bank[at + 18 + i] for i in range(len(PARAMS)) if self.bank[at + 36 + i]}
        return {"mask": mask, "components": comps, "locks": locks}

    def sound(self, p: int, t: int) -> list[int]:
        _check(p=p, t=t)
        at = p * PATTERN_SIZE + SOUND_AT + t * len(PARAMS)
        return list(self.bank[at:at + len(PARAMS)])

    def mute_group(self, p: int) -> int:
        _check(p=p)
        return self.bank[p * PATTERN_SIZE + MUTE_GROUP_AT]

    def muted(self, p: int, t: int, group: int | None = None) -> bool:
        """Muted in `group` (default: the pattern's active mute group); 2 bits per track, the upper one is mute."""
        _check(p=p, t=t)
        g = min(self.mute_group(p) if group is None else group, 9)
        return bool(self.bank[p * PATTERN_SIZE + MUTES_AT + g * 4 + t // 4] >> ((t % 4) * 2 + 1) & 1)

    def routing(self, p: int) -> tuple[int, int]:
        _check(p=p)
        return struct.unpack_from("<HH", self.bank, p * PATTERN_SIZE + TAPE_AT)

    def pattern_has_notes(self, p: int) -> bool:
        base = p * PATTERN_SIZE + NOTES_AT
        return any(self.bank[base + n * 8 + 4] != 0xFF for n in range(STEPS * SLOTS_PER_STEP))

    # global block ---------------------------------------------------------------------------------------------
    def chain(self, slot: int) -> list[int]:
        """Chain record `slot` (0-14 saved, 15 = the live chain), zero-based patterns up to the first FF."""
        if not 0 <= slot < CHAINS:
            raise FormatError("chain slot must be 0..15")
        rec = bytes(self.glob[slot * CHAIN_RECORD:(slot + 1) * CHAIN_RECORD])
        return list(rec[:rec.index(0xFF)] if 0xFF in rec else rec)

    @property
    def tempo(self) -> int:
        return struct.unpack_from("<H", self.glob, TEMPO_AT)[0]

    def summary(self) -> dict:
        g = self.glob
        return {"version": self.version, "tempo": self.tempo, "swing": g[SWING_AT],
                "levels": dict(zip(LEVELS, g[512:516])), "metronome": [g[METRONOME_LEVEL_AT], g[METRONOME_SOUND_AT]],
                "selected_chain": None if g[SELECTED_CHAIN_AT] == 0xFF else g[SELECTED_CHAIN_AT],
                "chains": {n: self.chain(n) for n in range(CHAINS) if self.chain(n)},
                "patterns_with_notes": [p for p in range(PATTERNS) if self.pattern_has_notes(p)]}

    # pattern bank: writing ------------------------------------------------------------------------------------
    def set_notes(self, p: int, t: int, s: int, notes: list[dict]) -> None:
        """Replace a step's notes. Each note: note 0-127, velocity 1-127, duration > 0 (6200 per step), micro
        ticks -12..+11 (stored x 8), age 0-255. Unused slots get note FF and keep their other bytes (as opzsysex)."""
        if len(notes) > NOTES_PER_STEP[_check(p=p, t=t, s=s)]:
            raise FormatError("%s holds at most %d notes per step" % (TRACK_NAMES[t], NOTES_PER_STEP[t]))
        for n in notes:
            micro = n.get("micro", 0)
            if not 0 <= n["note"] <= 127 or not 1 <= n.get("velocity", 100) <= 127 or n.get("duration", STEP_UNITS) <= 0 \
                    or not -12 <= micro <= 11 or not 0 <= n.get("age", 0) <= 255:
                raise FormatError("note out of range: %r" % (n,))
        for slot in range(NOTES_PER_STEP[t]):
            at = _note_at(p, t, s, slot)
            if slot < len(notes):
                n = notes[slot]
                struct.pack_into("<iBBbB", self.bank, at, n.get("duration", STEP_UNITS), n["note"],
                                 n.get("velocity", 100), n.get("micro", 0) * 8, n.get("age", 0))
            else:
                self.bank[at + 4] = 0xFF

    def set_component(self, p: int, t: int, s: int, name: str, value: int | None) -> None:
        """Enable a step component with a raw value (1-10 = keys 1-0), or disable it (None; the value stays)."""
        at = _step_at(p, t, s)
        i = COMPONENTS.index(name)
        mask = struct.unpack_from("<H", self.bank, at)[0]
        if value is None:
            mask &= ~(1 << i)
        else:
            _byte(value, "component value")
            mask |= 1 << i
            self.bank[at + 2 + i] = value
        struct.pack_into("<H", self.bank, at, mask)

    def set_lock(self, p: int, t: int, s: int, param: str, value: int | None) -> None:
        """Lock a sound parameter on a step (enable byte FF), or clear the lock (None; the value stays)."""
        at = _step_at(p, t, s)
        i = PARAMS.index(param)
        if value is None:
            self.bank[at + 36 + i] = 0
        else:
            self.bank[at + 18 + i] = _byte(value, "lock value")
            self.bank[at + 36 + i] = 0xFF

    def set_sound(self, p: int, t: int, values) -> None:
        values = [_byte(v, "sound value") for v in values]
        if len(values) != len(PARAMS):
            raise FormatError("a sound is 18 values")
        _check(p=p, t=t)
        at = p * PATTERN_SIZE + SOUND_AT + t * len(PARAMS)
        self.bank[at:at + len(PARAMS)] = bytes(values)

    def set_track(self, p: int, t: int, *, step_count: int | None = None, step_length: int | None = None,
                  plug: int | None = None, note_length: int | None = None, quantize: int | None = None) -> None:
        _check(p=p, t=t)
        at = p * PATTERN_SIZE + t * TRACK_RECORD
        if step_count is not None:
            if not 1 <= step_count <= STEPS:
                raise FormatError("step count must be 1..16")
            self.bank[at + 4] = step_count
        if step_length is not None:
            if not 1 <= step_length <= STEP_LENGTH_MAX:
                raise FormatError("step length must be 1..%d" % STEP_LENGTH_MAX)
            self.bank[at + 6] = step_length
        if plug is not None:
            if not 0 < plug <= 0xFFFFFFFF:
                raise FormatError("plug id must be a non-zero u32")
            struct.pack_into("<I", self.bank, at, plug)
        if note_length is not None:
            self.bank[at + 9] = _byte(note_length, "note length")
        if quantize is not None:
            self.bank[at + 7] = _byte(quantize, "quantize")

    def set_muted(self, p: int, t: int, muted: bool, group: int | None = None) -> None:
        _check(p=p, t=t)
        g = min(self.mute_group(p) if group is None else group, 9)
        at, bit = p * PATTERN_SIZE + MUTES_AT + g * 4 + t // 4, 1 << ((t % 4) * 2 + 1)
        self.bank[at] = self.bank[at] | bit if muted else self.bank[at] & ~bit

    def clear_track(self, p: int, t: int) -> None:
        """Remove every note, component and lock of one track in one pattern (settings and sound stay)."""
        for s in range(STEPS):
            self.set_notes(p, t, s, [])
            at = _step_at(p, t, s)
            self.bank[at:at + 2] = b"\0\0"
            self.bank[at + 36:at + 54] = bytes(18)

    def copy_pattern(self, src: int, dst: int) -> None:
        _check(p=src)
        _check(p=dst)
        self.bank[dst * PATTERN_SIZE:(dst + 1) * PATTERN_SIZE] = self.pattern(src)

    def set_chain(self, slot: int, patterns: list[int]) -> None:
        """Saved chain slot 0-14 (the device shows 1-15), up to 31 zero-based patterns; [] clears it."""
        if not 0 <= slot < CHAINS - 1:
            raise FormatError("saved chain slot must be 0..14 (record 15 is the live chain)")
        if len(patterns) > CHAIN_MAX or any(not 0 <= v < PATTERNS for v in patterns):
            raise FormatError("a chain holds up to %d patterns 0..15" % CHAIN_MAX)
        self.glob[slot * CHAIN_RECORD:(slot + 1) * CHAIN_RECORD] = bytes(patterns) + b"\xff" * (CHAIN_RECORD - len(patterns))

    def set_tempo(self, bpm: int) -> None:
        if not 40 <= bpm <= 200:
            raise FormatError("tempo must be 40..200 BPM")
        struct.pack_into("<H", self.glob, TEMPO_AT, bpm)

    # checks ---------------------------------------------------------------------------------------------------
    def problems(self) -> list[str]:
        """What would make this file unsafe to hand to the OP-Z: anything outside what the OS writes itself.
        Empty list = writable. The OS checks almost none of this (research/patterns.md, *Malformed files*)."""
        out = []
        if (self.major, self.minor) != (MAJOR, MINOR):
            out.append("version %s: only %d.%d files are written (older ones load through the OS's upgrade path)"
                       % (self.version, MAJOR, MINOR))
        for p in range(PATTERNS):
            base = p * PATTERN_SIZE
            for t in range(TRACKS):
                tr = self.track(p, t)
                if not 1 <= tr["step_count"] <= STEPS:
                    out.append("pattern %d %s: step count %d" % (p + 1, TRACK_NAMES[t], tr["step_count"]))
                if not 1 <= tr["step_length"] <= STEP_LENGTH_MAX:
                    out.append("pattern %d %s: step length %d" % (p + 1, TRACK_NAMES[t], tr["step_length"]))
                for s in range(STEPS):
                    for slot in range(NOTES_PER_STEP[t]):
                        # durations are not checked: the OS itself stores some as -256 (meaning open)
                        _dur, note, velocity, micro, _age = struct.unpack_from("<iBBbB", self.bank,
                                                                              _note_at(p, t, s, slot))
                        if note == 0xFF:
                            continue
                        if note > 127 or velocity > 127 or micro % 8 or not -96 <= micro <= 88:
                            out.append("pattern %d %s step %d: note %d velocity %d micro %d"
                                       % (p + 1, TRACK_NAMES[t], s + 1, note, velocity, micro))
                    at = _step_at(p, t, s)
                    if struct.unpack_from("<H", self.bank, at)[0] & 0xC000:
                        out.append("pattern %d %s step %d: reserved component bits" % (p + 1, TRACK_NAMES[t], s + 1))
                    if any(b not in (0, 0xFF) for b in self.bank[at + 36:at + 54]):
                        out.append("pattern %d %s step %d: lock enable not 00/FF" % (p + 1, TRACK_NAMES[t], s + 1))
            if self.bank[base + MUTE_GROUP_AT] > 9:
                out.append("pattern %d: mute group %d" % (p + 1, self.bank[base + MUTE_GROUP_AT]))
        for n in range(CHAINS):
            rec = self.glob[n * CHAIN_RECORD:(n + 1) * CHAIN_RECORD]
            chain = self.chain(n)
            if any(v >= PATTERNS for v in chain) or len(chain) > CHAIN_MAX:
                out.append("chain %d: %s" % (n + 1, bytes(rec).hex()))
        if not 40 <= self.tempo <= 200:
            out.append("tempo %d" % self.tempo)
        sel = self.glob[SELECTED_CHAIN_AT]
        if sel != 0xFF and sel >= CHAINS - 1:
            out.append("selected chain %d" % sel)
        return out


def _check(p: int = 0, t: int = 0, s: int = 0) -> int:
    if not (0 <= p < PATTERNS and 0 <= t < TRACKS and 0 <= s < STEPS):
        raise FormatError("pattern, track and step are 0..15")
    return t


def _byte(value: int, what: str) -> int:
    if not 0 <= value <= 255:
        raise FormatError("%s must be 0..255" % what)
    return value


def _note_at(p: int, t: int, s: int, slot: int) -> int:
    return p * PATTERN_SIZE + NOTES_AT + (s * SLOTS_PER_STEP + NOTE_OFFSETS[t] + slot) * 8


def _step_at(p: int, t: int, s: int) -> int:
    _check(p=p, t=t, s=s)
    return p * PATTERN_SIZE + STEPS_AT + (t * STEPS + s) * STEP_RECORD


# --- generators (F-05, F-18) -----------------------------------------------------------------------------------

def euclid(hits: int, steps: int = 16, rotate: int = 0) -> list[bool]:
    """`hits` onsets spread as evenly as possible over `steps` (Bresenham form of Bjorklund's rhythm: E(3,8) is
    x..x..x.), rotated right by `rotate` steps."""
    if not 0 < steps <= 64 or not 0 <= hits <= steps:
        raise FormatError("need 0 <= hits <= steps <= 64")
    base = [(i * hits) % steps < hits for i in range(steps)]
    r = rotate % steps
    return base[-r:] + base[:-r] if r else base


def write_rhythm(project: Project, p: int, t: int, hits: list[bool], note: int = 60, velocity: int = 100,
                 length: float = 1.0) -> None:
    """Replace one track's notes in one pattern with `hits` (one entry per step, up to 16); sets the step count."""
    if not 1 <= len(hits) <= STEPS:
        raise FormatError("a pattern track holds 1..16 steps")
    for s in range(STEPS):
        on = s < len(hits) and hits[s]
        project.set_notes(p, t, s, [{"note": note, "velocity": velocity, "duration": round(length * STEP_UNITS)}]
                          if on else [])
    project.set_track(p, t, step_count=len(hits))


def write_pages(project: Project, t: int, steps: list[list[dict]], first: int, chain_slot: int) -> list[int]:
    """F-18: a track of up to 64 steps (each a list of notes) as consecutive 16-step patterns `first`, `first`+1, ...
    plus saved chain `chain_slot` over them. Every page starts as a copy of pattern `first`, so the other tracks
    repeat their 16 steps on each page. Returns the patterns written."""
    pages = (len(steps) + STEPS - 1) // STEPS
    if not 0 < len(steps) <= 4 * STEPS or first + pages > PATTERNS:
        raise FormatError("1..64 steps, and the pages must fit in patterns %d..16" % (first + 1))
    used = list(range(first, first + pages))
    for n, p in enumerate(used):
        if p != first:
            project.copy_pattern(first, p)
        page = steps[n * STEPS:(n + 1) * STEPS]
        for s in range(STEPS):
            project.set_notes(p, t, s, page[s] if s < len(page) else [])
        project.set_track(p, t, step_count=len(page))
    project.set_chain(chain_slot, used)
    return used


# --- transport guards (offline) --------------------------------------------------------------------------------

def project_path(number: int) -> str:
    """Device path of project `number` 1-10. The file service reloads any written path containing
    "projects/project" into RAM slot number-1 with no bounds check (device 0x8012824a), so 00 or 11+ would write
    outside the ten projects: only these ten names are ever produced."""
    if not 1 <= number <= PROJECTS:
        raise FormatError("projects are numbered 1..%d" % PROJECTS)
    return "projects/project%02d.opz" % number


def project_file_id(number: int) -> int:
    """File-service download id of project `number` 1-10: 3..12 (device file server, 0x8012bde8)."""
    project_path(number)
    return number + 2


def bank_upload_frames(prefix: bytes, project: int, transfer_id: int) -> list[bytes]:
    """`$09`/`$0A` frames that replace patterns 1..N of project `project` (0-9) with `prefix` (N x 21,392 bytes).
    The receiver multiplies address >> 4 by the bank size with no bounds check (device 0x801141a8), and keeps
    compressed chunks at a 16-bit offset, so both are checked here."""
    if not 0 <= project < PROJECTS:
        raise FormatError("project index must be 0..9 (the receiver does not check it)")
    if not prefix or len(prefix) % PATTERN_SIZE or len(prefix) > BANK_SIZE:
        raise FormatError("send whole patterns: 1..16 x %d bytes" % PATTERN_SIZE)
    if len(zlib.compress(prefix)) > MAX_BANK_COMPRESSED:
        raise FormatError("compressed bank over %d bytes: the receiver's 16-bit chunk offset would wrap"
                          % MAX_BANK_COMPRESSED)
    return pattern_upload_frames(prefix, project << 4, transfer_id)


# --- shared test vectors ---------------------------------------------------------------------------------------

VECTORS = os.path.join(ROOT, "core", "test-vectors", "project.json")


def vector_project() -> Project:
    """The synthetic project behind core/test-vectors/project.json: every kind of edit once."""
    pr = Project()
    pr.set_tempo(97)
    write_rhythm(pr, 0, 0, euclid(5, 16), note=53, velocity=110)
    write_rhythm(pr, 0, 1, euclid(3, 8, 2), note=55, length=0.5)
    pr.set_notes(0, 6, 3, [{"note": n, "velocity": 64 + n % 7, "duration": 3100, "micro": m}
                           for n, m in zip((60, 64, 67, 71, 74, 77, 81, 84), (-12, -4, 0, 3, 5, 8, 10, 11))])
    pr.set_component(0, 4, 2, "glide", 7)
    pr.set_component(0, 4, 2, "pulse", 4)
    pr.set_component(0, 4, 2, "pulse", None)
    pr.set_lock(0, 5, 0, "filter", 200)
    pr.set_lock(0, 5, 1, "filter", 10)
    pr.set_lock(0, 5, 1, "filter", None)
    pr.set_sound(0, 5, list(range(100, 118)))
    pr.set_track(0, 4, step_count=12, step_length=2, plug=61016, note_length=32, quantize=255)
    pr.set_muted(0, 3, True)
    pr.set_muted(0, 7, True, group=2)
    write_pages(pr, 4, [[{"note": 36 + s % 12}] if s % 3 == 0 else [] for s in range(40)], first=8, chain_slot=4)
    pr.set_chain(0, [0, 0, 1, 15])
    return pr


def vectors() -> dict:
    pr = vector_project()
    data = pr.to_bytes()
    reads = {"track": [pr.track(p, t) for p, t in ((0, 0), (0, 4), (9, 4), (10, 4), (1, 15))],
             "notes": [pr.notes(p, t, s) for p, t, s in ((0, 0, 0), (0, 0, 1), (0, 1, 2), (0, 6, 3), (9, 4, 3))],
             "step": [pr.step(0, 4, 2), pr.step(0, 5, 0), pr.step(0, 5, 1), pr.step(3, 3, 3)],
             "sound": [pr.sound(0, 5), pr.sound(2, 0)],
             "muted": [pr.muted(0, 3), pr.muted(0, 7), pr.muted(0, 7, group=2)],
             "routing": list(pr.routing(0)),
             "summary": pr.summary()}
    return {"format": 1, "name": "project", "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "default_pattern_sha256": hashlib.sha256(bytes(default_pattern())).hexdigest(),
            "default_global_sha256": hashlib.sha256(bytes(default_global())).hexdigest(),
            "euclid": {"%d,%d,%d" % k: [int(x) for x in euclid(*k)] for k in ((3, 8, 0), (5, 16, 0), (3, 8, 2),
                                                                                (0, 4, 0), (7, 7, 1), (5, 12, 13))},
            "reads": reads, "problems": pr.problems()}


def _dump(doc: dict) -> str:
    return json.dumps(doc, indent=1, sort_keys=True) + "\n"


# --- CLI ---------------------------------------------------------------------------------------------------------

def _load(path: str) -> Project:
    with open(path, "rb") as f:
        return Project.parse(f.read())


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("info", help="tempo, chains, and per-track settings and notes of one pattern")
    a.add_argument("file")
    a.add_argument("--pattern", type=int, help="1-16: list its tracks and notes")
    a = sub.add_parser("check", help="list anything the OP-Z itself would not write")
    a.add_argument("files", nargs="+")
    a = sub.add_parser("diff", help="which patterns, tracks and global fields differ")
    a.add_argument("a")
    a.add_argument("b")
    a = sub.add_parser("empty", help="write a project file as the OS's constructors make it (no plugs)")
    a.add_argument("out")
    a = sub.add_parser("rhythm", help="copy IN to OUT with a Euclidean rhythm on one track of one pattern")
    a.add_argument("infile")
    a.add_argument("out")
    a.add_argument("--pattern", type=int, required=True, help="1-16")
    a.add_argument("--track", required=True, choices=TRACK_NAMES)
    a.add_argument("--hits", type=int, required=True)
    a.add_argument("--steps", type=int, default=16)
    a.add_argument("--rotate", type=int, default=0)
    a.add_argument("--note", type=int, default=53, help="MIDI note (drum keys 53-76)")
    a.add_argument("--velocity", type=int, default=100)
    a = sub.add_parser("vectors", help="write core/test-vectors/project.json")
    a.add_argument("--check", action="store_true", help="exit 1 if the committed file is stale")
    args = ap.parse_args(argv)

    if args.cmd == "info":
        pr = _load(args.file)
        print(json.dumps(pr.summary(), indent=1))
        if args.pattern:
            p = args.pattern - 1
            for t in range(TRACKS):
                tr = pr.track(p, t)
                steps = [(s + 1, [n["note"] for n in pr.notes(p, t, s)]) for s in range(STEPS) if pr.notes(p, t, s)]
                print("%-8s plug %-10d steps %2d x %d  %s" % (TRACK_NAMES[t], tr["plug"], tr["step_count"],
                                                              tr["step_length"], steps))
        return 0
    if args.cmd == "check":
        bad = 0
        for path in args.files:
            try:
                problems = _load(path).problems()
            except FormatError as e:
                problems = [str(e)]
            print("%s: %s" % (path, "ok" if not problems else "%d problem(s)" % len(problems)))
            for line in problems[:20]:
                print("  " + line)
            bad += bool(problems)
        return 1 if bad else 0
    if args.cmd == "diff":
        x, y = _load(args.a), _load(args.b)
        for p in range(PATTERNS):
            if x.pattern(p) != y.pattern(p):
                tracks = [TRACK_NAMES[t] for t in range(TRACKS)
                          if any(x.notes(p, t, s) != y.notes(p, t, s) or x.step(p, t, s) != y.step(p, t, s)
                                 for s in range(STEPS)) or x.track(p, t) != y.track(p, t) or x.sound(p, t) != y.sound(p, t)]
                print("pattern %d differs: %s" % (p + 1, ", ".join(tracks) or "mutes/routing/other bytes"))
        sx, sy = x.summary(), y.summary()
        for k in sx:
            if k != "patterns_with_notes" and sx[k] != sy[k]:
                print("%s: %s -> %s" % (k, sx[k], sy[k]))
        if x.glob != y.glob and sx == sy:
            print("global block differs in unnamed bytes")
        return 0
    if args.cmd == "empty":
        with open(args.out, "wb") as f:
            f.write(Project().to_bytes())
        return 0
    if args.cmd == "rhythm":
        pr = _load(args.infile)
        if (pr.major, pr.minor) != (MAJOR, MINOR):
            raise SystemExit("%s is version %s; only %d.%d files are edited" % (args.infile, pr.version, MAJOR, MINOR))
        write_rhythm(pr, args.pattern - 1, TRACK_NAMES.index(args.track), euclid(args.hits, args.steps, args.rotate),
                     args.note, args.velocity)
        with open(args.out, "wb") as f:
            f.write(pr.to_bytes())
        print("wrote %s: pattern %d %s %s; %d problem(s)" % (args.out, args.pattern, args.track, "".join(
            "x" if h else "." for h in euclid(args.hits, args.steps, args.rotate)), len(pr.problems())))
        return 0
    if args.cmd == "vectors":
        text = _dump(vectors())
        if args.check:
            with open(VECTORS) as f:
                if f.read() != text:
                    print("%s is stale: run tools/opz_project.py vectors" % VECTORS)
                    return 1
            return 0
        with open(VECTORS, "w") as f:
            f.write(text)
        print("wrote %s" % VECTORS)
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
