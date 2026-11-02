#!/usr/bin/env python3
"""opz_bench — automated device test of one engine: play a test script on the OP-Z, record its USB audio, measure it.

Usage (also as `opz.py test ...`):
    opz_bench.py ENGINE_DIR --track lead [--script auto|default|PATH] [--record DIR | --no-record] [--dry-run]
                 [--engine-confirmed] [--yes] [--no-slot-check] [--source lead] [--channel lead=6 ...]
                 [--audio-device OP-Z] [--sim] [--no-stop-on-silence] [--port OP-Z] [--fake]
    opz_bench.py --analyze RUN_DIR [--sim]          re-measure a recorded run offline (run.json + recording.wav)
    opz_bench.py ENGINE_DIR --track lead --scn OUT.scn   the script as an opzsim scenario (offline)

What a run does (docs/roadmap/device-tests.md):
 1. Builds the plan from the script (engines/<name>/tests/device.json, or the default one generated from plug.json
    and presets.json): each segment sets sounds through `$0E` (a track's 18 sound bytes in the active pattern) and
    plays MIDI notes (track N on MIDI channel N + 1, the OP-Z's default map). `$0E` sends are never closer than
    40 ms (TE's app and the OP-Z send at most every 31 ms). `--dry-run` prints the plan and touches nothing.
 2. Pre-flight: the MIDI ports, the slot map read back (read-only) to say which black key selects the engine on the
    track, the audio capture (ffmpeg avfoundation, device found by name), the user's confirmation that the engine
    is the active sound (state sync cannot tell; the answer goes into the report), then one confirmation for the run.
 3. Plays the plan in a StateSync session that opens with the OP-Z's own sounds (the restore points), watching for
    a crash: the session's `$01` replies stop, the MIDI ports vanish, the audio stream ends, or (unless
    --no-stop-on-silence) no audio for 3 s while notes play. On a crash it stops at once and reports.
 4. Puts every touched track's sound back (the values the OP-Z sent at the start, as `presets audition` does).
 5. Measures each segment (tools/opz_audio.py: level, clipping, silence, dropouts, clicks, pitch in cents, tails)
    and writes RUN_DIR/report.md with a table, a WAV snippet per segment and the README's listening checklist;
    run.json keeps everything for `--analyze`. `--sim` adds opzsim renders of the same script for comparison.

Never sends: CCs, program changes, file-service pushes, update-mode commands. Never writes the slot map, plugs or
presets (a guard refuses any other SysEx than `$00`, `$0E`, the read-only bank reads `$0B`/`$08` and the read-only
file-service requests). `$0E` changes
the active pattern: run it in a spare project. One client at a time: close TE's app and the web app first.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime
import json
import math
import os
import random
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import opz_audio as au  # noqa: E402
from opz import (PARAM_NAMES, PLUGS, ROOT, SLOTS, TRACK_NAMES, TRACKS, PlanError, check_sound,  # noqa: E402
                 format_sound, load_manifest, preset_slot, set_command, slot_entries, track_index)
from opz_session import FileSession, MidoTransport, SessionError, StateSession  # noqa: E402
from opzsysex.sysex import ProtocolError, decode_frame  # noqa: E402

VOICES = (2, 2, 2, 2, 2, 3, 2, 4, 1, 1, 1, 1)     # opzsim / app voicePolyphony; fx tracks: one note at a time
MIN_SOUND_INTERVAL = 0.04
SWEEP_STEP = 0.05
SETTLE = 0.1                 # a segment's sound is sent this long before its first event
GAP = 0.3
LEAD_IN = 0.3
ACK_TIMEOUT = 3.5            # hellos go out every second; three missed replies = session lost
AUDIO_TIMEOUT = 1.5
HANG_WINDOW = 3.0
WATCH_EVERY = 0.25
PORTS_EVERY = 1.0
DRUM_KEYS = range(53, 77)
SNIPPET_MAX = 6.0
PITCH_TOLERANCE = 30.0       # cents from the nearest octave of the note (octave shifts are reported apart)
PARAMS = {n.replace(" ", "_"): i for i, n in enumerate(PARAM_NAMES)} | {"p1": 0, "p2": 1}
# Messages the runner may send: hello and sound for the run, the bank reads that give the restore points ($0B acks a
# bank part, $08 asks for the bank again: both only move the OP-Z's send cursor, TE's StateSync app 0x29bd0/0x29bdc),
# and the read-only file-service requests of the slot check ($51 advertise, $52 with sync-active 0, $35 download
# request, $55 close). $52 with 1 starts a push.
ALLOWED_SYSEX = {0x00, 0x0E, 0x0B, 0x08, 0x51, 0x52, 0x35, 0x55}


class Abort(RuntimeError):
    pass


# ---------------------------------------------------------------------------------------------------------------
# Safety: a transport wrapper that refuses anything but notes, hellos, sounds, bank reads and read-only file requests
# ---------------------------------------------------------------------------------------------------------------

class Guard:
    def __init__(self, transport):
        self.t = transport
        self.sent: list[tuple[str, int]] = []

    def send(self, raw: bytes) -> None:
        try:
            f = decode_frame(raw)
        except ProtocolError as e:
            raise PlanError("refusing to send a malformed SysEx frame") from e
        if f.message_id not in ALLOWED_SYSEX or (f.message_id == 0x52 and len(f.payload) >= 3 and f.payload[2]):
            raise PlanError("refusing to send SysEx $%02X: the test runner never writes to the OP-Z's files"
                            % f.message_id)
        self.sent.append(("sysex", f.message_id))
        self.t.send(raw)

    def send_short(self, data: bytes) -> None:
        if data[0] & 0xF0 not in (0x80, 0x90):
            raise PlanError("refusing to send MIDI status %02X: only note on/off" % data[0])
        self.sent.append(("note", data[0]))
        self.t.send_short(data)

    def receive(self, timeout: float):
        return self.t.receive(timeout)

    def present(self) -> bool:
        return getattr(self.t, "present", lambda: True)()


# ---------------------------------------------------------------------------------------------------------------
# Scripts
# ---------------------------------------------------------------------------------------------------------------

NOTE_RE = re.compile(r"^([A-Ga-g])([#b]?)(-?\d+)$")
SEMITONES = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}


def parse_note(value) -> int:
    """MIDI note from a number or a name: C4 = 60, A#3, Eb2, C-1 = 0 (as opzsim)."""
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    m = NOTE_RE.match(str(value))
    if not m:
        raise PlanError("bad note %r (a MIDI number or a name like C4, A#3, Eb2)" % (value,))
    n = SEMITONES[m.group(1).lower()] + {"#": 1, "b": -1, "": 0}[m.group(2)] + 12 * (int(m.group(3)) + 1)
    return n


