#!/usr/bin/env python3
"""Measure Blackfin cycle counts of OP-Z engines offline, in the GNU Blackfin simulator (bfin-elf-run).

Usage:
    opz_cycles.py engines/NAME [engines/NAME ...] [--all] [--quick] [--json FILE] [--markdown]
    opz_cycles.py engines/NAME --profile "CASE" [--profile-blocks N]
    opz_cycles.py engines/NAME [--all] --dump DIR       also save every rendered block (device build output)
    opz_cycles.py --compare DIR_A DIR_B                 null test of two dumps: peak and RMS difference
    opz_cycles.py --layout "bass=table chord=modal*2 kick=kit ..." [--report FILE.json] [--line N]
                                                        a whole set of tracks against the OS's shared pool

For each engine: build it with tools/opz_build.py (the same compiler, flags, linker script and slot as the
device build), copy its image byte for byte to the slot base of a bare-metal driver (tools/cycles/), and run the
driver in the simulator. The driver finds the entry points through the export table, as the OS does, calls
initModule, then per case init + onNoteOn for every voice and render/renderFX for N blocks, reading the CYCLES
register around every call. The host's envelope callback (our model) runs inside render, as TE's does.
--profile traces one case instruction by instruction and counts instructions per engine function and source line.
--layout adds up a whole layout (an engine and a number of sounding voices per instrument track) against the one
pool the OS times, instead of judging each engine against the per-voice figure (research/cpu-headroom.md §2.1).

Every call also runs the crash checks of tools/cycles/checks.S (research/crash-safety.md): a stack per entry
point, registers handed back (L0-L3, LC0/LC1, callee-saved, SP), guards past the device's state area and the
buffers, the engine's code and the rest of its slot window, and the NULL page unmapped so that a NULL access
stops the simulator as it crashes the OP-Z. They are printed after each engine (tools/opz_crashcheck.py adds
static checks and a verdict).

Reported per case: cycles per render call (one voice, one 64-frame block; the mean leaves out warm-up blocks),
cycles per sample, the worst block, note-on cost, and the verdict against the budget in
research/cpu-budget.md: the OS times its instrument stage (every voice plus its own per-track work) and kills a
releasing voice above 265,000 cycles per block three blocks running, or above 290,000 once. After the OS's own share
(~61,500 cycles with 8 active tracks and 19 voices, from instruction counts) that leaves ~167 cycles/sample/voice
for the steady state and ~188 for a single block, with 19 voices; 150 is the ABI's target.

The simulator counts one cycle per instruction plus fixed penalties for branches, multi-cycle instructions,
calls and returns; it does not model pipeline stalls, cache misses or DDR wait states. See
research/cycle-model.md for what that means for the numbers.

Needs ~/opt/bfin-elf with bfin-elf-gcc and bfin-elf-run (research/toolchain-setup.md).
Python 3 standard library only.
"""

import argparse
import bisect
import contextlib
import io
import json
import math
import os
import re
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import opz_build  # noqa: E402
import opz_pack  # noqa: E402
import opz_tables  # noqa: E402

DRIVER_DIR = os.path.join(HERE, "cycles")
HOST_BASE, HOST_SIZE = 0x80000000, 0x100000            # driver code, data, stack: below every slot window
SLOT_REGION = (0x81000000, 0x01000000)                 # all slot windows (sdk/docs/memory-slots.md) and,
                                                       # below them at 0x81000000, the 2 MB FX buffer
# Device memory an engine may read on purpose, mapped (zero-filled) so that it does not count as a wild access:
# L2 SRAM (1 MB, DCPLB-covered on the device; opz_test_l2) and the sample-pack cache (14 x 1,058,400 B at 0x82700000,
# where getSample's slices point). research/memory-map.md, cpu-headroom.md §2.5.
# Our bfin-elf-run with the Blackfin+ 32 x 32 multiply (research/toolchain-setup.md), for --blackfin-plus.
BFP_RUN = os.path.expanduser("~/opt/bfin-elf-bfp/bin/bfin-elf-run")
DEVICE_REGIONS = [(0x08000000, 0x00100000), (0x82700000, 0x00E22000)]
MAX_VOICES = 24
FS, BLOCK = 44100, 64

CPU_HZ = 400_000_000
BLOCK_CYCLES = 580_499
KILL_SOFT, KILL_HARD = 265_000, 290_000
MAX_VOICES_DEVICE = 19                                         # per voice layer: 2,2,2,2,2,3,2,4 (0x815C7ACC)
# The OS's own work inside its timed instrument stage (research/cpu-budget.md, instruction counts of opz.ldr
# 1.2.45 without stalls, so a floor): fixed part, per active instrument track (LFOs, vibrato, voice loop, track
# filter, DC blocker, mix into the buses), the bass track's second filter stage, per sounding voice (Voice::render).
OS_WINDOW_FIXED, OS_PER_TRACK, OS_BASS_EXTRA, OS_PER_VOICE = 1_330, 6_600, 770, 350
INSTRUMENT_TRACKS = 8
OS_WINDOW_CYCLES = (OS_WINDOW_FIXED + INSTRUMENT_TRACKS * OS_PER_TRACK + OS_BASS_EXTRA
                    + MAX_VOICES_DEVICE * OS_PER_VOICE)        # ~61,500
BUDGET_PER_SAMPLE = (KILL_SOFT - OS_WINDOW_CYCLES) / MAX_VOICES_DEVICE / BLOCK       # ~167: steady state
BUDGET_PER_SAMPLE_HARD = (KILL_HARD - OS_WINDOW_CYCLES) / MAX_VOICES_DEVICE / BLOCK  # ~188: any single block
OS_TRACK_FILTER = 2_400       # part of OS_PER_TRACK skipped by plug flag 0x04 (with OS_BASS_EXTRA on the bass track)
OS_SILENT_TRACK = 1_000       # an instrument track silent for 1.45 s: LFOs, vibrato, voice loop, VU only
TRACK_NAMES = ["kick", "snare", "perc", "sample", "bass", "lead", "arp", "chord"]
TRACK_VOICES = [2, 2, 2, 2, 2, 3, 2, 4]                        # 0x815C7ACC; chord 6 with generous_chords
TARGET_PER_SAMPLE = 150                                        # sdk/docs/engine-abi.md rule 5
FX_TARGET_PER_SAMPLE = 600
DRUM_PITCH = 1024                                              # drum tracks: fixed unity rate (engine-abi.md)
# The device's state areas: Voice::init clears 0x118 bytes at +0x48 and the OS keeps its portamento fields after
# them (opz.ldr Voice::render 0x8012e2c0, +0x160..+0x17b); FX records hold 204 bytes. [code, device]
DEVICE_VOICE_STATE_BYTES = 280
DEVICE_FX_STATE_BYTES = 200
NULL_PAGE_REGION = (0x4, 0x4)  # mapping address 4 stops the simulator mapping 128 MB at 0: NULL now faults

# Finding codes of tools/cycles/driver.c ("F" lines): (name, level, what it means)
FINDINGS = {
    1: ("L register", "error", "L%(a)d = 0x%(b)x on return: the OS assumes L = 0 (circular addressing breaks it)"),
    2: ("loop counter", "note", "LC%(a)d = %(b)d on return: a hardware loop was left early (GCC does this for "
                                "`break` in a counted loop); harmless unless that loop's last instruction can run "
                                "again without its LSETUP, which GCC's code does not do"),
    3: ("callee-saved", "error", "%(reg)s = 0x%(b)x on return, not restored (C ABI: R4-R7, P3-P5, FP)"),
    4: ("stack pointer", "error", "SP = 0x%(a)x on return, expected 0x%(b)x"),
    5: ("state", "error", "voice %(a)d: wrote state byte %(b)d, past the device's %(limit)d-byte state area"),
    6: ("output buffer", "error", "wrote %(side)s the 128-value output buffer"),
    7: ("envelope", "error", "voice %(a)d: wrote byte %(b)d of the envelope record (it has %(envsize)d)"),
    8: ("params", "error", "wrote params[%(a)d] (read-only; the OS's smoothed parameters)"),
    9: ("FX input", "error", "wrote the read-only FX input buffer (value %(a)d)"),
    10: ("FX buffer", "error", "wrote past the end of the memRequest buffer (byte %(a)d after it)"),
    11: ("code", "error", "the engine's export table, code or constants changed"),
    12: ("slot window", "error", "wrote 0x%(a)08x, past the end of its .bss (the next slot's engine on the device)"),
    13: ("stack overflow", "error", "used the whole 8 KB test stack"),
    14: ("scratch", "warning", "used %(a)d bytes of the scratch buffer (TE's crush uses 256; the size is unknown)"),
}
ENTRY_NAMES = ("initModule", "init", "onNoteOn", "onNoteOff", "render", "after the case")
SAVED_REGS = ("R4", "R5", "R6", "R7", "P3", "P4", "P5", "FP")
STACK_KEYS = (("module", "initModule"), ("init", "init"), ("on", "onNoteOn"), ("off", "onNoteOff"),
              ("render", "render"))
EXCAUSE = {0x21: "undefined instruction", 0x22: "illegal instruction combination", 0x23: "data access protection "
           "violation", 0x24: "misaligned data access", 0x25: "unrecoverable event", 0x26: "data CPLB miss",
           0x2a: "misaligned instruction", 0x2b: "instruction protection violation", 0x2c: "instruction CPLB miss",
           0x2e: "supervisor resource used"}

#                 P1   P2  att  dec  sus  rel fx1 fx2 filt res  pan  lvl por ldp lsp ltg lsh sty
DEFAULT_PARAMS = [128, 128,  0,  64, 200,  64,  0,  0, 128,  0, 128, 200,  0,  0, 64,  0,  0,  0]


# --------------------------------------------------------------------------- cases

def note_pitch(note):
    """render's pitch word: Q32 phase increment per sample at 44.1 kHz."""
    return int(round(440.0 * 2.0 ** ((note - 69) / 12.0) / FS * 2 ** 32)) & 0xffffffff


def params(**kw):
    p = list(DEFAULT_PARAMS)
    for k, v in kw.items():
        p[{"p1": 0, "p2": 1, "attack": 2, "decay": 3, "sustain": 4, "release": 5, "pan": 10, "level": 11}[k]] = v
    return p


def case(name, p, notes, track, blocks, velocity=100, warmup=0, release_at=0, group=None, p1_to=None, knobs=0,
         glide=0, glide_flip=0, pitch=None, resend=0, raw=None, stream=None):
    """One benchmark case. p1_to: P1 glides to this byte. knobs: P1 and P2 sweep their full range as triangles
    (periods knobs and 3*knobs/2+1 blocks), a new value every block while the notes play. glide: every voice's
    pitch moves by glide/4096 of itself per block, as a pitch slope; glide_flip: it turns round every glide_flip
    blocks. pitch: render's pitch words instead of the notes' (one per note). resend (FX): every resend blocks the
    next of the notes arrives, and halfway between the OS's re-send onNoteOn(note, 0, NULL); (instruments): every
    resend blocks the next voice is struck again, after the host's fade if it is still sounding. raw: (P1, P2) as
    32-bit parameter words instead of knob bytes. stream: per-block rows of 20 words (18 params, then pitch at the
    block's start and end, or the FX pitch bend), from block `warmup` on, repeated (tools/opz_params.py)."""
    if len(notes) > MAX_VOICES:
        raise ValueError("at most %d voices" % MAX_VOICES)
    drum = track < 4
    return {"name": name, "group": group or name, "params": p, "notes": list(notes), "track": track,
            "velocity": velocity, "blocks": blocks, "warmup": warmup, "release_at": release_at,
            "p1_to": 0xffff if p1_to is None else p1_to, "knobs": knobs, "glide": glide, "glide_flip": glide_flip,
            "resend": resend, "raw": None if raw is None else [int(v) & 0xffffffff for v in raw],
            "stream": None if stream is None else [[int(v) & 0xffffffff for v in r] for r in stream],
            "pitch": list(pitch) if pitch else [DRUM_PITCH if drum or n == 255 else note_pitch(n) for n in notes]}


CHORD = [48, 55, 60, 64]                                  # four voices of one synth track, C3 G3 C4 E4
STRESS_NOTES = [36 + 2 * i for i in range(MAX_VOICES_DEVICE)]

SNAP_KEYS = ["clap 909", "clap 808", "tight clap", "big clap", "handclaps", "crowd", "snap", "bright snap",
             "rim 909", "rim 808", "side stick", "sticks", "clave", "woodblock", "shaker", "maracas", "tambourine",
             "tamb shake", "cabasa", "brush hit", "brush swish", "slap", "guiro", "gated clap"]

KIT_KEYS = ["kick deep", "kick punchy", "kick boom", "kick hard", "snare 808", "snare tight", "clap", "rim",
            "closed hat", "pedal hat", "open hat", "cymbal", "low tom", "mid tom", "high tom", "conga", "cowbell",
            "clave", "shaker", "zap", "blip", "noise", "hi-q", "metal"]
FMPERC_KEYS = ["deep kick", "punch kick", "boom kick", "hard kick", "snare", "tight snare", "crack", "rim",
               "closed hat", "pedal hat", "open hat", "cymbal", "low tom", "mid tom", "high tom", "syn tom",
               "low wood", "high wood", "clave", "cowbell", "bell", "glock", "zap", "laser"]

HATS_KEYS = ["closed hat", "tight hat", "pedal hat", "half-open", "open hat", "open long", "dark closed",
             "dark open", "glass closed", "glass open", "ring closed", "ring open", "tick", "ride", "ride bell",
             "dark ride", "crash", "big crash", "splash", "china", "china short", "sizzle", "swell", "bell"]


EIGHTBIT_KEYS = ["kick tri", "kick pulse", "boom", "kick hard", "snare tri", "snare tight", "snare metallic", "rim",
                 "closed hat", "closed metallic", "open hat", "open metallic", "low tom", "mid tom", "high tom",
                 "pulse tom", "blip", "chirp", "coin", "zap", "laser", "power-up", "explosion", "jump"]


def instrument_grid(blocks):
    out = []
    for p1 in (0, 128, 255):
        for p2 in (0, 128, 255):
            out.append(case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2), CHORD, 4, blocks))
    return out


def knob_cases(is_fx, blocks, notes=None, track=None, **kw):
    """Knobs moving every block while notes play: a fast (8-block) and a slow (40-block) sweep of P1 and P2.
    Every engine should have these; they catch state that breaks when parameters change mid-note."""
    if notes is None:
        notes = [255] if is_fx else CHORD
    if track is None:
        track = 8 if is_fx else 4
    warmup = kw.pop("warmup", 8 if is_fx else 0)
    return [case("knobs moving every block, period %d" % period, params(), notes, track, max(blocks, 3 * period),
                 warmup=warmup, knobs=period, group="knobs", **kw) for period in (8, 40)]


def fx_grid(blocks):
    return [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2), [255], 8, blocks, warmup=8)
            for p1 in (0, 255) for p2 in (0, 128, 255)]


def drum_grid(blocks):
    """Drum-only engines without cases of their own: the P1/P2 grid on the kick track, four keys across the range,
    with the drum envelope's hold and decay at full (sustain and release 255) so the hits ring through the case."""
    return [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2, sustain=255, release=255), [53, 60, 67, 76], 0,
                 blocks, velocity=127) for p1 in (0, 128, 255) for p2 in (0, 128, 255)]