def note_name(n: int) -> str:
    return "%s%d" % (("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")[n % 12], n // 12 - 1)


def param_index(name) -> int:
    if isinstance(name, int):
        return name
    key = str(name).lower().replace(" ", "_")
    if key not in PARAMS:
        raise PlanError("unknown parameter %r (names: %s)" % (name, ", ".join(sorted(PARAMS))))
    return PARAMS[key]


def kind_of(track: int) -> str:
    return "drum" if track < 4 else "synth" if track < 8 else "fx"


RELEASE_POINTS = ((0, 0.3), (40, 0.9), (90, 2.0), (128, 3.0), (200, 10.0), (255, 20.0))


def release_tail(byte: int) -> float:
    """Rough seconds to −66 dB for a release byte (opzsim README: 40 ≈ 0.9 s, 90 ≈ 2 s, 128 ≈ 3 s, 200 ≈ 10 s)."""
    for (b0, s0), (b1, s1) in zip(RELEASE_POINTS, RELEASE_POINTS[1:]):
        if byte <= b1:
            return s0 + (s1 - s0) * (byte - b0) / (b1 - b0)
    return RELEASE_POINTS[-1][1]


def slot_presets(engine: dict, track: int) -> list[dict]:
    slot = preset_slot(track)
    ps = [p for p in engine["presets"] if int(p.get("slot", 0)) == slot]
    if track >= 8:
        ps = [p for p in ps if int(p.get("index", 0)) == 0]
    return sorted(ps, key=lambda p: int(p.get("index", 0)))


def octave_of(p: dict) -> int:
    return int(p.get("defaultOctave", p.get("default_octave", 0)))


def default_script(engine: dict, track: int) -> dict:
    """The generated script: presets, range, velocity, P1/P2 sweeps, chords and voice stealing, fast P1+P2 moves
    while chords play (the macro-crash situation) and a release tail; drums play every key 53–76; effects get a
    synth track's send (`source`) as their input and notes of their own."""
    kind = kind_of(track)
    presets = slot_presets(engine, track)
    base = presets[0] if presets else None
    test = {"preset": base["name"]} if base else {}
    oc = octave_of(base) if base else 0
    poly = VOICES[track]
    segs = [{"name": "silence", **test, "length": 1.0, "check": {"silence": True}}]
    if kind == "fx":
        return {"description": "default effect script (generated)", "segments": segs + _fx_segments(presets)}
    if kind == "drum":
        segs.append({"name": "align", **test, "set": {"attack": 0}, "notes": [{"at": 0, "note": 53, "len": 0.1}],
                     "length": 0.6})
        for p in presets:
            segs.append({"name": "preset %s" % p["name"], "preset": p["name"],
                         "notes": [{"at": 0.4 * i, "note": k, "len": 0.15} for i, k in enumerate((53, 60, 67, 76))],
                         "length": 2.0})
        for k in DRUM_KEYS:
            segs.append({"name": "key %d %s" % (k - 52, note_name(k)), **test, "group": "keys",
                         "notes": [{"at": 0, "note": k, "len": 0.2}], "length": 0.6, "check": {"dropouts": True}})
        for v in (16, 40, 70, 100, 127):
            segs.append({"name": "velocity %d" % v, **test, "group": "velocity",
                         "notes": [{"at": 0, "note": 53, "len": 0.2, "vel": v}], "length": 0.6})
        hits = [{"at": 0.125 * i, "note": 53, "len": 0.06} for i in range(36)]
        for knob in ("p1", "p2"):
            segs.append({"name": "%s sweep" % knob.upper(), **test, "notes": hits,
                         "sweeps": [{"param": knob, "from": 0, "to": 255, "at": 0, "len": 4.0}],
                         "length": 4.8})
        segs.append({"name": "two keys at once", **test,
                     "notes": [{"at": 0.4 * i, "note": [53, 60], "len": 0.15} for i in range(4)], "length": 2.0})
        segs.append({"name": "fast P1+P2 with hits", **test,
                     "notes": [{"at": 0.1 * i, "note": 53 if i % 2 else 64, "len": 0.08} for i in range(60)],
                     "wobble": {"params": ["p1", "p2"], "at": 0, "len": 6.0, "seed": 1},
                     "length": 6.6, "check": {"clicks": False}})
        segs.append({"name": "tail", **test, "notes": [{"at": 0, "note": 53, "len": 0.1}], "length": 4.0,
                     "check": {"tail": True}})
        return {"description": "default drum script (generated)", "segments": segs}

    root = 60
    triad = [root, root + 4, root + 7, root + 11][:max(1, min(poly, 3))]
    full = [root, root + 4, root + 7, root + 11, root + 14][:poly]
    segs.append({"name": "align", **test, "set": {"attack": 0, "sustain": 255},
                 "notes": [{"at": 0, "note": root, "len": 0.3}], "length": 0.8})
    for p in presets:
        tail = min(2.0, release_tail(p["values"][5]))
        notes = [{"at": 0, "note": root, "len": 1.2}]
        if poly >= 2:
            notes.append({"at": 1.2 + min(1.0, tail), "note": triad, "len": 1.0})
        segs.append({"name": "preset %s" % p["name"], "preset": p["name"], "notes": notes,
                     "length": notes[-1]["at"] + notes[-1]["len"] + tail})
    quick = {"attack": 0, "release": 20, "portamento": 0}       # one note must not ring under the next
    for n in range(24, 97, 12):                                  # C1–C7 as played, whatever the preset's octave
        segs.append({"name": "note %s" % note_name(n), **test, "group": "range", "octave": 0,
                     "set": quick, "notes": [{"at": 0, "note": n, "len": 0.7}], "length": 1.0,
                     "check": {"pitch": True}})
    for v in (16, 40, 70, 100, 127):
        segs.append({"name": "velocity %d" % v, **test, "group": "velocity", "set": quick,
                     "notes": [{"at": 0, "note": root, "len": 0.5, "vel": v}], "length": 0.9})
    for knob in ("p1", "p2"):
        segs.append({"name": "%s sweep" % knob.upper(), **test, "notes": [{"at": 0, "note": root, "len": 4.5}],
                     "sweeps": [{"param": knob, "from": 0, "to": 255, "at": 0.25, "len": 4.0}], "length": 5.0})
    segs.append({"name": "chord", **test, "notes": [{"at": 0, "note": full, "len": 1.5}], "length": 2.2})
    steal = [{"at": 0.1 * i, "note": n, "len": 1.5 - 0.1 * i} for i, n in enumerate(full + [root + 17])]
    segs.append({"name": "voice steal (%d notes on %d voices)" % (poly + 1, poly), **test, "notes": steal,
                 "length": 2.2, "check": {"dropouts": False}})
    segs.append({"name": "fast P1+P2 with chords", **test,
                 "notes": [{"at": 0.5 * i, "note": full, "len": 0.4} for i in range(12)],
                 "wobble": {"params": ["p1", "p2"], "at": 0, "len": 6.0, "seed": 1}, "length": 6.6,
                 "check": {"clicks": False}})
    segs.append({"name": "release tail", **test, "notes": [{"at": 0, "note": root, "len": 0.4}],
                 "length": 0.4 + 3.0, "check": {"tail": True}})
    return {"description": "default %s script (generated)" % kind, "segments": segs}


def _fx_segments(presets) -> list[dict]:
    setup = {"track": "source", "set": {"send": 255, "attack": 0, "decay": 64, "sustain": 255, "release": 40,
                                        "level": 200, "portamento": 0}}
    wet = {"track": "source", "set": {"send": 255}}
    dry = {"track": "source", "set": {"send": 0}}
    src = lambda at, n, ln: {"at": at, "note": n, "len": ln, "track": "source"}     # noqa: E731
    pulses = [src(0.5 * i, 60, 0.3) for i in range(9)]
    return [
        {"name": "align", "sounds": [setup], "notes": [src(0, 60, 0.3)], "length": 0.8},
        {"name": "dry (send 0)", "group": "dry", "sounds": [dry], "notes": [src(0, 60, 1.0)], "length": 1.5},
        {"name": "wet (send 255)", "group": "wet", "sounds": [wet], "notes": [src(0, 60, 1.0)], "length": 3.5,
         "check": {"tail": True}},
        {"name": "P1 sweep", "notes": pulses, "sweeps": [{"param": "p1", "from": 0, "to": 255, "at": 0, "len": 4.0}],
         "length": 5.0},
        {"name": "P2 sweep", "notes": pulses, "sweeps": [{"param": "p2", "from": 0, "to": 255, "at": 0, "len": 4.0}],
         "length": 5.0},
        {"name": "FX-track notes", "notes": [src(0, 60, 3.0)] + [{"at": 0.5 + 0.7 * i, "note": n, "len": 0.5}
                                                                  for i, n in enumerate((60, 64, 67))],
         "length": 3.5},
        {"name": "fast P1+P2 with chords", "notes": [src(0.5 * i, [60, 64, 67], 0.4) for i in range(12)],
         "wobble": {"params": ["p1", "p2"], "at": 0, "len": 6.0, "seed": 1}, "length": 6.6,
         "check": {"clicks": False}},
        {"name": "tail", "notes": [src(0, 60, 0.4)], "length": 4.4, "check": {"tail": True}},
    ]


def load_script(engine_dir: str, engine: dict, track: int, which: str = "auto") -> tuple[dict, str]:
    """(script, where it came from). `auto` = the engine's tests/device.json if there is one, else the default.
    A script with "extends": "default" runs the default segments first, then its own."""
    own = os.path.join(engine_dir, "tests", "device.json")
    if which == "default" or (which == "auto" and not os.path.exists(own)):
        return default_script(engine, track), "default (generated)"
    path = own if which == "auto" else which
    with open(path) as f:
        script = json.load(f)
    if script.get("extends") == "default":
        base = default_script(engine, track)
        script = dict(script, segments=base["segments"] + script.get("segments", []))
    return script, os.path.relpath(path, ROOT) if os.path.abspath(path).startswith(ROOT) else path


# ---------------------------------------------------------------------------------------------------------------
# Plan: a script expanded into timed actions
# ---------------------------------------------------------------------------------------------------------------

def find_script_preset(engine: dict, track: int, ref) -> dict:
    ps = slot_presets(engine, track) or engine["presets"]
    for p in ps:
        if p.get("name") == ref or (isinstance(ref, int) and int(p.get("index", 0)) == ref):
            return p
    raise PlanError("no preset %r for track %s in %s" % (ref, TRACK_NAMES[track], engine["name"]))


def build_plan(script: dict, engine: dict, track: int, initial: dict, source: int | None = None,
               channels: dict | None = None) -> dict:
    """Expand a script: segments with absolute times (seconds from plan start) and actions
    (t, "sound", track, values) / (t, "on", track, note, vel) / (t, "off", track, note).

    `initial` = each touched track's sound at the start (from the OP-Z, or placeholders for a dry run)."""
    channels = channels or {}
    kind = kind_of(track)
    if kind == "fx" and source is None:
        source = TRACKS["lead"]
    gap = float(script.get("gap", GAP))
    state = {t: list(v) for t, v in initial.items()}

    def resolve(name) -> int:
        if name in (None, "test"):
            return track
        if name == "source":
            if source is None:
                raise PlanError("this script uses the source track; give --source")
            return source
        return track_index(str(name))

    changes, notes, segments = [], [], []
    t = LEAD_IN
    for n, spec in enumerate(script.get("segments", [])):
        start, ev0 = t, t + SETTLE
        seg_track = resolve(spec.get("track"))
        octave = spec.get("octave")
        sounds = list(spec.get("sounds", []))
        if any(k in spec for k in ("preset", "values", "set")):
            sounds.insert(0, {k: spec[k] for k in ("preset", "values", "set") if k in spec} | {"track": spec.get("track")})
        for snd in sounds:
            tr = resolve(snd.get("track"))
            upd = {}
            if "preset" in snd:
                p = find_script_preset(engine, tr, snd["preset"])
                upd.update(enumerate(p["values"]))
                if tr == track and octave is None:
                    octave = octave_of(p)
            if "values" in snd:
                upd.update(enumerate(check_sound(tr, snd["values"])))
            for k, v in snd.get("set", {}).items():
                if k == "send":
                    upd[6 if track == TRACKS["fx1"] else 7] = int(v)
                    upd[7 if track == TRACKS["fx1"] else 6] = 0
                else:
                    upd[param_index(k)] = int(v)
            changes.append((start, tr, upd))
        octave = int(octave or 0)
        seg_notes = []
        for ev in spec.get("notes", []):
            tr = resolve(ev.get("track", spec.get("track")))
            keys = ev["note"] if isinstance(ev["note"], list) else [ev["note"]]
            shift = 12 * octave if tr == track else 0
            for k in keys:
                note = parse_note(k) + shift
                if not 0 <= note <= 127:
                    raise PlanError("segment %r: note %s + octave %d is outside MIDI 0..127" % (spec.get("name"), k, octave))
                on = ev0 + float(ev.get("at", 0))
                vel = int(ev.get("vel", 100))
                if not 1 <= vel <= 127:
                    raise PlanError("segment %r: velocity must be 1..127" % spec.get("name"))
                seg_notes.append([round(on, 4), round(on + float(ev.get("len", 0.5)), 4), tr, note, vel])
        last = max([e[1] for e in seg_notes], default=ev0)
        for sw in spec.get("sweeps", []):
            tr, idx = resolve(sw.get("track", spec.get("track"))), param_index(sw["param"])
            a, ln = ev0 + float(sw.get("at", 0)), float(sw["len"])
            steps = max(1, int(round(ln / SWEEP_STEP)))
            for k in range(steps + 1):
                v = round(sw["from"] + (sw["to"] - sw["from"]) * k / steps)
                changes.append((a + k * ln / steps, tr, {idx: int(v)}))
            last = max(last, a + ln)
        wob = spec.get("wobble")
        if wob:
            tr = resolve(wob.get("track", spec.get("track")))
            idxs = [param_index(p) for p in wob.get("params", ["p1", "p2"])]
            rng = random.Random(wob.get("seed", 1))
            lo, hi = int(wob.get("low", 0)), int(wob.get("high", 255))
            a, ln = ev0 + float(wob.get("at", 0)), float(wob["len"])
            for k in range(int(ln / SWEEP_STEP) + 1):
                # extremes a third of the time: the corners are where table indexing and overflows break
                changes.append((a + k * SWEEP_STEP, tr, {i: rng.choice((lo, hi)) if rng.random() < 0.33
                                                         else rng.randint(lo, hi) for i in idxs}))
            last = max(last, a + ln)
        end = ev0 + float(spec["length"]) if "length" in spec else last + float(spec.get("tail", 0.5))
        notes += seg_notes
        # drums: hits choke each other on purpose (hats), so only single-key segments look for dropouts
        chk = {"sound": bool(seg_notes), "dropouts": bool(seg_notes) and kind != "drum", "clicks": True, "pitch": False,
               "silence": False, "tail": False}
        chk.update(spec.get("check", {}))
        if kind != "synth" or any(e[2] != track for e in seg_notes):
            chk["pitch"] = chk["pitch"] and kind == "synth" and all(e[2] == track for e in seg_notes)
        segments.append({"n": n + 1, "name": str(spec.get("name", "segment %d" % (n + 1))), "start": round(start, 4),
                         "end": round(end, 4), "notes": sorted(seg_notes), "check": chk,
                         "group": spec.get("group"), "octave": octave})
        t = end + gap

    actions = _sound_actions(changes, state)
    notes = _no_overlaps(notes)
    for on, off, tr, note, vel in notes:
        actions.append((on, "on", tr, note, vel))
        actions.append((off, "off", tr, note))
    order = {"off": 0, "sound": 1, "on": 2}
    actions.sort(key=lambda a: (a[0], order[a[1]]))
    tracks = sorted(initial)
    return {"engine": engine["name"], "track": track, "source": source, "kind": kind, "tracks": tracks,
            "channels": {tr: channels.get(tr, tr) for tr in tracks},
            "segments": segments, "actions": actions, "duration": round(t - gap + 0.5, 3),
            "restore": {tr: list(initial[tr]) for tr in tracks}}


def _sound_actions(changes, state) -> list:
    """Fold parameter changes into full 18-byte sounds per track, then rate-limit: no two `$0E` closer than
    MIN_SOUND_INTERVAL; a delayed change is dropped when a later change of the same track supersedes it."""
    snaps = []
    for t, tr, upd in sorted(changes, key=lambda c: c[0]):
        if tr not in state:
            raise PlanError("track %s is not in the plan's tracks" % TRACK_NAMES[tr])
        for i, v in upd.items():
            if not 0 <= v <= 255:
                raise PlanError("parameter %s value %d is outside 0..255" % (PARAM_NAMES[i], v))
            state[tr][i] = v
        if snaps and snaps[-1][1] == tr and abs(snaps[-1][0] - t) < 1e-9:
            snaps[-1] = (t, tr, list(state[tr]))
        else:
            snaps.append((t, tr, list(state[tr])))
    out, last, sent = [], -1e9, {}
    for i, (t, tr, values) in enumerate(snaps):
        when = max(t, last + MIN_SOUND_INTERVAL)
        if any(s[1] == tr and s[0] <= when for s in snaps[i + 1:i + 8]):
            continue                                           # superseded before it could go out
        if sent.get(tr) == values:
            continue
        out.append((round(when, 4), "sound", tr, values))
        sent[tr] = values
        last = when
    return out


def _no_overlaps(notes) -> list:
    """A note that starts while the same key is held on that track ends the held one 5 ms earlier."""
    notes = sorted(notes)
    for i, a in enumerate(notes):
        for b in notes[i + 1:]:
            if b[0] >= a[1]:
                break
            if b[2] == a[2] and b[3] == a[3]:
                a[1] = round(max(a[0] + 0.005, b[0] - 0.005), 4)
    return notes


def describe_plan(plan: dict, log=print) -> None:
    tr = plan["track"]
    log("plan: %s on track %s (MIDI channel %d), %d segments, %.1f s, %d $0E sends, %d notes" % (
        plan["engine"], TRACK_NAMES[tr], plan["channels"][tr] + 1, len(plan["segments"]), plan["duration"],
        sum(1 for a in plan["actions"] if a[1] == "sound"), sum(1 for a in plan["actions"] if a[1] == "on")))
    if plan["source"] is not None:
        log("      source track %s (MIDI channel %d) feeds the effect through its send" % (
            TRACK_NAMES[plan["source"]], plan["channels"][plan["source"]] + 1))
    for s in plan["segments"]:
        keys = sorted({note_name(n[3]) for n in s["notes"]}, key=parse_note)
        sounds = sum(1 for a in plan["actions"] if a[1] == "sound" and s["start"] <= a[0] < s["end"] + GAP)
        log("  %2d %6.2f-%6.2f s  %-34s %3d notes %-24s %3d $0E  checks: %s" % (
            s["n"], s["start"], s["end"], s["name"][:34], len(s["notes"]), " ".join(keys[:5]) + (" …" if len(keys) > 5 else ""),
            sounds, ", ".join(k for k, v in s["check"].items() if v is True)))
    log("  then: the sounds of %s are put back" % ", ".join(TRACK_NAMES[t] for t in plan["tracks"]))


def to_scn(plan: dict, tempo: float = 120.0) -> str:
    """The plan's test-track part as an opzsim scenario (one step = a sixteenth at `tempo`)."""
    step = 60.0 / tempo / 4
    tr = plan["track"]
    lines = ["# generated by tools/opz_bench.py from the %s device test plan (track %s)" % (plan["engine"], TRACK_NAMES[tr]),
             "tempo %g" % tempo, "steps %d" % math.ceil(plan["duration"] / step), "tail 0.5", "track %s" % TRACK_NAMES[tr]]
    current = list(plan["restore"][tr])
    sounds = [a for a in plan["actions"] if a[1] == "sound" and a[2] == tr]
    if sounds and sounds[0][0] <= LEAD_IN + 1e-6:
        current = list(sounds.pop(0)[3])
    lines.append("preset " + " ".join(str(v) for v in current))
    for a in plan["actions"]:
        if a[2] != tr:
            continue
        if a[1] == "sound":
            for i, v in enumerate(a[3]):
                if v != current[i]:
                    lines.append("param %d %d %.4f" % (i, v, max(0.001, a[0] / step)))
            current = list(a[3])
    for on, off, ntr, note, vel in sorted(n for s in plan["segments"] for n in s["notes"]):
        if ntr == tr:
            lines.append("note %.4f %d %.4f %d" % (on / step, note, (off - on) / step, vel))
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------------------------------------------
# Playing
# ---------------------------------------------------------------------------------------------------------------

def play(plan: dict, session: StateSession, transport, capture, clock, log=print, stop_on_silence: bool = True) -> dict:
    """Send the plan's actions on time while keeping the session alive and watching for a crash. Returns timing,
    the abort reason (or None) and the clock time of plan t=0."""
    t0 = clock() + 0.2
    held, late, done = set(), 0.0, 0
    next_watch = next_ports = 0.0
    note_times = [t0 + a[0] for a in plan["actions"] if a[1] == "on"]
    sound_windows = [(t0 + n[0], t0 + n[1]) for s in plan["segments"] if s["check"].get("sound") for n in s["notes"]]
    abort = None

    def watchdog(now):
        nonlocal next_ports
        if session.last_ack is not None and now - session.last_ack > ACK_TIMEOUT:
            raise Abort("session lost: no $01 reply to the hellos for %.1f s" % (now - session.last_ack))
        if capture is not None:
            if not capture.alive():
                raise Abort("the audio stream ended (USB audio gone: the OP-Z restarted or crashed)")
            if capture.data_age() > AUDIO_TIMEOUT:
                raise Abort("no audio data for %.1f s (the OP-Z's USB audio stopped)" % capture.data_age())
        if now < next_ports:
            return
        next_ports = now + PORTS_EVERY
        if not transport.present():
            raise Abort("the OP-Z's MIDI ports vanished (it restarted or crashed)")
        if capture is not None:
            if stop_on_silence and now - t0 > HANG_WINDOW + 0.5:
                a, b = now - HANG_WINDOW, now - 0.3
                played = sum(1 for t in note_times if a <= t <= b - 0.1)
                in_sound = any(w0 <= b - 0.1 and w1 >= a for w0, w1 in sound_windows)
                level = capture.level_db(a, b)
                if played >= 2 and in_sound and level < -90:
                    raise Abort("no audio for %.1f s while %d notes played (hung, muted, or the engine is not active%s)"
                                % (HANG_WINDOW, played, "; all zeros: or macOS is blocking the microphone for this app"
                                   if level <= -180 else ""))

    try:
        for act in plan["actions"]:
            target = t0 + act[0]
            while True:
                now = clock()
                if now >= next_watch:
                    watchdog(now)
                    next_watch = now + WATCH_EVERY
                if now >= target:
                    break
                session.tick(min(0.005, target - now))
            late = max(late, clock() - target)
            if act[1] == "sound":
                session.set(act[2], act[3])
            elif act[1] == "on":
                transport.send_short(bytes((0x90 | plan["channels"][act[2]], act[3], act[4])))
                held.add((act[2], act[3]))
            else:
                transport.send_short(bytes((0x80 | plan["channels"][act[2]], act[3], 0)))
                held.discard((act[2], act[3]))
            done += 1
        end = t0 + plan["duration"]
        while clock() < end:
            now = clock()
            if now >= next_watch:
                watchdog(now)
                next_watch = now + WATCH_EVERY
            session.tick(min(0.005, end - now))
    except Abort as e:
        abort = str(e)
        log("STOPPED: %s" % abort)
    except KeyboardInterrupt:
        abort = "interrupted (Ctrl-C)"
        log("STOPPED: interrupted")
    except (OSError, RuntimeError) as e:                      # rtmidi/mido errors once the ports are gone
        if isinstance(e, PlanError):
            raise
        abort = "MIDI error: %s (the OP-Z disconnected?)" % e
        log("STOPPED: %s" % abort)
    finally:
        try:
            if held and transport.present():
                for tr, note in sorted(held):
                    transport.send_short(bytes((0x80 | plan["channels"][tr], note, 0)))
        except (OSError, RuntimeError):
            pass
    return {"t0": t0, "abort": abort, "actions_done": done, "actions": len(plan["actions"]),
            "late_max_ms": round(1000 * late, 1), "stopped_at": round(clock() - t0, 3)}


def restore(plan: dict, session: StateSession, transport, clock, log=print) -> bool:
    """Put back each touched track's sound; False (with the commands to do it by hand) if the OP-Z is gone."""
    alive = transport.present() and (session.last_ack is None or clock() - session.last_ack < ACK_TIMEOUT)
    if alive:
        try:
            for tr in plan["tracks"]:
                session.set(tr, plan["restore"][tr])
                session.pump(MIN_SOUND_INTERVAL + 0.01)
            session.pump(0.3)
            log("restored the sounds of %s" % ", ".join(TRACK_NAMES[t] for t in plan["tracks"]))
            return True
        except (OSError, RuntimeError) as e:
            log("restore failed: %s" % e)
    log("could NOT restore the sounds (the OP-Z is gone). When it is back, if the project kept the test sounds:")
    for tr in plan["tracks"]:
        log("  " + set_command(tr, plan["restore"][tr]))
    return False


# ---------------------------------------------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------------------------------------------

def slot_check(transport, engine: dict, track: int, log=print, clock=time.monotonic) -> dict:
    """Read the slot map (read-only) and say which slot holds the engine's plug on the track."""
    fs = FileSession(transport, log=lambda m: None, clock=clock)
    fs.open(timeout=10)
    try:
        plugs, slots = json.loads(fs.pull(PLUGS)), json.loads(fs.pull(SLOTS))
    finally:
        fs.close()
    pid = int(engine["manifest"]["plug"]["id"])
    on_track = [e["slot"] for e in slot_entries(slots, track) if e["id"] == pid]
    record = next((p for p in plugs.get("plugs", []) if p.get("id") == pid), None)
    if record is None:
        raise PlanError("plug %d (%s) is not installed; install it first (opz.py install — the test runner never "
                        "installs)" % (pid, engine["name"]))
    if not on_track:
        raise PlanError("plug %d (%s) is not in track %s's slot map; assign it first (opz.py assign %d --track %s)"
                        % (pid, engine["name"], TRACK_NAMES[track], pid, TRACK_NAMES[track]))
    slot = on_track[0]
    log("slot map: %s (plug %d, version %s) is in slot %d of %s: on the OP-Z hold %s and press black key %d" % (
        engine["name"], pid, record.get("version"), slot, TRACK_NAMES[track], TRACK_NAMES[track], slot % 10))
    return {"plug": pid, "slot": slot, "version": record.get("version")}


def ask(question: str) -> str:
    try:
        return input(question + " [y/N] ").strip()
    except EOFError:
        return ""


# ---------------------------------------------------------------------------------------------------------------
# Analysis and report
# ---------------------------------------------------------------------------------------------------------------

def align(samples, rate: int, plan: dict, coarse: float) -> tuple[float, str]:
    """Refine the recording offset (recording seconds at plan t=0) from the first note's onset."""
    first = min((n[0] for s in plan["segments"] for n in s["notes"]), default=None)
    if first is None:
        return coarse, "coarse only (no notes)"
    pre = 0.25
    x = au.mono(samples, int((first + coarse - pre) * rate), int((first + coarse + 0.6) * rate))
    noise = au.rms_db(au.mono(samples, int((first + coarse - 0.9) * rate), int((first + coarse - pre - 0.05) * rate)))
    onset = au.find_onset(x, rate, max(noise + 20, -50.0))
    if onset is None:
        return coarse, "coarse only (no onset found near the first note)"
    delta = onset - pre
    if not -0.24 <= delta <= 0.55:
        return coarse, "coarse only (onset %+.0f ms away: implausible)" % (1000 * delta)
    return coarse + delta, "first-note onset %+.1f ms from the clock estimate" % (1000 * delta)


def analyse(samples, rate: int, plan: dict, offset: float) -> list[dict]:
    results = [au.analyse_segment(samples, rate, s, offset) for s in plan["segments"]]
    cents = [c for r in results for c in r.get("cents", [])]
    octave = 0
    if cents:
        counts = {}
        for c in cents:
            counts[round(c / 1200)] = counts.get(round(c / 1200), 0) + 1
        octave = max(counts, key=counts.get)
    for r in results:
        if r.get("cents"):
            res = sorted(au.fold_cents(c) for c in r["cents"])
            r["pitch_cents"] = round(res[len(res) // 2], 1)
            shifts = sorted({round((c - au.fold_cents(c)) / 1200) - octave for c in r["cents"]} - {0})
            if shifts:
                r["octave_shift"] = shifts[0]
            if abs(r["pitch_cents"]) > PITCH_TOLERANCE:
                r["reasons"].append("WARN: pitch off by %+.0f cents" % r["pitch_cents"])
                if r["status"] == "OK":
                    r["status"] = "WARN"
    for r, s in zip(results, plan["segments"]):
        r["n"], r["name"], r["group"] = s["n"], s["name"], s["group"]
    results.append({"octave_offset": octave})
    return results


def summary_lines(plan: dict, results: list[dict]) -> list[str]:
    rows = [r for r in results if "n" in r]
    octave = results[-1].get("octave_offset", 0) if results else 0
    out = []
    noise = [r["noise_db"] for r in rows if "noise_db" in r]
    if noise:
        out.append("Noise floor: %.1f dBFS" % noise[0])
    pitched = [r["pitch_cents"] for r in rows if "pitch_cents" in r]
    if pitched:
        shifted = ["%s (%+d)" % (r["name"], r["octave_shift"]) for r in rows if r.get("octave_shift")]
        out.append("Pitch: %d segments measured, median error %+.1f cents from the key%s%s" % (
            len(pitched), sorted(pitched)[len(pitched) // 2],
            "" if octave == 0 else "; MIDI notes sound %+d octave(s) off (the track's octave applies to MIDI input?)"
            % octave, "; in another octave than the rest: " + ", ".join(shifted) if shifted else ""))
    vel = [r for r in rows if r.get("group") == "velocity" and "note_db" in r]
    if len(vel) >= 2:
        levels = [r["note_db"] for r in vel]
        rising = all(b >= a - 1.0 for a, b in zip(levels, levels[1:]))
        out.append("Velocity: %s (%s dBFS)" % ("level rises with velocity" if rising else "level does NOT rise steadily",
                                              " → ".join("%.1f" % v for v in levels)))
    dry = next((r for r in rows if r.get("group") == "dry" and "note_db" in r), None)
    wet = next((r for r in rows if r.get("group") == "wet" and "rms_db" in r), None)
    if dry and wet:
        out.append("Effect: wet segment %.1f dB vs dry %.1f dB (send 255 vs 0)%s" % (
            wet["rms_db"], dry["rms_db"], "; the tail lasts %.2f s" % wet["tail_s"] if wet.get("tail_s") else ""))
    return out


def readme_checklist(engine_dir: str) -> list[str]:
    path = os.path.join(engine_dir, "README.md")
    if not os.path.exists(path):
        return []
    lines, on = [], False
    with open(path) as f:
        for line in f:
            if line.startswith("## "):
                on = "listen for" in line.lower()
                continue
            if on:
                lines.append(line.rstrip("\n"))
    while lines and not lines[-1].strip():
        lines.pop()
    while lines and not lines[0].strip():
        lines.pop(0)
    return lines


def slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")[:40]


def write_snippets(run_dir: str, samples, rate: int, plan: dict, offset: float) -> dict:
    out = {}
    for s in plan["segments"]:
        a = int((s["start"] + offset - 0.05) * rate)
        b = int((min(s["end"], s["start"] + SNIPPET_MAX) + offset) * rate)
        a, b = max(0, a), min(len(samples) // 2, b)
        if b - a < rate // 20:
            continue
        name = "snippets/%02d-%s.wav" % (s["n"], slug(s["name"]))
        au.write_wav(os.path.join(run_dir, name), samples[2 * a:2 * b], rate)
        out[s["n"]] = name
    return out


def render_sim(plan: dict, engine: dict, run_dir: str, log=print) -> tuple[object, int] | None:
    """Render the plan with opzsim (instrument and drum engines); None with a note when it cannot."""
    if plan["kind"] == "fx":
        log("sim: effects are not compared (the device input is another track's engine)")
        return None
    sim = os.path.join(ROOT, "sim", "build", "opzsim")
    dylib = os.path.join(ROOT, "sim", "build", "engines", engine["dir"] + ".dylib")
    if not (os.path.exists(sim) and os.path.exists(dylib)):
        log("sim: build it first: make -C sim build/opzsim build/engines/%s.dylib" % engine["dir"])
        return None
    scn, wav = os.path.join(run_dir, "sim.scn"), os.path.join(run_dir, "sim.wav")
    with open(scn, "w") as f:
        f.write(to_scn(plan))
    m = engine["manifest"]
    r = subprocess.run([sim, "-e", dylib, "--slot", str(m["memoryslot"]), "--trackmask", str(m["plug"]["trackmask"]),
                        "--bits", "16", "-o", wav, scn], capture_output=True, text=True)
    if not os.path.exists(wav):
        log("sim: opzsim failed (exit %d): %s" % (r.returncode, (r.stderr or "").strip()[-300:]))
        return None
    if r.returncode:
        log("sim: opzsim exit %d (%s)" % (r.returncode, (r.stderr or "").strip().splitlines()[-1:] or ""))
    return au.read_wav(wav)


def write_report(run_dir: str, info: dict, plan: dict, results: list[dict], snippets: dict,
                 sim_results: list[dict] | None, checklist: list[str]) -> str:
    rows = [r for r in results if "n" in r]
    counts = {k: sum(1 for r in rows if r["status"] == k) for k in ("OK", "WARN", "FAIL")}
    tr = plan["track"]
    L = ["# Device test: %s on %s" % (plan["engine"], TRACK_NAMES[tr]), ""]
    if info.get("abort"):
        L += ["> **STOPPED: %s** after %.1f s. Segments after that point were not played." % (
            info["abort"], info.get("stopped_at", 0)), ""]
    L += ["- When: %s" % info.get("when", ""),
          "- Engine: %s (plug %s), track %s, MIDI channel %d%s" % (
              plan["engine"], info.get("plug", "?"), TRACK_NAMES[tr], plan["channels"][tr] + 1,
              "; source track %s" % TRACK_NAMES[plan["source"]] if plan["source"] is not None else ""),
          "- Script: %s, %d segments, %.1f s" % (info.get("script", "?"), len(plan["segments"]), plan["duration"]),
          "- Engine active on the track: %s" % info.get("engine_active", "not asked"),
          "- Result: **%d OK, %d WARN, %d FAIL**%s" % (counts["OK"], counts["WARN"], counts["FAIL"],
                                                         " — **CRASH / STOP**" if info.get("abort") else ""),
          "- Alignment: %s" % info.get("alignment", "n/a"),
          "- Timing: latest action %s ms late" % info.get("late_max_ms", "?"),
          "- Restore: %s" % info.get("restored", "n/a")]
    L += ["- " + s for s in summary_lines(plan, results)]
    if info.get("all_zero"):
        L += ["- **The recording is all zeros**: macOS probably blocked the microphone for this app (System "
              "Settings → Privacy & Security → Microphone), or the OP-Z's USB audio is muted."]
    L += ["", "| # | segment | time (s) | RMS dB | peak dB | clip | dropouts | clicks | pitch (¢) | tail (s) |%s status | listen |"
          % (" sim RMS | Δ dB |" if sim_results else ""),
          "|---|---|---|---|---|---|---|---|---|---|%s---|---|" % ("---|---|" if sim_results else "")]
    sim_by_n = {r["n"]: r for r in (sim_results or []) if "n" in r}
    deltas = [r["rms_db"] - sim_by_n[r["n"]]["rms_db"] for r in rows
              if r["n"] in sim_by_n and "rms_db" in r and "rms_db" in sim_by_n[r["n"]] and r.get("note_db", 0) > -60]
    median = sorted(deltas)[len(deltas) // 2] if deltas else 0.0
    for r in rows:
        fmt = lambda k, f="%.1f": (f % r[k]) if r.get(k) is not None else ""            # noqa: E731
        cells = ["%d" % r["n"], r["name"], "%.1f–%.1f" % (r["start"], r["end"]), fmt("rms_db"), fmt("peak_db"),
                 str(r.get("clipped", "")), str(len(r.get("dropouts", []))) if "dropouts" in r else "",
                 clicks_cell(r), fmt("pitch_cents", "%+.0f") + (
                     " (oct %+d)" % r["octave_shift"] if r.get("octave_shift") else ""), fmt("tail_s", "%.2f")]
        if sim_results:
            sr = sim_by_n.get(r["n"], {})
            both = "rms_db" in sr and "rms_db" in r and min(sr["rms_db"], r["rms_db"]) > -90
            cells += [("%.1f" % sr["rms_db"]) if "rms_db" in sr else "",
                      ("%+.1f" % (r["rms_db"] - sr["rms_db"] - median)) if both else ""]
        cells += ["**%s**" % r["status"] if r["status"] != "OK" else "OK",
                  "[wav](%s)" % snippets[r["n"]] if r["n"] in snippets else ""]
        L.append("| " + " | ".join(c.replace("|", "/") for c in cells) + " |")
    findings = [r for r in rows if r["reasons"]]
    if findings:
        L += ["", "## Findings", ""]
        L += ["- %d %s: %s" % (r["n"], r["name"], "; ".join(r["reasons"])) for r in findings]
    sim_findings = [r for r in (sim_results or []) if r.get("reasons")]
    if sim_findings:
        L += ["", "## Findings in the opzsim render", ""]
        L += ["- %d %s: %s" % (r["n"], r["name"], "; ".join(r["reasons"])) for r in sim_findings]
    if sim_results:
        L += ["", "Sim: the same plan rendered by opzsim (test track only; no OS level, pan or master effects, so "
                  "Δ is device − sim minus the median difference %.1f dB: look for segments far from 0)." % median]
    if checklist:
        L += ["", "## Listen (from the engine's README)", ""] + checklist
    if info.get("seen"):
        fmt_ids = lambda d: ", ".join("$%02X × %d" % (int(k), v) for k, v in sorted(d.items(), key=lambda kv: int(kv[0])))  # noqa: E731
        L += ["", "## State-sync messages received (research)", "",
              "- opening burst: " + fmt_ids(info.get("seen_burst", {})), "- whole run: " + fmt_ids(info["seen"]),
              "", "Anything besides $01 and $0E is new: see docs/roadmap/device-tests.md, \"Selecting a sound slot "
              "remotely\"."]
    L += ["", "Files: recording.wav (whole run), snippets/, run.json (plan, timing, measurements; "
              "`opz_bench.py --analyze %s` re-measures offline)." % os.path.basename(run_dir.rstrip("/"))]
    path = os.path.join(run_dir, "report.md")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")
    return path


def clicks_cell(r: dict) -> str:
    if "clicks" not in r:
        return ""
    return str(len(r["clicks"])) + (" (+%d at note starts)" % r["onset_clicks"] if r.get("onset_clicks") else "")


def finish(run_dir: str, plan: dict, info: dict, samples, rate: int, offset: float, engine: dict | None,
           engine_dir: str | None, sim: bool, log=print) -> int:
    """Measure, write snippets, report and run.json. Exit code: 0 ok/warn, 1 a segment failed, 3 crash/stop."""
    os.makedirs(run_dir, exist_ok=True)
    info["all_zero"] = len(samples) > 0 and not any(samples[::97]) and not any(samples)
    results = analyse(samples, rate, plan, offset)
    snippets = write_snippets(run_dir, samples, rate, plan, offset)
    sim_results = None
    if sim and engine is not None:
        got = render_sim(plan, engine, run_dir, log)
        if got:
            sim_results = analyse(got[0], got[1], plan, 0.0)
    checklist = readme_checklist(engine_dir) if engine_dir else info.get("checklist", [])
    info["checklist"] = checklist
    report = write_report(run_dir, info, plan, results, snippets, sim_results, checklist)
    with open(os.path.join(run_dir, "run.json"), "w") as f:
        json.dump({"info": info, "plan": plan, "offset": offset, "results": results, "sim": sim_results,
                   "engine_dir": engine_dir}, f, indent=1)
    rows = [r for r in results if "n" in r]
    log("report: %s (%d OK, %d WARN, %d FAIL)" % (report, *(sum(1 for r in rows if r["status"] == k)
                                                           for k in ("OK", "WARN", "FAIL"))))
    if info.get("abort"):
        return 3
    return 1 if any(r["status"] == "FAIL" for r in rows) else 0


def reanalyse(run_dir: str, sim: bool = False, log=print) -> int:
    with open(os.path.join(run_dir, "run.json")) as f:
        doc = json.load(f)
    plan = doc["plan"]
    plan["channels"] = {int(k): v for k, v in plan["channels"].items()}
    plan["restore"] = {int(k): v for k, v in plan["restore"].items()}
    samples, rate = au.read_wav(os.path.join(run_dir, "recording.wav"))
    engine_dir = doc.get("engine_dir")
    engine = load_manifest(engine_dir) if engine_dir and os.path.isdir(engine_dir) else None
    if engine is not None:
        engine["dir"] = os.path.basename(os.path.abspath(engine_dir).rstrip("/"))
    info = doc["info"]
    return finish(run_dir, plan, info, samples, rate, doc["offset"], engine, engine_dir, sim, log)


# ---------------------------------------------------------------------------------------------------------------
# The whole run
# ---------------------------------------------------------------------------------------------------------------

class Bench:
    def __init__(self, transport, capture, clock=time.monotonic, log=print, confirm=None, ask_engine=None):
        self.t = Guard(transport)
        self.capture = capture
        self.clock = clock
        self.log = log
        self.confirm = confirm or (lambda q: ask(q).lower() in ("y", "yes"))
        self.ask_engine = ask_engine or (lambda q: ask(q))

    def run(self, engine: dict, engine_dir: str, script: dict, script_name: str, track: int, run_dir: str | None,
            source: int | None = None, channels: dict | None = None, slot_check_on: bool = True,
            engine_confirmed: bool = False, yes: bool = False, sim: bool = False, stop_on_silence: bool = True) -> int:
        log = self.log
        info = {"when": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), "script": script_name,
                "plug": engine["manifest"]["plug"]["id"]}
        if not self.t.present():
            raise PlanError("no OP-Z MIDI ports (connect it by USB, normal mode)")
        if slot_check_on:
            sc = slot_check(self.t, engine, track, log, self.clock)
            info["slot"] = sc["slot"]
            where = "slot %d (hold %s, press black key %d)" % (sc["slot"], TRACK_NAMES[track], sc["slot"] % 10)
        else:
            where = "its slot"
        if self.capture is not None:
            dev = self.capture.find_device()
            log("audio: recording %r" % dev[1])
            self.capture.start()
        try:
            question = "Is %s the active sound on track %s (selected with %s)?" % (engine["name"], TRACK_NAMES[track], where)
            if engine_confirmed:
                info["engine_active"] = "yes (confirmed by the user, --engine-confirmed)"
            else:
                answer = self.ask_engine(question)
                info["engine_active"] = "%s (answered %r at the prompt)" % (
                    "yes" if answer.lower() in ("y", "yes") else "NO", answer)
                if answer.lower() not in ("y", "yes"):
                    log("select the engine first (%s), then run again. Nothing was sent." % where)
                    return 2
            tracks = [track] + ([source] if source is not None else [])
            placeholder = {t: [0] * 18 for t in tracks}
            describe_plan(build_plan(script, engine, track, placeholder, source, channels), log)
            if not yes and not self.confirm("Play this on the OP-Z (sounds of %s change in the active pattern and "
                                            "come back at the end)?" % ", ".join(TRACK_NAMES[t] for t in tracks)):
                log("cancelled. Nothing was sent.")
                return 2
            s = StateSession(self.t, log=lambda m: log("  " + m), clock=self.clock)
            s.open(tracks)
            info["seen_burst"] = dict(s.seen)
            initial = {t: list(s.sounds[t]) for t in tracks}
            for t in tracks:
                log("track %s now: %s" % (TRACK_NAMES[t], format_sound(initial[t])))
                log("if this is interrupted, put it back with: " + set_command(t, initial[t]))
            plan = build_plan(script, engine, track, initial, source, channels)
            result = play(plan, s, self.t, self.capture, self.clock, log, stop_on_silence)
            info.update({k: result[k] for k in ("abort", "late_max_ms", "stopped_at", "actions_done", "actions")})
            info["restored"] = "yes" if restore(plan, s, self.t, self.clock, log) else "NO (commands in the log)"
            info["seen"] = dict(s.seen)
            s.close()
        finally:
            if self.capture is not None:
                self.capture.stop()
        if self.capture is None or run_dir is None:
            return 3 if info.get("abort") else 0
        samples = self.capture.samples()
        os.makedirs(run_dir, exist_ok=True)
        au.write_wav(os.path.join(run_dir, "recording.wav"), samples)
        coarse = result["t0"] - self.capture.start_time
        offset, how = align(samples, au.RATE, plan, coarse)
        info["alignment"] = "%s; plan t=0 at %.3f s into the recording" % (how, offset)
        return finish(run_dir, plan, info, samples, au.RATE, offset, engine, engine_dir, sim, log)


def parse_channels(items) -> dict:
    out = {}
    for item in items or []:
        name, _, ch = item.partition("=")
        if not ch.isdigit() or not 1 <= int(ch) <= 16:
            raise PlanError("--channel wants TRACK=1..16, got %r" % item)
        out[track_index(name)] = int(ch) - 1
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="opz_bench.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("engine_dir", nargs="?")
    ap.add_argument("--track", type=track_index)
    ap.add_argument("--script", default="auto", help="auto (tests/device.json, else default), default, or a path")
    ap.add_argument("--record", help="run directory (default device-tests/<time>-<engine>-<track>/)")
    ap.add_argument("--no-record", action="store_true", help="play only (listen by ear), no capture or report")
    ap.add_argument("--dry-run", action="store_true", help="print the plan; touch nothing")
    ap.add_argument("--engine-confirmed", action="store_true",
                    help="the user has confirmed the engine is the track's active sound (recorded in the report)")
    ap.add_argument("--yes", action="store_true", help="skip the run confirmation")
    ap.add_argument("--no-slot-check", action="store_true", help="skip the read-only slot map check")
    ap.add_argument("--source", type=track_index, help="effects: the synth track whose send feeds the effect (lead)")
    ap.add_argument("--channel", action="append", help="TRACK=CH if the OP-Z's MIDI channels differ (default ch = track + 1)")
    ap.add_argument("--audio-device", default="OP-Z")
    ap.add_argument("--sim", action="store_true", help="compare with opzsim renders of the same plan")
    ap.add_argument("--no-stop-on-silence", action="store_true")
    ap.add_argument("--scn", help="write the plan as an opzsim scenario and exit (offline)")
    ap.add_argument("--analyze", metavar="RUN_DIR", help="re-measure a recorded run (offline)")
    ap.add_argument("--port", default="OP-Z")
    ap.add_argument("--fake", action="store_true", help="in-memory OP-Z with a synthesised test tone (no hardware)")
    a = ap.parse_args(argv)
    try:
        if a.analyze:
            return reanalyse(a.analyze, a.sim)
        if not a.engine_dir or a.track is None:
            ap.error("ENGINE_DIR and --track are required")
        engine = load_manifest(a.engine_dir)
        engine["dir"] = os.path.basename(os.path.abspath(a.engine_dir).rstrip("/"))
        mask = int(engine["manifest"]["plug"].get("trackmask", 0))
        if a.track == TRACKS["master"]:
            raise PlanError("never on master: the OS calls master's renderFX with out = NULL (research/routing.md)")
        if a.track == TRACKS["tape"]:
            raise PlanError("tape is not supported by the test runner")
        if not mask >> a.track & 1:
            raise PlanError("%s's trackmask (%d) does not include track %s" % (engine["name"], mask, TRACK_NAMES[a.track]))
        if kind_of(a.track) != "fx" and a.source is not None:
            raise PlanError("--source is for effect engines")
        source = a.source if a.source is not None else (TRACKS["lead"] if kind_of(a.track) == "fx" else None)
        if source is not None and kind_of(source) != "synth":
            raise PlanError("--source must be a synth track (bass, lead, arp, chord)")
        script, script_name = load_script(a.engine_dir, engine, a.track, a.script)
        channels = parse_channels(a.channel)
        tracks = [a.track] + ([source] if source is not None else [])
        if a.dry_run or a.scn:
            from fake_opz import DEFAULT_SOUND
            plan = build_plan(script, engine, a.track, {t: list(DEFAULT_SOUND) for t in tracks}, source, channels)
            if a.scn:
                with open(a.scn, "w") as f:
                    f.write(to_scn(plan))
                print("wrote %s" % a.scn)
                return 0
            print("script: %s" % script_name)
            describe_plan(plan)
            print("dry run: nothing sent (values not set by the script come from the OP-Z at the start of a run)")
            return 0
        run_dir = None if a.no_record else (a.record or os.path.join(
            ROOT, "device-tests", "%s-%s-%s" % (datetime.datetime.now().strftime("%Y%m%d-%H%M%S"), engine["dir"],
                                                TRACK_NAMES[a.track])))
        if a.fake:
            from fake_opz import FakeOpz
            dev = FakeOpz()
            _fake_assign(dev, engine, a.track)
            capture = None if a.no_record else au.FakeCapture(dev)
            bench = Bench(dev, capture, clock=lambda: dev.now)
            transport_cm = contextlib.nullcontext(dev)
        else:
            transport_cm = MidoTransport(a.port)
        with transport_cm as transport:
            if not a.fake:
                capture = None if a.no_record else au.FfmpegCapture(a.audio_device)
                bench = Bench(transport, capture)
            return bench.run(engine, a.engine_dir, script, script_name, a.track, run_dir, source, channels,
                             not a.no_slot_check, a.engine_confirmed, a.yes, a.sim, not a.no_stop_on_silence)
    except (PlanError, SessionError, au.CaptureError, OSError, ValueError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 2


def _fake_assign(dev, engine: dict, track: int) -> None:
    """--fake: put the engine's plug into the fake's slot map on the track (as if installed), so the slot check
    passes."""
    plugs, slots = json.loads(dev.files[PLUGS]), json.loads(dev.files[SLOTS])
    pid = int(engine["manifest"]["plug"]["id"])
    path = "engines/%s.engine" % engine["name"]
    if not any(p["id"] == pid for p in plugs["plugs"]):
        plugs["engines"].append({"memoryslot": engine["manifest"]["memoryslot"], "path": path, "version": 1})
        plugs["plugs"].append({"engine": path, "flags": 0, "id": pid, "samples": "", "version": 1})
    tr = next((t for t in slots["tracks"] if t["index"] == track), None)
    if tr is None:
        tr = {"index": track, "packs": []}
        slots["tracks"].append(tr)
    tr["packs"] = [p for p in tr["packs"] if p["slot"] != 3] + [{"id": pid, "slot": 3}]
    dev.files[PLUGS], dev.files[SLOTS] = json.dumps(plugs).encode(), json.dumps(slots).encode()


if __name__ == "__main__":
    raise SystemExit(main())