def cases_for(name, is_fx, quick=False, drum=False):
    """The benchmark cases for an engine. Unknown engines get a P1/P2 grid (on a drum track when drum: a plug whose
    trackmask has drum tracks only)."""
    b = 24 if quick else 100
    if name == "macro":
        out = []
        for p1, shape in ((0, "saw"), (128, "square"), (255, "pulse")):
            for p2 in (0, 128, 255):
                out.append(case("%s (P1 %d), P2 %d" % (shape, p1, p2), params(p1=p1, p2=p2), CHORD, 4, b))
        out.append(case("pulse, P2 255, release from block 20", params(p1=255, p2=255), CHORD, 4, b,
                        release_at=20 if not quick else 8))
        out.append(case("19 voices, pulse, P2 255", params(p1=255, p2=255), STRESS_NOTES, 4, 12 if quick else 40,
                        group="stress"))
        return out + knob_cases(False, b)
    if name == "fm2":
        out = []
        for p1, ratio in ((8, "0.5"), (24, "1"), (184, "14"), (200, "2 drift"), (248, "pi")):
            for p2 in (0, 191, 255):
                out.append(case("ratio %s (P1 %d), P2 %d" % (ratio, p1, p2), params(p1=p1, p2=p2), CHORD, 4, b,
                                velocity=127))
        out.append(case("ratio pi, P2 255, release from block 20", params(p1=248, p2=255), CHORD, 4, b,
                        velocity=127, release_at=20 if not quick else 8))
        out.append(case("19 voices, ratio pi, P2 255", params(p1=248, p2=255), STRESS_NOTES, 4, 12 if quick else 40,
                        velocity=127, group="stress"))
        return out
    if name == "fold":
        out = [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2), CHORD, 4, b)
               for p1 in (0, 128, 255) for p2 in (0, 255)]
        out.append(case("pluck P1 255, P2 255, decay 60, sustain 0", params(p1=255, p2=255, decay=60, sustain=0),
                        CHORD, 4, b))
        out.append(case("P1 255, P2 255, release from block 20", params(p1=255, p2=255), CHORD, 4, b,
                        release_at=20 if not quick else 8))
        out.append(case("19 voices, P1 255, P2 255", params(p1=255, p2=255), STRESS_NOTES, 4, 12 if quick else 40,
                        group="stress"))
        return out
    if name == "dx":
        # P1 = 8 x patch + 4 (the middle of the patch's step); sustain 255 so the host envelope holds
        def dxp(n, p2=128, **kw):
            return params(p1=8 * n + 4, p2=p2, attack=0, sustain=255, **kw)
        out = []
        for n, patch in ((0, "TINE EP, alg 5"), (5, "SOLID BASS, alg 16"), (11, "BRASS, alg 22"),
                         (17, "STRINGS, alg 2"), (21, "ORGAN, alg 32"), (9, "SUB BASS, 3 operators")):
            out.append(case("patch %d %s" % (n, patch), dxp(n), CHORD, 4, b, velocity=127))
        out.append(case("patch 11 BRASS, P2 255", dxp(11, p2=255), CHORD, 4, b, velocity=127))
        out.append(case("patch 11 BRASS, P2 0", dxp(11, p2=0), CHORD, 4, b, velocity=127))
        out.append(case("patch 17 STRINGS, release from block 20", dxp(17), CHORD, 4, b, velocity=127,
                        release_at=20 if not quick else 8))
        out.append(case("19 voices, patch 11 BRASS", dxp(11), STRESS_NOTES, 4, 12 if quick else 40,
                        velocity=127, group="stress"))
        out.append(case("11 voices, patch 0 TINE EP", dxp(0), STRESS_NOTES[:11], 4, 12 if quick else 40,
                        velocity=127, group="stress"))
        return out
    if name == "pluck":
        out = []
        for p1, material in ((0, "nylon"), (127, "stiff"), (255, "metal")):
            for p2 in (0, 150, 255):
                out.append(case("%s (P1 %d), P2 %d" % (material, p1, p2), params(p1=p1, p2=p2), CHORD, 4, b,
                                velocity=127))
        out.append(case("low notes E1 C1 F0 C0 (sub-rate), P1 255, P2 255", params(p1=255, p2=255),
                        [28, 24, 17, 12], 4, b, velocity=127))
        out.append(case("high notes C6 C7 E7 C8, P1 255, P2 255", params(p1=255, p2=255), [84, 96, 100, 108], 4,
                        b, velocity=127))
        out.append(case("metal, P2 255, release from block 20", params(p1=255, p2=255), CHORD, 4, b, velocity=127,
                        release_at=20 if not quick else 8))
        # 13 voices: every synth voice of the OP-Z (bass 2, lead 3, arp 2, chord 6 with generous chords)
        out.append(case("13 voices, metal, P2 255", params(p1=255, p2=255), [36 + 2 * i for i in range(13)], 4,
                        12 if quick else 40, velocity=127, group="stress"))
        return out
    if name == "modal":
        out = []
        for p1, structure in ((0, "string"), (51, "marimba"), (102, "bar"), (153, "membrane"), (204, "bell"),
                              (255, "glass")):
            for p2 in (40, 191, 255):
                out.append(case("%s (P1 %d), P2 %d" % (structure, p1, p2), params(p1=p1, p2=p2), CHORD, 4, b,
                                velocity=127))
        out.append(case("high notes C6 C7 E7 C8, string, P2 191", params(p1=0, p2=191), [84, 96, 100, 108], 4, b,
                        velocity=127))
        out.append(case("low notes E1 C1 F0 C0, bell, P2 191", params(p1=204, p2=191), [28, 24, 17, 12], 4, b,
                        velocity=127))
        out.append(case("bell, P2 255 (bowed), release from block 20", params(p1=204, p2=255), CHORD, 4, b,
                        velocity=127, release_at=20 if not quick else 8))
        out.append(case("bowed + brightest: string, P2 255, velocity 127", params(p1=0, p2=255), CHORD, 4, b,
                        velocity=127))
        out.append(case("bell pad: bell, P2 240, attack 60", params(p1=204, p2=240, attack=60, sustain=255), CHORD,
                        4, b, velocity=100))
        # 13 voices: every synth voice of the OP-Z (bass 2, lead 3, arp 2, chord 6), all struck in the same block
        out.append(case("13 voices, bell, P2 191", params(p1=204, p2=191), [36 + 2 * i for i in range(13)], 4,
                        12 if quick else 40, velocity=127, group="stress"))
        # every voice of the OP-Z at once, all starting in the same block: the worst block is the first
        out.append(case("19 voices, string, P2 255 (bowed)", params(p1=0, p2=255), STRESS_NOTES, 4,
                        12 if quick else 40, velocity=127, group="stress"))
        out.append(case("19 voices, glass, P2 191", params(p1=255, p2=191), STRESS_NOTES, 4, 12 if quick else 40,
                        velocity=127, group="stress"))
        out.append(case("19 voices, P1 glide 0 -> 255, P2 255", params(p1=0, p2=255), STRESS_NOTES, 4,
                        12 if quick else 40, velocity=127, p1_to=255, group="stress"))
        return out + knob_cases(False, b, velocity=127) + knob_cases(False, b, notes=STRESS_NOTES, velocity=127)
    if name == "super":
        out = [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2), CHORD, 4, b)
               for p1 in (0, 128, 255) for p2 in (0, 160, 255)]
        out.append(case("high notes C6 G6 C7 E7, P1 255, P2 255", params(p1=255, p2=255), [n + 36 for n in CHORD], 4,
                        b))
        out.append(case("P1 128, P2 255, release from block 20", params(p1=128, p2=255), CHORD, 4, b,
                        release_at=20 if not quick else 8))
        out.append(case("19 voices, P1 128, P2 255", params(p1=128, p2=255), STRESS_NOTES, 4, 12 if quick else 40,
                        group="stress"))
        return out
    if name == "table":
        out = []
        for p1 in (0, 128, 255):
            for p2 in (0, 128, 255):
                out.append(case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2), CHORD, 4, b, velocity=127))
        out.append(case("P1 100, P2 150 (between waves and banks)", params(p1=100, p2=150), CHORD, 4, b,
                        velocity=127))
        out.append(case("low notes C0 C1 F1 C2 (256-sample mip), P1 255, P2 255", params(p1=255, p2=255),
                        [12, 24, 29, 36], 4, b, velocity=127))
        out.append(case("high notes C6 C7 C8 G8 (small mips), P1 200, P2 200", params(p1=200, p2=200),
                        [84, 96, 108, 115], 4, b, velocity=127))
        out.append(case("top notes 124-127 (pitch held at 11 kHz: the longest blend)", params(p1=200, p2=200),
                        [124, 125, 126, 127], 4, b, velocity=127))
        # under four mip boundaries (16 kHz / 127, 63, 31, 15), 0.6 % per block, turning every 2 blocks: the block
        # that crosses a boundary (either way) changes the mip and crossfades (two lines blended and read), so
        # every other block is a crossfade
        edges = [int(16000.0 / h * 0.991 / FS * 2 ** 32) for h in (127, 63, 31, 15)]
        out.append(case("vibrato across mip boundaries (a crossfade every other block)", params(p1=100, p2=150),
                        [47, 59, 71, 83], 4, b, velocity=127, glide=25, glide_flip=2, pitch=edges))
        out.append(case("glide C2 up 5 octaves (every mip on the way)", params(p1=128, p2=200), [36, 40, 43, 48],
                        4, b, velocity=127, glide=147))
        out.append(case("P1 255, P2 255, release from block 20", params(p1=255, p2=255), CHORD, 4, b,
                        velocity=127, release_at=20 if not quick else 8))
        out.append(case("19 voices, P1 100, P2 150", params(p1=100, p2=150), STRESS_NOTES, 4, 12 if quick else 40,
                        velocity=127, group="stress"))
        out.append(case("19 voices C5-F#6, knobs moving every block", params(), [72 + i for i in range(19)], 4,
                        24 if quick else 40, velocity=127, knobs=8, group="stress"))
        return out + knob_cases(False, b, velocity=127)
    if name == "vowel":
        # track 5 (lead): one singer per voice; track 7 (chord): two singers per voice (the worst case)
        out = []
        for p1, vow in ((0, "hum"), (109, "ah"), (182, "ee"), (255, "er")):
            for p2 in (0, 128):
                out.append(case("%s (P1 %d), P2 %d, lead" % (vow, p1, p2), params(p1=p1, p2=p2), CHORD, 5, b,
                                velocity=127))
        out.append(case("ah, P2 220 (breath), lead", params(p1=109, p2=220), CHORD, 5, b, velocity=127))
        out.append(case("ah, P2 255 (whisper), lead", params(p1=109, p2=255), CHORD, 5, b, velocity=127))
        for p2 in (128, 220):
            out.append(case("ah, P2 %d, chord (two singers)" % p2, params(p1=109, p2=p2), CHORD, 7, b,
                            velocity=127))
        out.append(case("low notes C0 F0 C1 E1, chord, P2 0", params(p1=109, p2=0), [12, 17, 24, 28], 7, b,
                        velocity=127))
        out.append(case("high notes C7 E7 G7 C8, chord, P2 191", params(p1=182, p2=191), [96, 100, 103, 108], 7,
                        b, velocity=127))
        out.append(case("ah, P2 220, chord, release from block 20", params(p1=109, p2=220), CHORD, 7, b,
                        velocity=127, release_at=20 if not quick else 8))
        out.append(case("19 voices, chord (two singers), P2 220", params(p1=109, p2=220), STRESS_NOTES, 7,
                        12 if quick else 40, velocity=127, group="stress"))
        out.append(case("19 voices, lead, P2 128", params(p1=109, p2=128), STRESS_NOTES, 5, 12 if quick else 40,
                        velocity=127, group="stress"))
        out.append(case("chord (two singers), knobs moving every block, period 8", params(), CHORD, 7, max(b, 24),
                        knobs=8, group="knobs"))
        return out + knob_cases(False, b)
    if name == "sync":
        # the cost follows the number of discontinuities per block: slave wraps (ratio x pitch, capped at 10 kHz)
        # plus resets (pitch); P2 above 128 adds the per-sample noise
        out = []
        for p1 in (0, 80, 160, 255):
            for p2 in (0, 128, 255):
                out.append(case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2), CHORD, 5, b, velocity=127))
        out.append(case("low notes C0 C1 F1 C2, P1 255, P2 255", params(p1=255, p2=255), [12, 24, 29, 36], 4, b,
                        velocity=127))
        out.append(case("high notes C6 G6 C7 E7, P1 255, P2 255 (slave at the 10 kHz cap)", params(p1=255, p2=255),
                        [84, 91, 96, 100], 5, b, velocity=127))
        out.append(case("top notes 124-127 (master at 11 kHz, a reset every 4 samples), P1 255, P2 255",
                        params(p1=255, p2=255), [124, 125, 126, 127], 5, b, velocity=127))
        out.append(case("C5 E5 G5 B5, P1 200 (slave at the cap, master 520-990 Hz), P2 255", params(p1=200, p2=255),
                        [72, 76, 79, 83], 5, b, velocity=127))
        out.append(case("glide C2 up 5 octaves, P1 160, P2 255", params(p1=160, p2=255), [36, 40, 43, 48], 5, b,
                        velocity=127, glide=147))
        out.append(case("P1 160, P2 255, release from block 20", params(p1=160, p2=255), CHORD, 5, b, velocity=127,
                        release_at=20 if not quick else 8))
        out.append(case("19 voices started in one block, P1 160, P2 255", params(p1=160, p2=255), STRESS_NOTES, 5,
                        12 if quick else 40, velocity=127, group="stress"))
        out.append(case("19 voices C5-F#6, P1 255, P2 255 (slave at the cap)", params(p1=255, p2=255),
                        [72 + i for i in range(19)], 5, 12 if quick else 40, velocity=127, group="stress"))
        out.append(case("19 voices C5-F#6, knobs moving every block", params(), [72 + i for i in range(19)], 5,
                        24 if quick else 40, velocity=127, knobs=8, group="stress"))
        return out + knob_cases(False, b, velocity=127)
    if name == "pd":
        # P2 points of the eight waves, and the middles between neighbours (both waves computed: the dear blocks)
        waves = ((0, "saw"), (36, "square"), (73, "pulse"), (109, "double sine"), (146, "saw-pulse"),
                 (182, "resonance I"), (219, "resonance II"), (255, "resonance III"))
        out = []
        for p2, wave in waves:
            for p1 in (128, 255):
                out.append(case("%s (P2 %d), P1 %d" % (wave, p2, p1), params(p1=p1, p2=p2), CHORD, 4, b,
                                velocity=127))
        for p2, pair in ((18, "saw/square"), (91, "pulse/double sine"), (128, "double sine/saw-pulse"),
                         (164, "saw-pulse/res I"), (200, "res I/res II"), (237, "res II/res III")):
            out.append(case("crossfade %s (P2 %d), P1 255" % (pair, p2), params(p1=255, p2=p2), CHORD, 4, b,
                            velocity=127))
        out.append(case("res II/res III (P2 237), P1 30 (sines crossfading in)", params(p1=30, p2=237), CHORD, 4,
                        b, velocity=1))
        out.append(case("P1 0 (sine), P2 128, velocity 1 (no DCW)", params(p1=0, p2=128), CHORD, 4, b, velocity=1))
        out.append(case("pluck: decay 0, sustain 0, P1 0, P2 0 (DCW sweeps down)", params(p1=0, p2=0, decay=0,
                                                                                            sustain=0),
                        CHORD, 4, b, velocity=127))
        out.append(case("high notes C6 G6 C7 E7, crossfade P2 237", params(p1=255, p2=237), [n + 36 for n in CHORD],
                        4, b, velocity=127))
        out.append(case("low notes C0 C1 F1 C2, crossfade P2 91", params(p1=255, p2=91), [12, 24, 29, 36], 4, b,
                        velocity=127))
        out.append(case("glide C2 up 5 octaves, crossfade P2 91", params(p1=200, p2=91), [36, 40, 43, 48], 4, b,
                        velocity=127, glide=147))
        out.append(case("crossfade P2 237, release from block 20", params(p1=255, p2=237), CHORD, 4, b,
                        velocity=127, release_at=20 if not quick else 8))
        out.append(case("19 voices, crossfade res II/res III (P2 237)", params(p1=255, p2=237), STRESS_NOTES, 4,
                        12 if quick else 40, velocity=127, group="stress"))
        out.append(case("19 voices, crossfade pulse/double sine (P2 91)", params(p1=255, p2=91), STRESS_NOTES, 4,
                        12 if quick else 40, velocity=127, group="stress"))
        out.append(case("19 voices, knobs moving every block", params(), STRESS_NOTES, 4, 24 if quick else 40,
                        velocity=127, knobs=8, group="stress"))
    if name == "ensemble":
        # chord track (7): where it is meant to play; every case at velocity 127
        out = []
        for p1, reg in ((0, "cello"), (128, "viola"), (255, "full")):
            for p2 in (0, 76, 153, 255):
                out.append(case("%s (P1 %d), P2 %d" % (reg, p1, p2), params(p1=p1, p2=p2), CHORD, 7, b,
                                velocity=127))
        out.append(case("violin (P1 192), P2 200, bass track (narrow)", params(p1=192, p2=200), CHORD, 4, b,
                        velocity=127))
        out.append(case("low notes C0 F0 C1 E1, full, P2 255", params(p1=255, p2=255), [12, 17, 24, 28], 7, b,
                        velocity=127))
        out.append(case("high notes C6 G6 C7 E7, full, P2 255", params(p1=255, p2=255), [n + 36 for n in CHORD], 7,
                        b, velocity=127))
        out.append(case("top notes 120-127 (pitch at the 11 kHz cap), full, P2 255", params(p1=255, p2=255),
                        [120, 123, 125, 127], 7, b, velocity=127))
        out.append(case("full, P2 255, release from block 20", params(p1=255, p2=255), CHORD, 7, b, velocity=127,
                        release_at=20 if not quick else 8))
        out.append(case("string pad: attack 90, P1 192, P2 170", params(p1=192, p2=170, attack=90, sustain=255),
                        CHORD, 7, b, velocity=127))
        out.append(case("vibrato glide on the chord, full, P2 255", params(p1=255, p2=255), CHORD, 7, b,
                        velocity=127, glide=20, glide_flip=4))
        out.append(case("6-voice chord C3-E5, every note-on in one block, full, P2 255", params(p1=255, p2=255),
                        [48, 52, 55, 60, 64, 67], 7, b, velocity=127))
        out.append(case("19 voices, full, P2 255", params(p1=255, p2=255), STRESS_NOTES, 7, 12 if quick else 40,
                        velocity=127, group="stress"))
        out.append(case("19 voices C5-F#6, full, P2 255", params(p1=255, p2=255), [72 + i for i in range(19)], 7,
                        12 if quick else 40, velocity=127, group="stress"))
        out.append(case("19 voices, knobs moving every block, period 8", params(), STRESS_NOTES, 7,
                        24 if quick else 40, velocity=127, knobs=8, group="stress"))
        out.append(case("chord track, knobs moving every block, period 8", params(), CHORD, 7, max(b, 24),
                        velocity=127, knobs=8, group="knobs"))
        return out + knob_cases(False, b)
    if name == "swarm":
        # P1 0-96 particles only, 96-224 both layers (the saws come in one by one), 255 all sixteen saws alone;
        # P2 160 wide cluster, 255 inharmonic (saws up to 5.1 x the note: the most wraps)
        out = []
        for p1 in (0, 64, 128, 160, 192, 255):
            for p2 in (0, 160, 255):
                out.append(case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2), CHORD, 4, b, velocity=127))
        out.append(case("high notes C6 G6 C7 E7, P1 160, P2 255", params(p1=160, p2=255), [n + 36 for n in CHORD],
                        4, b, velocity=127))
        out.append(case("high notes C6 G6 C7 E7, P1 255, P2 255", params(p1=255, p2=255), [n + 36 for n in CHORD],
                        4, b, velocity=127))
        out.append(case("top notes 108 115 120 127, P1 192, P2 200", params(p1=192, p2=200), [108, 115, 120, 127],
                        4, b, velocity=127))
        out.append(case("low notes C0 F0 C1 E1, P1 160, P2 255", params(p1=160, p2=255), [12, 17, 24, 28], 4, b,
                        velocity=127))
        out.append(case("P1 160, P2 255, release from block 20", params(p1=160, p2=255), CHORD, 4, b, velocity=127,
                        release_at=20 if not quick else 8))
        out.append(case("19 voices, densest: P1 255, P2 255", params(p1=255, p2=255), STRESS_NOTES, 4,
                        12 if quick else 40, velocity=127, group="stress"))
        out.append(case("19 voices, densest: P1 255, P2 160", params(p1=255, p2=160), STRESS_NOTES, 4,
                        12 if quick else 40, velocity=127, group="stress"))
        out.append(case("19 voices, both layers: P1 176, P2 255", params(p1=176, p2=255), STRESS_NOTES, 4,
                        12 if quick else 40, velocity=127, group="stress"))
        out.append(case("19 voices C5-F#6, knobs moving every block", params(), [72 + i for i in range(19)], 4,
                        24 if quick else 40, velocity=127, knobs=8, group="stress"))
        return out + knob_cases(False, b, velocity=127)
    if name == "chip":
        # P1: 8 waves of 32 (pulses, duty cycling, triangle, saw, noise, metallic noise); P2: 0 plain, 1-31 vibrato,
        # arpeggios of 32 (speed in the low 5 bits: +24 = 60 Hz), 224-255 sweeps
        out = []
        for p1, wave in ((0, "pulse 12.5 %"), (64, "pulse 50 %"), (96, "duty cycling"), (128, "triangle"),
                         (160, "saw"), (192, "noise"), (224, "metallic noise")):
            for p2, mode in ((0, "plain"), (88, "major arp 60 Hz")):
                out.append(case("%s (P1 %d), %s (P2 %d)" % (wave, p1, mode, p2), params(p1=p1, p2=p2), CHORD, 4, b,
                                velocity=127))
        out.append(case("triangle, vibrato (P2 31)", params(p1=128, p2=31), CHORD, 4, b, velocity=127))
        out.append(case("noise, sweep down 30 oct/s (P2 236)", params(p1=192, p2=236), CHORD, 4, b, velocity=127))
        out.append(case("metallic noise, sweep up (P2 252)", params(p1=224, p2=252), CHORD, 4, b, velocity=127))
        out.append(case("high notes C7 E7 G7 C8, noise, octave arp", params(p1=192, p2=56), [96, 100, 103, 108], 4,
                        b, velocity=127))
        out.append(case("high notes C7 E7 G7 C8, metallic, seventh arp", params(p1=224, p2=216), [96, 100, 103, 108],
                        4, b, velocity=127))
        out.append(case("low notes C0 F0 C1 E1, pulse 12.5 %", params(p1=0, p2=0), [12, 17, 24, 28], 4, b,
                        velocity=127))
        out.append(case("pulse 25 %, minor arp, release from block 20", params(p1=32, p2=120), CHORD, 4, b,
                        velocity=127, release_at=20 if not quick else 8))
        out.append(case("noise, release from block 20", params(p1=192, p2=0), CHORD, 4, b, velocity=127,
                        release_at=20 if not quick else 8))
        out.append(case("19 voices, metallic noise, seventh arp 60 Hz", params(p1=224, p2=216), STRESS_NOTES, 4,
                        12 if quick else 40, velocity=127, group="stress"))
        out.append(case("19 voices, noise, octave arp 60 Hz", params(p1=192, p2=56), STRESS_NOTES, 4,
                        12 if quick else 40, velocity=127, group="stress"))
        out.append(case("19 voices C5-F#6, knobs moving every block", params(), [72 + i for i in range(19)], 4,
                        24 if quick else 40, velocity=127, knobs=8, group="stress"))
        return out + knob_cases(False, b)
    if name == "kit":
        out = []
        for p2 in ((255,) if quick else (128, 255)):
            for k, key in enumerate(KIT_KEYS):
                out.append(case("key %d %s, P2 %d" % (k + 1, key, p2), params(p1=128, p2=p2), [53 + k], 0,
                                24 if quick else 200, velocity=127, group="key %d" % (k + 1)))
        for k in (10, 11, 23):
            out.append(case("8 voices, key %d %s, P2 255" % (k + 1, KIT_KEYS[k]), params(p1=128, p2=255),
                            [53 + k] * 8, 0, 12 if quick else 40, velocity=127, group="stress"))
        return out
    if name == "fmperc":
        # drum tracks: every key once at P1 128 / 255 (and P2 255: the longest decays), then many hits in one block
        # (8 voices, every key family, octaves up and down), a release (note-off is a no-op) and the knob cases.
        # Every block of a hit costs the same (no per-hit setup in render), so a retrigger's first block is any block.
        n = 24 if quick else 200
        out = []
        for p1, p2 in (((255, 255),) if quick else ((128, 128), (255, 255))):
            for k, key in enumerate(FMPERC_KEYS):
                out.append(case("key %d %s, P1 %d, P2 %d" % (k + 1, key, p1, p2), params(p1=p1, p2=p2), [53 + k],
                                0, n, velocity=127, group="key %d" % (k + 1)))
        m = 12 if quick else 40
        out.append(case("8 hits in one block: 8 keys, P1 255, P2 255", params(p1=255, p2=255),
                        [53, 57, 61, 64, 65, 69, 73, 76], 0, m, velocity=127, group="stress"))
        out.append(case("8 hits in one block: open hat x 8, P2 255", params(p1=200, p2=255), [63] * 8, 2, m,
                        velocity=127, group="stress"))
        out.append(case("8 hits in one block: octaves -2 .. +2, P2 255", params(p1=128, p2=255),
                        [5, 29, 77, 101, 9, 33, 80, 100], 3, m, velocity=127, group="stress"))
        out.append(case("bell, released at block 4, P2 255", params(p1=128, p2=255), [73], 1, m, velocity=100,
                        release_at=4, group="release"))
    if name == "mallet":
        # drum tracks: the pitch is the note (MIDI 53-76 at octave 0, +-24 per octave press); one voice per case
        # unless said, as kit's
        out = []
        n = 24 if quick else 100
        for p1, mat in ((0, "wood"), (32, "marimba"), (64, "xylophone"), (96, "vibes"), (128, "glock"),
                        (159, "steel drum"), (191, "bell"), (223, "gong"), (255, "glass")):
            for p2 in ((128,) if quick else (0, 128, 255)):
                out.append(case("%s (P1 %d), P2 %d, F3 + E5" % (mat, p1, p2), params(p1=p1, p2=p2), [53, 76], 2, n,
                                velocity=127, group=mat))
        for k in range(24):                                 # every key: gong keeps all ten partials in the band
            out.append(case("key %d (note %d), gong, P2 128" % (k + 1, 53 + k), params(p1=223, p2=128), [53 + k], 0,
                            12 if quick else 40, velocity=127, group="keys"))
        out.append(case("octaves -2 / -1 / +1 / +2 (notes 5 29 101 125), bell, P2 255", params(p1=191, p2=255),
                        [5, 29, 101, 125], 0, n, velocity=127, group="octaves"))
        out.append(case("soft hits (velocity 20), marimba, P2 128", params(p1=32, p2=128), [53, 60, 67, 76], 2, n,
                        velocity=20))
        # every drum voice of the OP-Z (four tracks x 2) struck in the same block: the worst block is the first
        out.append(case("8 hits in one block, gong, P2 255", params(p1=223, p2=255), [53 + 3 * i for i in range(8)], 0,
                        12 if quick else 40, velocity=127, group="stress"))
        out.append(case("8 hits in one block, wood, P2 0", params(p1=0, p2=0), [53 + 3 * i for i in range(8)], 0,
                        12 if quick else 40, velocity=127, group="stress"))
        # fast retriggers: the two voices of one drum track struck in turn every block / every other block, as a
        # 32nd-note roll at ~170 BPM / a 16th-note roll at ~340 BPM would (the host fades a voice still sounding)
        out.append(case("retrigger every block, 2 voices, glock, P2 255", params(p1=128, p2=255), [64, 64], 1, n,
                        velocity=127, resend=1, group="retrigger"))
        out.append(case("retrigger every 2 blocks, 2 voices, glass, P2 128", params(p1=255, p2=128), [60, 67], 1, n,
                        velocity=127, resend=2, group="retrigger"))
        out.append(case("retrigger every 3 blocks, 2 voices, wood, P2 64", params(p1=0, p2=64), [53, 55], 1, n,
                        velocity=110, resend=3, group="retrigger"))
        # long rings at the longest decay
        out.append(case("long ring, gong, P2 255, 2 voices", params(p1=223, p2=255), [41, 53], 2,
                        60 if quick else 600, velocity=127, group="long"))
        out.append(case("long ring, bell, P2 255, 2 voices", params(p1=191, p2=255), [60, 72], 2,
                        60 if quick else 600, velocity=127, group="long"))
        # the knobs are read when a hit starts: hits keep coming while both knobs move every block
        out.append(case("knobs moving every block, period 8, a hit every 2 blocks", params(), [53, 57, 60, 64], 0,
                        max(b, 24), velocity=127, knobs=8, resend=2, group="knobs"))
    if name == "hats":
        out = []
        for p1, p2 in (((255, 255),) if quick else ((128, 128), (255, 255), (0, 0))):
            for k, key in enumerate(HATS_KEYS):
                out.append(case("key %d %s, P1 %d, P2 %d" % (k + 1, key, p1, p2), params(p1=p1, p2=p2), [53 + k], 0,
                                24 if quick else 200, velocity=127, group="key %d" % (k + 1)))
        # every key at once, octave up (the most edges per block), all hits starting in the same block
        out.append(case("24 voices, every key, octave +2, P1 255, P2 255", params(p1=255, p2=255),
                        [101 + k for k in range(24)], 0, 12 if quick else 40, velocity=127, group="stress"))
        out.append(case("8 voices (4 drum tracks x 2), glass open, P1 255, P2 255", params(p1=255, p2=255),
                        [62] * 8, 0, 12 if quick else 40, velocity=127, group="stress"))
        out.append(case("19 voices, sizzle, octave +2, P1 255, P2 255", params(p1=255, p2=255), [122] * 19, 0,
                        12 if quick else 40, velocity=127, group="stress"))
        # an open hat and the closed hat that chokes it, struck in the same block, on track 2
        out.append(case("choke: open hat cut by a closed hat", params(), [57, 53], 2, 24 if quick else 60,
                        velocity=127, group="choke"))
        out.append(case("swell (rise 0.9 s), P2 255", params(p1=128, p2=255), [75], 0, 24 if quick else 200,
                        velocity=127, group="swell"))
        out.append(case("out-of-range notes 0, 127 and velocity 1", params(), [0, 127], 3, 24 if quick else 60,
                        velocity=1, group="edge"))
    if name == "8bit":
        out = []
        for p2 in ((0, 255) if quick else (0, 128, 255)):
            for k, key in enumerate(EIGHTBIT_KEYS):
                out.append(case("key %d %s, P2 %d" % (k + 1, key, p2), params(p1=128, p2=p2), [53 + k], k % 4,
                                24 if quick else 200, velocity=127, group="key %d" % (k + 1)))
        for p1, p2 in ((0, 0), (255, 0), (255, 255)):
            # octave buttons: -2 and +2 (notes 53 - 48 and 53 + 48 + key), every pitch extreme
            out.append(case("octave -2 / +2, P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2),
                            [5, 101, 24, 120, 18, 114, 21, 117], 0, 12 if quick else 100, velocity=127,
                            group="octaves"))
        hits = [53 + k for k in (0, 4, 8, 10, 11, 19, 20, 22)]       # kick, snare, hats, zap, laser, explosion
        for p2 in (0, 128, 255):
            out.append(case("8 voices (every drum voice), 8 keys hit in one block, P2 %d" % p2,
                            params(p1=128, p2=p2), hits, 0, 12 if quick else 60, velocity=127, group="stress"))
        out.append(case("8 voices, explosion x8, P2 0", params(p1=128, p2=0), [75] * 8, 0, 12 if quick else 60,
                        velocity=127, group="stress"))
        out.append(case("fast retriggers: a new hit every block across 8 voices, P2 0", params(p1=255, p2=0), hits,
                        0, 24 if quick else 100, velocity=127, resend=1, group="stress"))
        out.append(case("fast retriggers: closed/open hats every 2 blocks, P2 255", params(p1=128, p2=255),
                        [61, 63, 62, 64], 3, 24 if quick else 100, velocity=127, resend=2, group="stress"))
    if name == "snap":
        out = []
        n = 24 if quick else 200
        for p2 in ((255,) if quick else (128, 255)):
            for k, key in enumerate(SNAP_KEYS):
                out.append(case("key %d %s, P2 %d" % (k + 1, key, p2), params(p1=128, p2=p2), [53 + k], 1, n,
                                velocity=127, group="key %d" % (k + 1)))
        for k in (5, 22, 3, 8):                           # crowd, guiro: most bursts; big clap; rim (bodies)
            out.append(case("key %d %s, P1 255 (loosest, two more bursts), P2 255" % (k + 1, SNAP_KEYS[k]),
                            params(p1=255, p2=255), [53 + k], 1, n, velocity=127, group="spread"))
            out.append(case("key %d %s, P1 0 (tightest), P2 0" % (k + 1, SNAP_KEYS[k]), params(p1=0, p2=0),
                            [53 + k], 1, n, velocity=127, group="spread"))
        out.append(case("octaves -2 / +2 (rim 909, clave, shaker, clap) and notes 0 / 127", params(p1=200, p2=200),
                        [5, 101, 29, 125, 0, 127, 52, 77], 1, n, velocity=127, group="octaves"))
        for k in (5, 8, 10):                              # many hits in one block, on one track
            out.append(case("8 voices at once, key %d %s, P1 255, P2 255" % (k + 1, SNAP_KEYS[k]),
                            params(p1=255, p2=255), [53 + k] * 8, 1, 12 if quick else 60, velocity=127,
                            group="stress"))
        out.append(case("24 voices at once, every key, P1 255, P2 255", params(p1=255, p2=255),
                        [53 + k for k in range(24)], 1, 12 if quick else 60, velocity=127, group="stress"))
        for period in (1, 3, 7):                          # fast retriggers on sounding voices
            out.append(case("retrigger every %d block(s), 4 voices (crowd, rim 909, tambourine, gated clap)" %
                            period, params(p1=255, p2=255), [58, 61, 69, 76], 1, 24 if quick else 120,
                            velocity=127, resend=period, group="retrigger"))
        return out + knob_cases(False, b, notes=[53, 57, 60, 64], track=0)
    if name == "swirl":
        out = []
        for p2, mode in ((0, "phaser"), (64, "phaser"), (128, "crossfade"), (200, "flanger"), (255, "flanger")):
            out.append(case("%s, P1 128, P2 %d" % (mode, p2), params(p1=128, p2=p2), [255], 8, 16 if quick else 60,
                            warmup=8))
        out.append(case("flanger, P1 0, P2 255", params(p1=0, p2=255), [255], 8, 16 if quick else 60, warmup=8))
        out.append(case("flanger, P1 255, P2 255", params(p1=255, p2=255), [255], 8, 16 if quick else 60, warmup=8))
        out.append(case("note comb A3, P2 200", params(p1=128, p2=200), [57], 8, 16 if quick else 60, warmup=8))
        return out
    if name == "glue":
        n = 16 if quick else 60
        out = [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2), [255], 8, n, warmup=8)
               for p1 in (0, 128, 255) for p2 in (0, 127, 255)]
        out.append(case("duck note C4, P1 48, P2 64", params(p1=48, p2=64), [60], 8, n, velocity=127, warmup=8))
        out.append(case("parallel (pan 64), P1 200, P2 127", params(p1=200, p2=127, pan=64), [255], 8, n, warmup=8))
        return out
    if name == "echo":
        n = 16 if quick else 60
        out = [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2, level=255), [255], 8, n, warmup=8)
               for p1 in (0, 136, 255) for p2 in (0, 128, 255)]
        out.append(case("note A3 (crossfade, pluck), P2 200", params(p1=136, p2=200, level=255), [57], 8, n,
                        velocity=100))
        out.append(case("echo only (pan 255), P1 136, P2 200", params(p1=136, p2=200, pan=255, level=255), [255], 8,
                        n, warmup=8))
        return out
    if name == "plate":
        n = 24 if quick else 60                           # the first 14 blocks clear its 224 KB buffer
        out = [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2), [255], 8, n, warmup=16)
               for p1 in (0, 150, 255) for p2 in (0, 255)]
        out.append(case("freeze (note held), P1 150, P2 150", params(p1=150, p2=150), [60], 8, n, warmup=16))
        out.append(case("size glide from P1 0 to 255, P2 150", params(p1=0, p2=150), [255], 8, n, warmup=16,
                        p1_to=255))
        return out
    if name == "clouds":
        # the first 34 blocks clear its 1 MB ring (dry only); notes: C5 = grains an octave up (interpolated reads),
        # held 276 blocks = frozen; velocity 0 = the OS re-sending a held note with params NULL
        n, w = (60, 40) if quick else (120, 40)
        out = [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2), [255], 8, n, warmup=w)
               for p1 in (0, 128, 200, 255) for p2 in (0, 128, 255)]
        out.append(case("densest, P1 255, P2 255, note C5 (octave up, detuned)", params(p1=255, p2=255), [72], 8, n,
                        warmup=w, group="densest"))
        out.append(case("densest, P1 255, P2 255, note C3 (octave down)", params(p1=255, p2=255), [48], 8, n,
                        warmup=w, group="densest"))
        out.append(case("frozen, P1 255, P2 255, C5 held (freeze from block 310)", params(p1=255, p2=255), [72], 8,
                        420, warmup=320, group="freeze"))
        out.append(case("thaw, P1 255, P2 255, C5 released at block 360", params(p1=255, p2=255), [72], 8, 420,
                        warmup=320, release_at=360, group="freeze"))
        out.append(case("held note re-sent (velocity 0, params NULL), P1 255, P2 200", params(p1=255, p2=200), [67],
                        8, n, velocity=0, warmup=w, group="notes"))
        out.append(case("clearing warm-up (blocks 0-33 clear 32 KB each)", params(p1=255, p2=255), [255], 8, 40,
                        group="warm-up"))
        out.append(case("knobs moving every block, period 8, frozen (C5 held)", params(), [72], 8, 420, warmup=320,
                        knobs=8, group="knobs"))
        return out + knob_cases(True, n, notes=[72], warmup=w)
    if name == "shimmer":
        n = 24 if quick else 60                           # the first 15 blocks clear its 240 KB buffer
        w = 16
        out = [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2), [255], 8, n, warmup=w)
               for p1 in (0, 180, 255) for p2 in (0, 140, 255)]
        out.append(case("max size + max shimmer, wet only, level 255", params(p1=255, p2=255, pan=255, level=255),
                        [255], 8, n, warmup=w))
        out.append(case("intervals G3 C5 C2 C6 C3 arriving, re-sent (vel 0, NULL), P1 200, P2 200",
                        params(p1=200, p2=200), [55, 72, 36, 84, 48], 8, n, warmup=w, resend=8))
        out.append(case("size glide from P1 0 to 255, P2 200", params(p1=0, p2=200), [255], 8, n, warmup=w,
                        p1_to=255))
        out.append(case("clearing warm-up (first 16 blocks, dry only)", params(p1=180, p2=140), [255], 8, 16))
        return out + knob_cases(True, n, warmup=w)
    if name == "repeat":
        # notes: F3 = 1/4, A3 = 1/16, D#5 = 1/128T reversed (the shortest slice, the gate on every few frames);
        # velocity 0 = the OS re-sending a held note with params NULL
        n = 24 if quick else 100
        out = [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2, pan=255), [255], 8, n, warmup=8)
               for p1 in (0, 96, 255) for p2 in (0, 255)]
        out.append(case("held F3 (1/4), P2 128, 50/50", params(p1=0, p2=128), [53], 8, n, warmup=8, group="notes"))
        out.append(case("held D#5 (1/128T reversed), P2 255", params(p1=0, p2=255), [75], 8, n, warmup=8,
                        group="notes"))
        out.append(case("notes F3 A3 D#5 F4 arriving, re-sent (vel 0, NULL), P1 160", params(p1=160, p2=100),
                        [53, 57, 75, 65], 8, n, warmup=8, resend=6, group="notes"))
        out.append(case("knobs moving every block, period 8, D#5 held", params(), [75], 8, n, warmup=8, knobs=8,
                        group="knobs"))
        return out + knob_cases(True, n, notes=[57])
    if name == "spring":
        n = 24 if quick else 100                          # the first 6 blocks clear its 41 KB buffer
        out = [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2, pan=255), [255], 8, n, warmup=8)
               for p1 in (0, 128, 255) for p2 in (0, 255)]
        out.append(case("kicks C2 C6 G3 arriving, re-sent (vel 0, NULL), P1 100, P2 180", params(p1=100, p2=180),
                        [36, 84, 55], 8, n, warmup=8, resend=6, group="notes"))
        out.append(case("tension glide from P1 0 to 255, P2 255", params(p1=0, p2=255), [255], 8, n, warmup=8,
                        p1_to=255))
        out.append(case("clearing warm-up (first 6 blocks, dry only)", params(p1=128, p2=128), [255], 8, 8))
        return out + knob_cases(True, n, notes=[60])
    if name == "lofi":
        n = 24 if quick else 100                          # the first 8 blocks clear its 128 KB ring
        out = [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2, pan=255), [255], 8, n, warmup=10)
               for p1 in (0, 128, 255) for p2 in (0, 127, 200, 255)]
        out.append(case("tape stop F3 held (slowest), P1 255, P2 255", params(p1=255, p2=255), [53], 8, n,
                        warmup=10, group="notes"))
        out.append(case("tape stops E5 F3 A#4 arriving, re-sent (vel 0, NULL), P1 200, P2 140",
                        params(p1=200, p2=140), [76, 53, 70], 8, n, warmup=10, resend=6, group="notes"))
        out.append(case("stop and release (E5 released at block 40), P1 255, P2 255", params(p1=255, p2=255), [76],
                        8, n, warmup=10, release_at=40, group="notes"))
        out.append(case("clearing warm-up (first 8 blocks, dry only)", params(p1=128, p2=128), [255], 8, 10))
        out.append(case("knobs moving every block, period 8, E5 held", params(), [76], 8, n, warmup=10, knobs=8,
                        group="knobs"))
        return out + knob_cases(True, n, notes=[60], warmup=10)
    if name == "shift":
        n = 24 if quick else 100
        out = [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2, pan=255), [255], 8, n, warmup=8)
               for p1 in (0, 182, 255) for p2 in (0, 128, 255)]
        out.append(case("notes A2 C5 E3 arriving, re-sent (vel 0, NULL), P2 200", params(p1=60, p2=200),
                        [45, 72, 52], 8, n, warmup=8, resend=6, group="notes"))
        out.append(case("top note 127 held (12.5 kHz carrier), P2 255", params(p1=0, p2=255), [127], 8, n,
                        warmup=8, group="notes"))
        return out + knob_cases(True, n, notes=[57])
    if name == "harmonizer":
        n = 24 if quick else 100                          # the first 4 blocks clear its 64 KB buffer
        out = [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2, pan=255), [255], 8, n, warmup=8)
               for p1 in (0, 128, 216, 255) for p2 in (0, 255)]
        out.append(case("two keys G4 + E4 held, P2 200", params(p1=140, p2=200), [67, 64], 8, n, warmup=8,
                        group="notes"))
        out.append(case("keys C6 C2 G4 E4 arriving, re-sent (vel 0, NULL), P2 255", params(p1=140, p2=255),
                        [84, 36, 67, 64], 8, n, warmup=8, resend=6, group="notes"))
        out.append(case("clearing warm-up (first 4 blocks, dry only)", params(p1=216, p2=128), [255], 8, 6))
        return out + knob_cases(True, n, notes=[67])
    if name == "opz_test_sine":
        return [case("P1 %d, P2 %d" % (p1, p2), params(p1=p1, p2=p2), CHORD, 4, b)
                for p1 in (0, 128, 255) for p2 in (0, 255)]
    if name == "opz_test_passthrough":
        return [case("passthrough", params(), [255], 8, b)]
    if drum:
        return drum_grid(b) + knob_cases(False, b, notes=[53, 60, 67, 76], track=0, velocity=127)
    return (fx_grid(b) if is_fx else instrument_grid(b)) + knob_cases(is_fx, b)


def plug_kind(plug):
    """(is_fx, drum) from a plug.json: an effect has no instrument track in its mask, a drum plug no synth track."""
    mask = int(plug["plug"]["trackmask"])
    return not (mask & 0xff), bool(mask & 0x0f) and not (mask & 0xf0)


# --------------------------------------------------------------------------- generated sources

def adsr_tables():
    """Attack steps and decay coefficients per knob index, the curves of sim/src/callbacks.c (our fits)."""
    attack, decay = [], []
    for i in range(128):
        step = 0.0526142940 if i == 0 else 2.0 ** (-7.444 - 16.0 * i / 127.0)
        attack.append(int(step * 2147483648.0))
        tau = 33.6 if i == 0 else 250.0 * i + 11.5 * 2.0 ** (0.115 * i)
        c = math.exp(-1.0 / tau)
        decay.append(0x7fffffff if c >= 1.0 else int(c * 2147483648.0))
    return attack, decay


def c_array(ctype, name, values, per_line=8):
    rows = [", ".join(str(v) for v in values[i:i + per_line]) for i in range(0, len(values), per_line)]
    return "static const %s %s[%d] = {\n    %s\n};\n" % (ctype, name, len(values), ",\n    ".join(rows))


def bench_header(base, is_fx, fills, cases, dump=False, ro=None, tail=None, torn=False, pack_id=0):
    """ro: [(address, length)] of the engine's read-only sections; tail: (address, length) of the slot window after
    the engine's last byte (both checked by the driver after every case). torn: also render on half-written states
    after note-ons (driver.c, BENCH_TORN). pack_id: crc32 of the plug's `samples` path, passed to init as arg2/mem;
    the driver's getSample then serves a fake slice for it (driver.c, samples.md §5)."""
    attack, decay = adsr_tables()
    tail = tail or (0, 0)
    lines = ["/* Generated by tools/opz_cycles.py. */",
             "#define ENGINE_BASE 0x%08xu" % base,
             "#define ENGINE_IS_FX %d" % (1 if is_fx else 0),
             "#define BENCH_DUMP %d" % (1 if dump else 0),
             "#define BENCH_TORN %d" % (1 if torn else 0),
             "#define BENCH_STATE_LIMIT %d" % (DEVICE_FX_STATE_BYTES if is_fx else DEVICE_VOICE_STATE_BYTES),
             "#define BENCH_TAIL_START 0x%08xu" % tail[0],
             "#define BENCH_TAIL_WORDS %du" % (tail[1] // 4),
             "#define BENCH_PACK_ID 0x%08xu" % pack_id, ""]
    fl = fills or [(0, 0)]
    lines.append("static const BenchFill bench_fills[%d] = { %s };" % (
        len(fl), ", ".join("{ 0x%08xu, %du }" % f for f in fl)))
    rl = ro or [(base, 0)]
    lines.append("static const BenchFill bench_ro[%d] = { %s };" % (
        len(rl), ", ".join("{ 0x%08xu, %du }" % (a, n & ~3) for a, n in rl)))
    lines.append(c_array("int32_t", "bench_attack_step", attack))
    lines.append(c_array("int32_t", "bench_decay_coef", decay))
    lines.append(opz_tables.c_source("bench_"))
    for i, c in enumerate(cases):
        if c.get("stream"):
            lines.append(c_array("uint32_t", "bench_stream_%d" % i, ["%du" % v for r in c["stream"] for v in r]))
    lines.append("static const BenchCase bench_cases[%d] = {" % len(cases))
    for i, c in enumerate(cases):
        notes = c["notes"] + [255] * (MAX_VOICES - len(c["notes"]))
        pitch = c["pitch"] + [0] * (MAX_VOICES - len(c["pitch"]))
        lines.append("    /* %s */" % c["name"].replace("*/", ""))
        raw = c.get("raw")
        lines.append("    { { %s }, %d, %d, %d, { %s }, { %s }, %d, %d, %d, %d, %d, %d, %d, %d, %d, { %du, %du }, %d, %s }," % (
            ", ".join(str(v) for v in c["params"]), c["track"], c["velocity"], len(c["notes"]),
            ", ".join(str(n) for n in notes), ", ".join("%du" % p for p in pitch),
            c["blocks"], c["warmup"], c["release_at"], c.get("p1_to", 0xffff),
            c.get("knobs", 0), c.get("glide", 0), c.get("glide_flip", 0), c.get("resend", 0),
            1 if raw else 0, (raw or [0, 0])[0], (raw or [0, 0])[1],
            len(c["stream"]) if c.get("stream") else 0, "bench_stream_%d" % i if c.get("stream") else "0"))
    lines.append("};")
    return "\n".join(lines) + "\n"


def engine_image(elf_bytes, base, size):
    """The engine's loadable bytes from the slot base, and its zero-fill ranges (what the OS loader writes)."""
    blocks = opz_pack.blocks_from_elf(elf_bytes)
    data = [b for b in blocks if b["kind"] == "data"]
    fills = [(b["target"], b["count"]) for b in blocks if b["kind"] == "fill"]
    end = max(b["target"] + len(b["payload"]) for b in data)
    if min(b["target"] for b in data) != base or end > base + size:
        raise RuntimeError("engine image does not start at the slot base or leaves the window")
    image = bytearray(end - base)
    for b in data:
        off = b["target"] - base
        image[off:off + len(b["payload"])] = b["payload"]
    for addr, count in fills:
        if addr < base or addr + count > base + size:
            raise RuntimeError("engine .bss leaves the slot window")
    return bytes(image), fills


def engine_layout(elf_bytes, base, size):
    """Read-only sections [(address, length)] (export table, code, constants) and the slot window's tail after the
    engine's last byte (address, length), word-aligned."""
    blocks = opz_pack.blocks_from_elf(elf_bytes)
    ro = [(b["target"], len(b["payload"])) for b in blocks
          if b["kind"] == "data" and b["section"].split(".")[1:2] in (["opz_exports"], ["text"], ["rodata"])]
    end = max(b["target"] + (len(b["payload"]) if b["kind"] == "data" else b["count"]) for b in blocks)
    end = (end + 3) & ~3
    return ro, (end, base + size - end)


def image_source(image):
    rows = [", ".join("0x%02x" % v for v in image[i:i + 16]) for i in range(0, len(image), 16)]
    return ("/* Generated by tools/opz_cycles.py: the engine's slot image, byte for byte. */\n"
            "__attribute__((section(\".engine_image\"), used, aligned(4)))\n"
            "const unsigned char opz_engine_image[%d] = {\n    %s\n};\n" % (len(image), ",\n    ".join(rows)))


# --------------------------------------------------------------------------- run and parse

def parse_output(text):
    """Driver output -> {"module": {...}, "results": [{...}, ...], "done": bool, "dumps": [[int, ...], ...]}.
    dumps[i] holds case i's rendered samples (interleaved stereo, every block), when the driver printed them."""
    module, results, done, errors, dumps, cur = None, [], False, [], [], []
    findings, stacks, started, torn = [], None, None, None
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "S":
            w = parts[1]
            cur.extend(int(w[i:i + 8], 16) for i in range(0, len(w), 8))
            continue
        tag, kv = parts[0], {}
        for p in parts[1:]:
            if "=" in p:
                k, v = p.split("=", 1)
                kv[k] = int(v)
        if tag == "M":
            module = kv
        elif tag == "R":
            results.append(kv)
            dumps.append([v - (1 << 32) if v & 0x80000000 else v for v in cur])
            cur = []
        elif tag == "D":
            done = True
        elif tag == "E":
            errors.append(line[2:])
        elif tag == "C":
            started, torn = kv.get("case"), None
        elif tag == "T":
            torn = kv
        elif tag == "U":
            torn = None
        elif tag == "F":
            findings.append(kv)
        elif tag == "K":
            stacks = kv
    return {"module": module, "results": results, "done": done, "errors": errors, "dumps": dumps,
            "findings": findings, "stacks": stacks, "started": started, "torn": torn}


def describe_findings(findings, cases, is_fx):
    """Driver "F" lines -> [{"level", "check", "function", "case", "detail"}] (one per code and entry point; the
    first case that showed it)."""
    out, seen = [], set()
    for f in findings:
        key = (f["code"], f["entry"])
        if key in seen or f["code"] not in FINDINGS:
            continue
        seen.add(key)
        check, level, fmt = FINDINGS[f["code"]]
        a = f.get("a", 0)
        if a & 0x80000000:
            a -= 1 << 32
        torn = f["code"] in (5, 7) and a == MAX_VOICES          # the driver's slot for renders on torn states
        detail = fmt % {"a": a, "b": f.get("b", 0), "limit": DEVICE_FX_STATE_BYTES if is_fx else
                        DEVICE_VOICE_STATE_BYTES, "envsize": 24, "side": "before" if a == 0 else "after",
                        "reg": SAVED_REGS[a] if 0 <= a < len(SAVED_REGS) else "?"}
        if torn:
            detail = detail.replace("voice %d" % a, "a half-written state (onNoteOn interrupted)")
        case_i = f.get("case", 0xffff)
        out.append({"level": level, "check": check, "function": ENTRY_NAMES[f["entry"]],
                    "case": "initModule" if case_i == 0xffff else (cases[case_i]["name"] if case_i < len(cases)
                                                                   else str(case_i)),
                    "detail": detail})
    return out


class SimulatorCrash(RuntimeError):
    """The simulator stopped inside the benchmark: what the device would do is take an exception and reboot."""

    def __init__(self, info):
        RuntimeError.__init__(self, info["message"])
        self.info = info


def describe_crash(name, text, cases, syms=None):
    """Simulator output of a failed run -> {"message", "case", "pc", "where", "excause"}."""
    parsed = parse_output(text)
    case_i = parsed["started"]
    info = {"case": None if case_i is None else (cases[case_i]["name"] if case_i < len(cases) else str(case_i))}
    m = re.search(r"Unhandled exception 0x([0-9a-f]+) at 0x([0-9a-f]+) \(([^)]*)\)", text)
    sig = re.search(r"program stopped with signal (\d+) \(([^)]*)\)", text)
    if m:
        info["excause"] = int(m.group(1), 16)
        info["pc"] = int(m.group(2), 16)
        what = EXCAUSE.get(info["excause"], m.group(3))
    elif sig:
        what = "signal %s (%s): an access to unmapped memory (NULL page, wild pointer) or a simulator fault" % (
            sig.group(1), sig.group(2))
    else:
        what = "no result (exit without a report)"
    where = None
    if syms and info.get("pc") is not None:
        starts = [a for a, _ in syms]
        i = bisect.bisect_right(starts, info["pc"]) - 1
        if i >= 0:
            where = "%s+0x%x" % (syms[i][1], info["pc"] - syms[i][0])
    info["where"] = where
    t = parsed["torn"]
    info["torn"] = bool(t)
    info["message"] = "%s: %s%s%s%s" % (
        name, what, (" at %s" % where) if where else
        ((" at 0x%08x" % info["pc"]) if info.get("pc") is not None else ""),
        (" in case \"%s\"" % info["case"]) if info["case"] else " before the first case",
        (", rendering a state that onNoteOn had only partly written (%s; %d words changed): the audio interrupt "
         "can run render in the middle of onNoteOn on the device" % (
             "first note" if t.get("phase") == 0 else "re-trigger", t.get("words", 0))) if info["torn"] else "")
    return info


def summarise(c, r, is_fx):
    def mean(prefix):
        return r[prefix + "_sum"] / r[prefix + "_n"] if r.get(prefix + "_n") else 0.0
    steady = mean("steady") if r.get("steady_n") else mean("render")
    out = {
        "case": c["name"], "group": c["group"], "voices": r["voices"], "blocks": r["blocks"],
        "per_call_mean": round(steady, 1), "per_call_max": r["render_max"], "per_call_min": r["render_min"],
        "per_sample_mean": round(steady / BLOCK, 1), "per_sample_max": round(r["render_max"] / BLOCK, 1),
        "block_max": r["block_max"], "block_mean": round(mean("block"), 1),
        "note_on_mean": round(mean("on"), 1), "note_on_max": r["on_max"],
        "note_off_max": r["off_max"],
        "callback_per_call": round(r["cb"] / r["render_n"], 1) if r.get("render_n") else 0.0,
    }
    worst = out["per_sample_max"]
    if is_fx:
        out["verdict"] = "ok" if worst <= FX_TARGET_PER_SAMPLE else "over FX target"
    else:
        # the OS's two lines: 265,000 three blocks running (the steady mean) and 290,000 in any one block
        out["verdict"] = ("ok" if worst <= TARGET_PER_SAMPLE else
                          "tight" if out["per_sample_mean"] <= BUDGET_PER_SAMPLE and worst <= BUDGET_PER_SAMPLE_HARD
                          else "over budget")
    return out


# --------------------------------------------------------------------------- layout: the shared pool

def parse_layout(spec):
    """"bass=table chord=modal*2 lead=super:mono kick=kit" -> {track: (engine, voices)}. Separators: space or
    comma. Voices default to the track's OS voice count; *N sets the sounding voices (an engine-side cap or fewer
    notes), :mono is 1."""
    out = {}
    for item in re.split(r"[\s,]+", spec.strip()):
        if not item:
            continue
        m = re.fullmatch(r"(\w+)=([\w.-]+?)(?:\*(\d+)|:(mono))?", item)
        if not m or m.group(1) not in TRACK_NAMES:
            raise ValueError("layout item %r: want TRACK=ENGINE[*N|:mono], TRACK one of %s" % (
                item, ", ".join(TRACK_NAMES)))
        t = TRACK_NAMES.index(m.group(1))
        v = 1 if m.group(4) else int(m.group(3)) if m.group(3) else TRACK_VOICES[t]
        if not 1 <= v <= 6:
            raise ValueError("layout item %r: 1 to 6 voices" % item)
        out[t] = (m.group(2), v)
    if not out:
        raise ValueError("empty layout")
    return out


def engine_cost(entry):
    """Per-voice cost of one engine from its measure() report: the worst steady mean and the worst single block over
    all its cases (conservative: the layout's voices are assumed to sit in their dearest case together)."""
    cases = entry["cases"]
    return max(c["per_call_mean"] for c in cases), max(c["per_call_max"] for c in cases)


def layout_budget(layout, costs, flags, line=KILL_SOFT, hard=None):
    """The timed instrument stage of a layout. costs: engine -> (mean, max) cycles per voice per block; flags: engine
    -> plug flags. Returns a dict with the OS share, the steady and worst sums, the lines and a verdict."""
    hard = KILL_HARD - KILL_SOFT + line if hard is None else hard
    os_share, rows, voices = OS_WINDOW_FIXED, [], 0
    for t in range(INSTRUMENT_TRACKS):
        if t not in layout:
            os_share += OS_SILENT_TRACK
            continue
        engine, v = layout[t]
        own_filter = bool(flags.get(engine, 0) & 0x04)
        os_share += OS_PER_TRACK - (OS_TRACK_FILTER if own_filter else 0)
        if t == TRACK_NAMES.index("bass") and not own_filter:
            os_share += OS_BASS_EXTRA
        mean, worst = costs[engine]
        voices += v
        rows.append({"track": TRACK_NAMES[t], "engine": engine, "voices": v, "own_filter": own_filter,
                     "mean": v * mean, "max": v * worst, "per_sample_mean": round(mean / BLOCK, 1)})
    os_share += OS_PER_VOICE * voices
    steady = os_share + sum(r["mean"] for r in rows)
    worst = os_share + sum(r["max"] for r in rows)
    if steady <= line and worst <= hard:
        verdict = "fits"
    elif steady <= line:
        verdict = "tight: a block where every voice is at its worst could cut one release tail"
    else:
        verdict = "over: the OS will cut release tails while this plays"
    return {"rows": rows, "voices": voices, "os": os_share, "steady": round(steady), "worst": round(worst),
            "line": line, "hard": hard, "headroom": round(line - steady), "verdict": verdict}


def print_layout(b):
    print("%-7s %-12s %6s %7s %14s %14s" % ("track", "engine", "voices", "filter", "cycles/block", "worst block"))
    for r in b["rows"]:
        print("%-7s %-12s %6d %7s %14.0f %14.0f" % (r["track"], r["engine"], r["voices"],
                                                   "own" if r["own_filter"] else "OS", r["mean"], r["max"]))
    print("%-7s %-12s %6d %7s %14d %14d" % ("OS", "", b["voices"], "", b["os"], b["os"]))
    print("%-7s %-12s %6s %7s %14d %14d   of %d / %d (%.0f %% / %.0f %%)" % (
        "total", "", "", "", b["steady"], b["worst"], b["line"], b["hard"], 100.0 * b["steady"] / b["line"],
        100.0 * b["worst"] / b["hard"]))
    print("verdict: %s; %d cycles per block left (%.0f cycles/sample for one more voice)" % (
        b["verdict"], b["headroom"], max(0, b["headroom"] - OS_PER_VOICE) / BLOCK))


def blackfin_plus_multiplies(image):
    """Offsets of "Rd = Ra * Rb (T)" Blackfin+ words (0xC231, 0x2000 | d << 6 | a << 3 | b; opz_engine.h with
    OPZ_BLACKFIN_PLUS). The classic simulator decodes them as a 16-bit multiply."""
    return [i for i in range(0, len(image) - 3, 2)
            if image[i:i + 2] == b"\x31\xc2" and (image[i + 3] & 0xfe) == 0x20]


def tool(toolchain, name):
    return os.path.join(toolchain, "bin", "bfin-elf-" + name)


def available(toolchain):
    return all(os.access(tool(toolchain, t), os.X_OK) for t in ("gcc", "run"))


def measure(engine_dir, toolchain, out_root, quick=False, cases=None, quiet=False, timeout=1800, run=None,
            profile=False, dump=None, torn=False, blackfin_plus=False):
    """Build and run one engine. Returns a dict with the engine facts and one summary per case.
    profile=True also traces every instruction and adds "profile": instructions per function.
    dump=DIR also saves every case's rendered output to DIR/<engine>/ (see save_dump).
    torn=True adds the renders on half-written states (BENCH_TORN; the cycle numbers stay those of the real calls).
    blackfin_plus=True builds with OPZ_BLACKFIN_PLUS into OUT/bfin-bfp and runs our simulator that knows the
    Blackfin+ multiply (BFP_RUN unless run is given); results go to OUT/cycles-bfp."""
    edir = os.path.abspath(engine_dir)
    name = os.path.basename(edir.rstrip("/"))
    with open(os.path.join(edir, "plug.json")) as f:
        plug = json.load(f)
    is_fx, drum = plug_kind(plug)

    with contextlib.redirect_stdout(io.StringIO() if quiet else sys.stderr):
        elf, _, slot, base, size = opz_build.build(
            edir, toolchain, os.path.join(out_root, "bfin-bfp" if blackfin_plus else "bfin"),
            defines=("OPZ_BLACKFIN_PLUS",) if blackfin_plus else ())
    with open(elf, "rb") as f:
        elf_bytes = f.read()
    image, fills = engine_image(elf_bytes, base, size)
    bfp = blackfin_plus_multiplies(image)
    if bfp and not blackfin_plus:
        raise RuntimeError("%s: %d Blackfin+ 32 x 32 multiplies (OPZ_BLACKFIN_PLUS, first at 0x%08x): the classic "
                           "bfin-elf-run cannot run them; use --blackfin-plus (Q-47)" % (
                               os.path.basename(edir.rstrip("/")), len(bfp), base + bfp[0]))
    if blackfin_plus:
        run = run or BFP_RUN
        if not os.access(run, os.X_OK):
            raise RuntimeError("no Blackfin+ simulator at %s (research/toolchain-setup.md)" % run)
    ro, tail = engine_layout(elf_bytes, base, size)

    if cases is None:
        cases = cases_for(name, is_fx, quick, drum)
    work = os.path.join(out_root, "cycles-bfp" if blackfin_plus else "cycles", name)
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, "bench.h"), "w") as f:
        samples = str(plug["plug"].get("samples", ""))
        f.write(bench_header(base, is_fx, fills, cases, dump=bool(dump), ro=ro, tail=tail, torn=torn,
                             pack_id=zlib.crc32(samples.encode()) if samples and not is_fx else 0))
    with open(os.path.join(work, "image.c"), "w") as f:
        f.write(image_source(image))
    with open(os.path.join(DRIVER_DIR, "driver.ld.in")) as f:
        ld = (f.read().replace("@BASE@", "0x%08x" % base).replace("@SIZE@", "0x%x" % size)
              .replace("@HOSTSIZE@", "0x%x" % HOST_SIZE).replace("@HOST@", "0x%08x" % HOST_BASE))
    ld_path = os.path.join(work, "driver.ld")
    with open(ld_path, "w") as f:
        f.write(ld)

    drv = os.path.join(work, "driver.elf")
    opz_build.run([tool(toolchain, "gcc")] + opz_build.CFLAGS
                  + ["-I", os.path.join(ROOT, "sdk", "include"), "-I", work,
                     os.path.join(DRIVER_DIR, "crt0.S"), os.path.join(DRIVER_DIR, "checks.S"),
                     os.path.join(DRIVER_DIR, "driver.c"),
                     os.path.join(work, "image.c")]
                  + ["-nostdlib", "-nostartfiles", "-Wl,--gc-sections", "-T", ld_path, "-o", drv, "-lgcc"])

    t0 = time.time()
    try:
        r = subprocess.run([run or tool(toolchain, "run")] + (["--trace-insn=on", "--trace-file", os.path.join(work, "trace.txt")] if profile else []) +
                           ["--memory-region", "0x%x,0x%x" % NULL_PAGE_REGION,
                            "--memory-region", "0x%x,0x%x" % (HOST_BASE, HOST_SIZE),
                            "--memory-region", "0x%x,0x%x" % SLOT_REGION]
                           + [a for r in DEVICE_REGIONS for a in ("--memory-region", "0x%x,0x%x" % r)] + [drv],
                           capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
        raise RuntimeError("%s: the simulator did not finish in %d s%s" % (
            name, timeout, "" if out.strip() else
            " and printed nothing: an unpatched bfin-elf-run on arm64 mis-decodes signed immediates, so the "
            "driver's system calls never reach the host (research/toolchain-setup.md, SIGNEXTEND patch)"))
    trace = None
    if profile:
        with open(os.path.join(work, "trace.txt")) as f:
            trace = f.read()
    parsed = parse_output(r.stdout)
    if r.returncode != 0 or not parsed["done"] or len(parsed["results"]) != len(cases):
        if parsed["errors"]:
            raise RuntimeError("%s: %s" % (name, "; ".join(parsed["errors"])))
        raise SimulatorCrash(describe_crash(name, r.stdout + r.stderr, cases, symbols(elf, toolchain)))
    if dump:
        save_dump(os.path.join(dump, name), cases, parsed["dumps"])
    return {
        "engine": name, "kind": "effect" if is_fx else "instrument", "slot": slot,
        "image_bytes": len(image), "init_module_cycles": parsed["module"]["cycles"],
        "mem_request": parsed["module"]["mem"], "sim_seconds": round(time.time() - t0, 1),
        "cases": [summarise(c, res, is_fx) for c, res in zip(cases, parsed["results"])],
        "checks": {"stack": {k2: parsed["stacks"][k] for k, k2 in STACK_KEYS if parsed["stacks"] and k in parsed["stacks"]},
                   "scratch": (parsed["stacks"] or {}).get("scratch", 0),
                   "findings": describe_findings(parsed["findings"], cases, is_fx)},
        **({"profile": profile_trace(trace, elf, base, base + size, debug_twin(elf, edir, toolchain), toolchain)}
           if profile else {}),
    }


def symbols(elf, toolchain):
    """Function start addresses of an ELF, sorted: [(address, name)]."""
    out = opz_build.run([tool(toolchain, "nm"), "-n", elf])
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            syms.append((int(parts[0], 16), parts[2].lstrip("_")))
    return syms


def debug_twin(elf, edir, toolchain):
    """The same engine linked again with -g1 (line tables only; the code is checked byte for byte), so that
    addr2line can name source lines and inlined helpers. Returns its path, or None if the code differs."""
    out = os.path.dirname(elf)
    twin = elf[:-4] + ".g.elf"
    with open(os.path.join(out, "engine.ld")) as f:
        ld = f.read().replace("*(.debug*)", "")                  # keep the (non-loaded) line tables
    ld_path = os.path.join(out, "engine.g.ld")
    with open(ld_path, "w") as f:
        f.write(ld)
    sources = sorted(os.path.join(edir, s) for s in os.listdir(edir) if s.endswith(".c"))
    opz_build.run([tool(toolchain, "gcc")] + opz_build.CFLAGS + ["-g1", "-I", os.path.join(ROOT, "sdk", "include")]
                  + sources + opz_build.LDFLAGS + ["-T", ld_path, "-o", twin, "-lgcc"])

    def code(path):
        with open(path, "rb") as f:
            return [(b["target"], b.get("payload"), b.get("count")) for b in opz_pack.blocks_from_elf(f.read())]
    return twin if code(twin) == code(elf) else None


def profile_trace(trace, elf, lo, hi, twin, toolchain):
    """Count traced instructions inside the engine's window [lo, hi): per function (with entries = calls) and, when
    the -g1 twin is available, per source line (an inlined helper counts at its own line, e.g. opz_dsp.h). Shares
    are of engine instructions only (the driver's own work is left out)."""
    syms = symbols(elf, toolchain)
    starts = [a for a, _ in syms]
    per_pc = {}
    for line in trace.splitlines():
        if line.startswith("insn:"):
            parts = line.split()
            if len(parts) > 1 and parts[1].startswith("0x"):
                pc = int(parts[1], 16)
                if lo <= pc < hi:
                    per_pc[pc] = per_pc.get(pc, 0) + 1
    total = sum(per_pc.values()) or 1
    funcs, calls = {}, {}
    for pc, n in per_pc.items():
        i = bisect.bisect_right(starts, pc) - 1
        name = syms[i][1] if i >= 0 else "?"
        funcs[name] = funcs.get(name, 0) + n
        if i >= 0 and starts[i] == pc:
            calls[name] = calls.get(name, 0) + n
    result = {"engine_instructions": total,
              "functions": sorted(({"function": f, "instructions": n, "share": round(100.0 * n / total, 1),
                                    "calls": calls.get(f, 0)} for f, n in funcs.items()),
                                  key=lambda r: -r["instructions"])}
    if twin:
        pcs = sorted(per_pc)
        out = opz_build.run([tool(toolchain, "addr2line"), "-e", twin, "-f", "-a"] + ["0x%x" % p for p in pcs])
        where, lines = {}, out.splitlines()
        for k in range(0, len(lines) - 2, 3):
            if lines[k].startswith("0x"):
                fn, loc = lines[k + 1], lines[k + 2]
                path, _, num = loc.rpartition(":")
                base = os.path.basename(path) or "?"
                where[int(lines[k], 16)] = ("libgcc %s" % fn.lstrip("_") if base.endswith(".S") or base == "?"
                                            else "%s:%s %s" % (base, num.split(" ")[0], fn.lstrip("_")))
        by_line = {}
        for pc, n in per_pc.items():
            key = where.get(pc, "?")
            by_line[key] = by_line.get(key, 0) + n
        result["lines"] = sorted(({"line": l, "instructions": n, "share": round(100.0 * n / total, 1)}
                                  for l, n in by_line.items()), key=lambda r: -r["instructions"])
    return result


# --------------------------------------------------------------------------- null test

def save_dump(directory, cases, dumps):
    """One little-endian int32 file per case (interleaved stereo, as rendered) plus cases.json with the names."""
    os.makedirs(directory, exist_ok=True)
    names = []
    for i, (c, d) in enumerate(zip(cases, dumps)):
        with open(os.path.join(directory, "%03d.i32" % i), "wb") as f:
            f.write(struct.pack("<%di" % len(d), *d))
        names.append(c["name"])
    with open(os.path.join(directory, "cases.json"), "w") as f:
        json.dump(names, f, indent=1)


def load_dump(directory):
    """{case name: (samples, ...)} from save_dump."""
    with open(os.path.join(directory, "cases.json")) as f:
        names = json.load(f)
    out = {}
    for i, n in enumerate(names):
        with open(os.path.join(directory, "%03d.i32" % i), "rb") as f:
            b = f.read()
        out[n] = struct.unpack("<%di" % (len(b) // 4), b)
    return out


def dbfs(x):
    return 20.0 * math.log10(x / 2147483648.0) if x > 0 else float("-inf")


def null_test(a, b):
    """Peak and RMS of a - b (the shorter one padded with silence) and the peak of a, all in dBFS, plus the
    number of samples that differ."""
    n = max(len(a), len(b))
    peak = sq = diff = sig = 0
    for i in range(n):
        x = a[i] if i < len(a) else 0
        y = b[i] if i < len(b) else 0
        d = abs(x - y)
        if d:
            diff += 1
            sq += d * d
            peak = max(peak, d)
        sig = max(sig, abs(x))
    return {"samples": n, "differ": diff, "peak_dbfs": dbfs(peak), "rms_dbfs": dbfs(math.sqrt(sq / n) if n else 0),
            "signal_peak_dbfs": dbfs(sig), "length_a": len(a), "length_b": len(b)}


def compare_dumps(dir_a, dir_b):
    """Null-test every engine and case present in both dump directories. Returns [(engine, case, result)]."""
    rows = []
    for engine in sorted(os.listdir(dir_a)):
        pa, pb = os.path.join(dir_a, engine), os.path.join(dir_b, engine)
        if not (os.path.exists(os.path.join(pa, "cases.json")) and os.path.exists(os.path.join(pb, "cases.json"))):
            continue
        da, db = load_dump(pa), load_dump(pb)
        for name in da:
            if name in db:
                rows.append((engine, name, null_test(da[name], db[name])))
    return rows


# --------------------------------------------------------------------------- report

def markdown(report):
    lines = []
    for e in report:
        fx = e["kind"] == "effect"
        lines.append("### %s (%s, slot %d, %d-byte image)\n" % (e["engine"], e["kind"], e["slot"], e["image_bytes"]))
        lines.append("| Case | Voices | Blocks | Cycles/block/voice mean | max | Cycles/sample mean | max "
                     "| Note-on mean | max | Verdict |")
        lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---|")
        for c in e["cases"]:
            lines.append("| %s | %d | %d | %s | %s | %.1f | %.1f | %s | %s | %s |" % (
                c["case"], c["voices"], c["blocks"], "{:,.0f}".format(c["per_call_mean"]),
                "{:,}".format(c["per_call_max"]), c["per_sample_mean"], c["per_sample_max"],
                "{:,.0f}".format(c["note_on_mean"]) if not fx or c["note_on_max"] else "–",
                "{:,}".format(c["note_on_max"]) if not fx or c["note_on_max"] else "–", c["verdict"]))
        lines.append("")
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("engine_dirs", nargs="*")
    ap.add_argument("--all", action="store_true", help="every engine under engines/")
    ap.add_argument("--quick", action="store_true", help="fewer cases and blocks (for tests)")
    ap.add_argument("--toolchain", default=os.path.expanduser("~/opt/bfin-elf"))
    ap.add_argument("--run", help="simulator binary (default: TOOLCHAIN/bin/bfin-elf-run)")
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "build"))
    ap.add_argument("--json", help="write the full report as JSON")
    ap.add_argument("--markdown", action="store_true", help="print Markdown tables instead of text")
    ap.add_argument("--profile", metavar="CASE",
                    help="trace one case (first whose name contains CASE; one voice, --profile-blocks blocks) and "
                         "print instructions per function")
    ap.add_argument("--profile-blocks", type=int, default=4)
    ap.add_argument("--blackfin-plus", action="store_true",
                    help="EXPERIMENTAL: build with OPZ_BLACKFIN_PLUS and run on our Blackfin+ simulator (Q-47)")
    ap.add_argument("--layout", metavar="SPEC",
                    help='a whole layout against the shared pool, e.g. "bass=table chord=modal*2 kick=kit"')
    ap.add_argument("--report", metavar="JSON", help="with --layout: take costs from an earlier --json report "
                    "instead of measuring")
    ap.add_argument("--line", type=int, default=KILL_SOFT,
                    help="with --layout: the overload line (default %d; the single-block line is 25,000 above)"
                    % KILL_SOFT)
    ap.add_argument("--dump", metavar="DIR", help="also save every case's rendered output (for --compare)")
    ap.add_argument("--compare", nargs=2, metavar=("DIR_A", "DIR_B"),
                    help="null-test two --dump directories: peak and RMS of the difference, in dBFS")
    args = ap.parse_args(argv)

    if args.compare:
        print("%-14s %-42s %9s %10s %10s %10s" % ("engine", "case", "differ", "peak dBFS", "RMS dBFS", "signal"))
        for engine, name, r in compare_dumps(*args.compare):
            print("%-14s %-42s %9d %10.1f %10.1f %10.1f%s" % (
                engine, name[:42], r["differ"], r["peak_dbfs"], r["rms_dbfs"], r["signal_peak_dbfs"],
                "" if r["length_a"] == r["length_b"] else "  (length %d vs %d)" % (r["length_a"], r["length_b"])))
        return

    if args.layout:
        try:
            layout = parse_layout(args.layout)
        except ValueError as e:
            ap.error(str(e))
        names = sorted({e for e, _ in layout.values()})
        flags = {}
        for n in names:
            pj = os.path.join(ROOT, "engines", n, "plug.json")
            if not os.path.exists(pj):
                ap.error("no engine %r under engines/" % n)
            with open(pj) as f:
                flags[n] = int(json.load(f)["plug"].get("flags", 0))
        if args.report:
            with open(args.report) as f:
                known = {e["engine"]: e for e in json.load(f)["engines"]}
            missing = [n for n in names if n not in known]
            if missing:
                ap.error("%s not in %s" % (", ".join(missing), args.report))
        else:
            if not available(args.toolchain):
                sys.exit("opz_cycles: bfin-elf-gcc / bfin-elf-run not found in %s/bin "
                         "(research/toolchain-setup.md)" % args.toolchain)
            known = {n: measure(os.path.join(ROOT, "engines", n), args.toolchain, args.out, quick=args.quick,
                                quiet=True, run=args.run, blackfin_plus=args.blackfin_plus) for n in names}
        b = layout_budget(layout, {n: engine_cost(known[n]) for n in names}, flags, line=args.line)
        print_layout(b)
        return

    dirs = list(args.engine_dirs)
    if args.all:
        edir = os.path.join(ROOT, "engines")
        dirs += sorted(os.path.join(edir, d) for d in os.listdir(edir)
                       if os.path.exists(os.path.join(edir, d, "plug.json")))
    if not dirs:
        ap.error("name an engine directory or pass --all")
    if not available(args.toolchain):
        sys.exit("opz_cycles: bfin-elf-gcc / bfin-elf-run not found in %s/bin "
                 "(research/toolchain-setup.md)" % args.toolchain)

    if args.profile:
        for d in dirs:
            name = os.path.basename(os.path.abspath(d).rstrip("/"))
            with open(os.path.join(d, "plug.json")) as f:
                is_fx, drum = plug_kind(json.load(f))
            pick = [c for c in cases_for(name, is_fx, drum=drum) if args.profile in c["name"]]
            if not pick:
                sys.exit("opz_cycles: no %s case matches %r" % (name, args.profile))
            c = dict(pick[0], notes=pick[0]["notes"][:1], pitch=pick[0]["pitch"][:1], blocks=args.profile_blocks,
                     warmup=0, release_at=0)
            e = measure(d, args.toolchain, args.out, cases=[c], quiet=True, run=args.run, profile=True,
                        blackfin_plus=args.blackfin_plus)
            prof = e["profile"]
            print("%s, %s: %d blocks, one voice, %d engine instructions" % (
                name, c["name"], c["blocks"], prof["engine_instructions"]))
            print("  by function:")
            for row in prof["functions"][:12]:
                print("    %5.1f %%  %8d  %-24s %6d calls" % (row["share"], row["instructions"], row["function"],
                                                            row["calls"]))
            if "lines" in prof:
                print("  by source line (an inlined helper counts at its own line):")
                for row in prof["lines"][:25]:
                    print("    %5.1f %%  %8d  %s" % (row["share"], row["instructions"], row["line"]))
            else:
                print("  (no per-line profile: the -g1 build did not produce identical code)")
        return

    report = []
    for d in dirs:
        e = measure(d, args.toolchain, args.out, quick=args.quick, quiet=True, run=args.run, dump=args.dump,
                    blackfin_plus=args.blackfin_plus)
        report.append(e)
        if not args.markdown:
            print("%s (%s, slot %d): initModule %d cycles, memRequest %d, %.1f s in the simulator" % (
                e["engine"], e["kind"], e["slot"], e["init_module_cycles"], e["mem_request"], e["sim_seconds"]))
            for c in e["cases"]:
                print("  %-40s %2d v  %6.0f cyc/block/voice (max %6d)  %6.1f cyc/sample (max %6.1f)  "
                      "note-on %5.0f (max %5d)  %s" % (
                          c["case"][:40], c["voices"], c["per_call_mean"], c["per_call_max"],
                          c["per_sample_mean"], c["per_sample_max"], c["note_on_mean"], c["note_on_max"],
                          c["verdict"]))
            ck = e["checks"]
            print("  checks: stack bytes %s; scratch %d bytes; %s" % (
                ", ".join("%s %d" % kv for kv in ck["stack"].items()), ck["scratch"],
                "%d finding(s)" % len(ck["findings"]) if ck["findings"] else "no findings"))
            for f in ck["findings"]:
                print("    %s: %s in %s (case \"%s\"): %s" % (f["level"], f["check"], f["function"], f["case"],
                                                         f["detail"]))
    if args.markdown:
        print(markdown(report))
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"budget": {"cpu_hz": CPU_HZ, "block_cycles": BLOCK_CYCLES, "kill_soft": KILL_SOFT,
                                  "kill_hard": KILL_HARD, "os_window": OS_WINDOW_CYCLES,
                                  "per_sample_per_voice_19": round(BUDGET_PER_SAMPLE, 1),
                                  "per_sample_per_voice_19_hard": round(BUDGET_PER_SAMPLE_HARD, 1),
                                  "target_per_sample": TARGET_PER_SAMPLE},
                       "engines": report}, f, indent=1)
            f.write("\n")


if __name__ == "__main__":
    main()
