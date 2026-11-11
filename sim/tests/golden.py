#!/usr/bin/env python3
"""Host checks and golden-render tests for opzsim.

Run through make, from the repository root:

    make -C sim test      # host checks + golden hashes
    make -C sim golden    # re-render and overwrite golden files (review the diff)

Host checks drive small fixture engines (sim/tests/fixtures) to prove that opzsim enforces the ABI rules and
follows sdk/docs/engine-abi.md (parameter conversion, smoothing, pitch). Golden tests render every
engines/<name>/tests/*.scn and compare the WAV's SHA-256 with tests/golden/<scenario>.wav.sha256. On a
mismatch the octave-band spectrum is compared with tests/golden/<scenario>.spectrum.json and the distance is
reported; --spectral accepts renders within --tolerance dB (for refactors that change rounding only).

opzsim's static-size check measures the host (arm64) build, an estimate only. When the Blackfin toolchain is
installed (~/opt/bfin-elf, research/toolchain-setup.md) each engine is also built for the device with
tools/opz_build.py, whose linker refuses an image that leaves its slot window; an engine whose device build fits
does not fail on the host estimate's size warning. Without the toolchain the warning is reported, not failed: the
device build checks the fit when the engine is built for the OP-Z.
"""

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.dirname(HERE)
REPO = os.path.dirname(SIM)
ENGINES = os.path.join(REPO, "engines")
FIXTURES = os.path.join(HERE, "fixtures")

EXIT_ENGINE, EXIT_EXPECT, EXIT_BUDGET = 2, 3, 4


def opzsim(build, engine, scenario, *extra, out=None, stats=None):
    cmd = [os.path.join(build, "opzsim"), "-e", engine, *extra]
    if out:
        cmd += ["-o", out]
    if stats:
        cmd += ["--stats", stats]
    cmd.append(scenario)
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, p.stderr


def read_wav32(path):
    with wave.open(path) as w:
        assert w.getsampwidth() == 4 and w.getnchannels() == 2
        n = w.getnframes()
        data = w.readframes(n)
    return struct.unpack("<%di" % (2 * n), data)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def param_word(b):
    return b * 0x808080 + (b >> 1)


class Results:
    def __init__(self, verbose):
        self.passed = self.failed = 0
        self.verbose = verbose

    def check(self, name, ok, detail="", log=""):
        print("%-4s %s%s" % ("ok" if ok else "FAIL", name, (" — " + detail) if detail else ""))
        if (not ok or self.verbose) and log:
            for line in log.rstrip().splitlines():
                print("       | " + line)
        if ok:
            self.passed += 1
        else:
            self.failed += 1


# --------------------------------------------------------------------------- host checks

def host_checks(build, res, tmp):
    fx = os.path.join(build, "fixtures")
    one_note = os.path.join(FIXTURES, "one_note.scn")

    rc, log = opzsim(build, os.path.join(fx, "missing_export.dylib"), one_note)
    res.check("host: engine without onNoteOff is refused", rc == EXIT_ENGINE, "exit %d" % rc, log)

    rc, log = opzsim(build, os.path.join(fx, "state_overflow.dylib"), one_note)
    res.check("host: voice state over 280 B is an error", rc == EXIT_BUDGET and "past the 280-byte" in log,
              "exit %d" % rc, log)

    rc, log = opzsim(build, os.path.join(fx, "overwrite.dylib"), one_note)
    res.check("host: render that overwrites out is an error", rc == EXIT_BUDGET and "does not add" in log,
              "exit %d" % rc, log)

    rc, log = opzsim(build, os.path.join(fx, "fx_big_init.dylib"), os.path.join(FIXTURES, "fx_master.scn"))
    res.check("host: master pool is 16 KB and a heavy FX init is flagged",
              rc == 0 and "master pool of 16384 B; capped" in log and "init writes 16384 B" in log,
              "exit %d" % rc, log)

    # Parameter words, 1/64 one-pole per block, Q32 pitch: compare against a model of the ABI document.
    wav = os.path.join(tmp, "param_probe.wav")
    scn = os.path.join(FIXTURES, "param_probe.scn")
    rc, log = opzsim(build, os.path.join(fx, "param_probe.dylib"), scn, out=wav)
    if rc != 0:
        res.check("host: parameter smoothing and pitch match engine-abi.md", False, "exit %d" % rc, log)
        return
    x = read_wav32(wav)
    step = 44100 * 15 / 120
    event = int(2 * step + 0.5)
    note_off_block = int(8 * step + 0.5) // 64
    cur, tgt, bad = param_word(0), param_word(0), []
    for b in range(note_off_block):
        if b * 64 >= event:
            tgt = param_word(255)
        diff = (tgt - cur + 2**31) % 2**32 - 2**31  # int32
        cur = (cur + (diff >> 6)) % 2**32
        if x[(b * 64) * 2] != cur >> 1:
            bad.append((b, x[(b * 64) * 2], cur >> 1))
        if x[(b * 64 + 1) * 2] != 0x184CBB >> 1:
            bad.append((b, "pitch", x[(b * 64 + 1) * 2]))
    res.check("host: parameter smoothing and pitch match engine-abi.md", not bad and param_word(255) == 0x7fffffff,
              "%d blocks checked" % note_off_block if not bad else "first mismatch %r" % (bad[0],), log)

    ahd_check(build, res, tmp)
    tables_check(build, res, tmp)
    tempo_check(build, res, tmp)
    sample_check(build, res, tmp)
    voicecap_check(build, res, tmp)
    mix_check(build, res, tmp)


def voicecap_check(build, res, tmp):
    """opz_voices.h (cap 2): never more than two voices sound, an evicted voice fades on the host's fade (no jump)
    and is freed within ~12 blocks, and everything ends silent."""
    name = "sdk: opz_voices.h caps sounding voices and evicts with the host fade"
    wav = os.path.join(tmp, "voicecap_probe.wav")
    rc, log = opzsim(build, os.path.join(build, "fixtures", "voicecap_probe.dylib"),
                     os.path.join(FIXTURES, "voicecap_probe.scn"), out=wav)
    if rc != 0:
        res.check(name, False, "exit %d" % rc, log)
        return
    x = read_wav32(wav)
    left, right = x[0::2], x[1::2]
    sounding = [r >> 24 for r in right]
    fading = [(r >> 16) & 0xff for r in right]
    bad = []
    if max(sounding) != 2:
        bad.append("most voices sounding at once is %d, not 2" % max(sounding))
    jump = max(abs(left[i + 1] - left[i]) for i in range(len(left) - 1))
    if jump > 1 << 25:
        bad.append("left jumps by %d in one frame (a click)" % jump)
    runs, run = [], 0                      # lengths of the fading stretches, in frames
    for f in fading:
        if f:
            run += 1
        elif run:
            runs.append(run)
            run = 0
    if len(runs) != 3:
        bad.append("expected 3 evictions (G4, B4, D5 each push one out), saw %d fading stretches" % len(runs))
    elif max(runs) > 12 * 64:
        bad.append("an evicted voice faded for %d frames (> 12 blocks)" % max(runs))
    if any(left[-64:]) or any(right[-64:]):
        bad.append("not silent at the end")
    res.check(name, not bad, "%d evictions, longest fade %d frames" % (len(runs), max(runs or [0]))
              if not bad else bad[0], log)


# Level and pan (opz_mix.h), modelled from sdk/docs/engine-abi.md §4 independently of the C code: TE's balance law,
# gain_L = level * min(2(1 - pan), 1), gain_R = level * min(2 pan, 1), Q15, ramped linearly across each block.

def mix_targets(level_word, pan_word):
    level, pan = level_word & 0x7fffffff, pan_word & 0x7fffffff
    a = (0x7fffffff - pan) << 1 if pan >= 0x40000000 else 0x7fffffff
    b = pan << 1 if pan < 0x40000000 else 0x7fffffff
    return min((level * a) >> 31, 0x7fffffff) >> 16, min((level * b) >> 31, 0x7fffffff) >> 16


def mix_check(build, res, tmp):
    """opz_mix.h through the host: a constant voice shows the gains, their per-sample ramps and the host's
    per-block smoothing of params 10/11 exactly; the same voice through an FX shows that what an FX track is fed
    (the OS takes the sends from the track buffer) follows level and pan."""
    name = "sdk: opz_mix.h applies level and pan with TE's law, ramped per block"
    wav = os.path.join(tmp, "mix_probe.wav")
    dylib, scn = os.path.join(build, "fixtures", "mix_probe.dylib"), os.path.join(FIXTURES, "mix_probe.scn")
    rc, log = opzsim(build, dylib, scn, out=wav)
    if rc != 0:
        res.check(name, False, "exit %d" % rc, log)
        return
    x = read_wav32(wav)
    step = 44100 * 15 / 120
    events = [(int(2 * step + 0.5), 11, 128), (int(4 * step + 0.5), 10, 0), (int(14 * step + 0.5), 10, 255),
              (int(24 * step + 0.5), 11, 0)]
    cur, tgt = {10: param_word(127), 11: param_word(255)}, {10: param_word(127), 11: param_word(255)}
    sl, sr = mix_targets(cur[11], cur[10])                 # opz_mix_start at the note-on
    bad, seen = [], set()
    for b in range(int(36 * step + 0.5) // 64):
        for t, idx, byte in events:
            if b * 64 >= t:
                tgt[idx] = param_word(byte)
        for idx in cur:
            diff = (tgt[idx] - cur[idx] + 2**31) % 2**32 - 2**31
            cur[idx] = (cur[idx] + (diff >> 6)) % 2**32
        tl, tr = mix_targets(cur[11], cur[10])
        for i in range(64):
            gl = tl if (sl, sr) == (tl, tr) else ((sl << 6) + (i + 1) * (tl - sl)) >> 6
            gr = tr if (sl, sr) == (tl, tr) else ((sr << 6) + (i + 1) * (tr - sr)) >> 6
            want = ((0x20000000 * gl) >> 15, (0x20000000 * gr) >> 15)
            got = (x[2 * (b * 64 + i)], x[2 * (b * 64 + i) + 1])
            if got != want and not bad:
                bad.append("block %d frame %d: L, R = %r, model %r" % (b, i, got, want))
            seen.add((gl == 0, gr == 0))
        sl, sr = tl, tr
    if not bad and not {(True, False), (False, True)} <= seen:
        bad.append("the pans never reached hard left and hard right")
    if not bad and any(x[2 * (int(34 * step) + i)] or x[2 * (int(34 * step) + i) + 1] for i in range(64)):
        bad.append("level 0 is not silent")
    res.check(name, not bad, "level 255 -> 128 -> 0, pan 127 -> 0 -> 255, every sample as modelled"
              if not bad else bad[0], log)

    name = "sdk: what an FX track is fed follows the instrument's level and pan"
    fx = os.path.join(build, "engines", "opz_test_passthrough.dylib")
    wav_fx = os.path.join(tmp, "mix_probe_fx.wav")
    rc, log = opzsim(build, dylib, scn, "--fx", fx, out=wav_fx)
    res.check(name, rc == 0 and read_wav32(wav_fx) == x, "exit %d" % rc if rc else
              "FX input identical to the levelled, panned track", log)


# No pan byte is the exact centre: 127 (0x3fbfbfbf) has gain_L = 1 and gain_R = 0.996, 128 (0x40404040) the reverse.
MIX_VARIANTS = {"ref": (255, 127), "ref_r": (255, 128), "half": (128, 127), "left": (255, 0), "right": (255, 255),
                "off": (0, 127)}


def mix_scenario(src, dst, level, pan):
    """src with every track at this level and pan: expectations dropped (a silent variant would fail them), level
    and pan moves dropped, the two set after every track and preset line."""
    out = []
    with open(src) as f:
        for line in f:
            w = line.split()
            if w and w[0] == "expect":
                continue
            if len(w) > 1 and w[0] in ("param", "sweep") and w[1].lower() in ("level", "pan", "10", "11"):
                continue
            if len(w) > 1 and w[0] in ("sample", "preset_file") and w[1] not in ("builtin", "unloaded") \
                    and not os.path.isabs(w[1]):         # the variant lives elsewhere: re-anchor relative paths
                target = os.path.join(os.path.dirname(os.path.abspath(src)), w[1])
                line = " ".join([w[0], os.path.relpath(target, os.path.dirname(os.path.abspath(dst)))] + w[2:])
            out.append(line.rstrip("\n") + "\n")
            if w and w[0] in ("track", "preset"):
                out.append("param level %d\nparam pan %d\n" % (level, pan))
    with open(dst, "w") as f:
        f.writelines(out)


def engine_mix_check(build, res, tmp, name, plug, dylib, scn):
    """An instrument engine must apply the track's level and pan (the OS does not): renders scn at level 255/128/0
    and pan centre/hard left/hard right and compares the channels."""
    label = "%s: level and pan dials (engine-abi.md §4)" % name
    x = {}
    for key, (level, pan) in MIX_VARIANTS.items():
        src = os.path.join(tmp, name, "mix_%s.scn" % key)
        mix_scenario(scn, src, level, pan)
        wav = os.path.join(tmp, name, "mix_%s.wav" % key)
        rc, log = opzsim(build, dylib, src, "--slot", str(plug["memoryslot"]), "--trackmask",
                         str(plug["plug"]["trackmask"]), out=wav)
        if rc != 0:
            res.check(label, False, "%s variant: opzsim exit %d" % (key, rc), log)
            return
        w = read_wav32(wav)
        x[key] = (w[0::2], w[1::2])

    def rms(v):
        return (sum(s * s for s in v) / max(len(v), 1)) ** 0.5

    bad = []
    ref_l, ref_r = x["ref"]
    if rms(ref_l) < 1e5 or rms(ref_r) < 1e5:
        bad.append("the reference render is (near) silent")
    for ch, (r, h) in enumerate(zip(x["ref"], x["half"])):
        ratio = rms(h) / max(rms(r), 1)
        if not 0.49 <= ratio <= 0.515:
            bad.append("level 128 gives %.3f x level 255 on %s (want ~0.502)" % (ratio, "LR"[ch]))
    if any(x["left"][1]) or x["left"][0] != ref_l:
        bad.append("pan 0 is not hard left (R silent, L as at the centre)")
    if any(x["right"][0]) or x["right"][1] != x["ref_r"][1]:
        bad.append("pan 255 is not hard right (L silent, R as at the centre)")
    if any(x["off"][0]) or any(x["off"][1]):
        bad.append("level 0 is not silent")
    res.check(label, not bad, "%s: level 128 = -6 dB, pans hard left/right, level 0 silent" % os.path.basename(scn)
              if not bad else bad[0])


# The drum envelope as described in sdk/docs/engine-abi.md §3, written independently of the C code.

def sat32(x):
    return max(-2**31, min(2**31 - 1, x))


def cdiv(a, b):
    """C integer division (truncates toward zero)."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def ahd_layout(length, pa, pb, pc, pd):
    if pa & 0xffff8000 and pb < 0x400000:
        pb = 0x400000
    start = min(pa * length >> 31, length - 512)
    attack = ((length - start) >> 1) * pb >> 31
    rest = length - start - attack
    hold = max(rest * pc >> 31, 512)
    held = start + attack + hold
    dlen = min(min(pd, 0x40000000) * 2, 2**31 - 1)
    shape = (pd - 0x40000000) * 2 if pd >= 0x40000000 else 0
    coef = 2**31 - 1 if pd >= 0x40000000 else 0x7ffe5c92
    return [start, min(start + attack, length - 192), min(held, length - 64),
            held + (dlen * (rest - hold) >> 31), coef, shape]


def ahd_update(env, pos, a_end, h_end, d_end, coef, shape):
    """env = [level, coef, step, stage]; stages 1 attack, 7 hold, 4 decay, 0 idle."""
    if env[3] == 1:
        if a_end > pos:
            env[1:3] = [2**31 - 1, (2**31 - 1) // (a_end - pos)]
            return
        env[3] = 7
    if env[3] == 7:
        if pos < h_end:
            env[0:3] = [2**31 - 1, 2**31 - 1, 0]
            return
        env[3] = 4
    if env[3] == 4:
        if d_end > pos:
            left = d_end - pos
            f = sat32(cdiv(left << 31, d_end - h_end or 1))
            t = f
            for _ in range(3):
                t = t * shape >> 31
            u = sat32(2**31 - 1 - t)
            u4 = u
            for _ in range(3):
                u4 = u4 * u >> 31
            lin = -cdiv(env[0], max(left, 64))
            env[1:3] = [coef, u4 * lin >> 31]
            return
        env[0:4] = [env[0], 0x70000000, 0, 0]


def ahd_check(build, res, tmp):
    name = "host: drum envelope (opz_ahd_layout, updateParametersAHD) matches engine-abi.md"
    wav = os.path.join(tmp, "ahd_probe.wav")
    rc, log = opzsim(build, os.path.join(build, "fixtures", "ahd_probe.dylib"),
                     os.path.join(FIXTURES, "ahd_probe.scn"), out=wav)
    if rc != 0:
        res.check(name, False, "exit %d" % rc, log)
        return
    x = read_wav32(wav)
    left, right = x[0::2], x[1::2]
    bad, hits, i = [], 0, 0
    while i < len(left) and not bad:
        if left[i] == 0:
            i += 1
            continue
        hits += 1
        params, layout = list(right[i:i + 4]), list(right[i + 4:i + 10])
        if ahd_layout(8192, *params) != layout:
            bad.append(("layout", i, layout, ahd_layout(8192, *params)))
            break
        env, pos, first = [0, 2**31 - 1, 0, 1], layout[0], True
        while env[3]:
            ahd_update(env, pos, *layout[1:])
            if not env[3]:
                break
            for k in range(64):
                env[0] = sat32(sat32((env[1] * env[0] + 2**30) >> 31) + env[2])
                j = i + k
                if left[j] != env[0] >> 1 or (not (first and k < 10) and right[j] != env[3]):
                    bad.append((j, left[j], env[0] >> 1, right[j], env[3]))
                    break
            if bad:
                break
            i, pos, first = i + 64, pos + 64, False
        if not bad and any(left[i:i + 64]):
            bad.append(("not idle after decay_end", i))
    res.check(name, not bad and hits == 3, "%d hits checked" % hits if not bad else "first mismatch %r" % (bad[0],),
              log)


# The sequencer clock and opz_tempo.h as described in sdk/docs/engine-abi.md ("Sequencer keys"), written
# independently of the C code: 24 ticks per step, tick k at k * 44100 * 60 / (96 * BPM) frames after play; a tick
# plays (step, tick) and advances the counter, which runs on while stopped; stop moves the step to 0 (tick < 8)
# or to the last step; OpzClock snaps to the tick just played (at a pattern wrap: the last tick of the last step)
# and interpolates without passing the next tick.

def i32(x):
    x &= 0xffffffff
    return x - 2**32 if x >= 2**31 else x


def tempo_pos(step, tick):
    return ((step << 20) + (tick << 17) // 3) & 0xffffffff


def tempo_check(build, res, tmp):
    name = "host: sequencer clock (keys 4-7) and opz_tempo.h match engine-abi.md"
    wav = os.path.join(tmp, "tempo_probe.wav")
    rc, log = opzsim(build, os.path.join(build, "fixtures", "tempo_probe.dylib"),
                     os.path.join(FIXTURES, "tempo_probe.scn"), out=wav)
    if rc != 0:
        res.check(name, False, "exit %d" % rc, log)
        return
    x = read_wav32(wav)
    left, right = x[0::2], x[1::2]
    bpm, step_frames = 137, 44100 * 15 / 137
    events = sorted([(int(9.4 * step_frames + 0.5), 0), (int(12 * step_frames + 0.5), 1)])
    playing, step, tick, origin, ticks = 1, 0, 0, 0, 0
    pos, inc, count, jumps = 0, (bpm * 2**24 + 82687) // 165375, -1, []
    want_r = [2646000 // bpm, 661500 // bpm, 3 * 10584000 // (16 * bpm), (4 * bpm << 32) // 10584000, inc,
              i32(tempo_pos(-1, 23)), inc << 4]
    bad = []
    for b in range(len(left) // 64):
        t0 = b * 64
        while events and events[0][0] <= t0:
            when, play = events.pop(0)
            if play:
                playing, step, tick, origin, ticks = 1, 0, 0, when, 0
            elif playing:
                playing, step = 0, 0 if tick < 8 else 15
        while ticks * 27562.5 <= (t0 - origin) * bpm:
            tick += 1
            if tick == 24:
                tick, step = 0, (step + 1) % 16
            ticks += 1
        c = step * 24 + tick
        nxt = tempo_pos(step, tick)
        if c != count:
            wrap = count >= 0 and c == 0 and count % 24 == 23
            jumped = not (count >= 0 and (c == count + 1 or wrap))
            pos = tempo_pos(step, tick - 1) if tick else tempo_pos(count // 24 if wrap else step - 1, 23)
            count = c
        else:
            jumped = False
            pos = (pos + min(inc, (nxt - 1 - pos) & 0xffffffff)) & 0xffffffff
        if jumped:
            jumps.append(b)
        want_l = [step, tick, playing, bpm, i32(pos), int(jumped), i32(((pos << 10) & 0xffffffff) >> 1)]
        got_l, got_r = list(left[t0:t0 + 7]), list(right[t0:t0 + 7])
        if got_l != want_l or got_r != want_r:
            bad.append((b, got_l, want_l, got_r, want_r))
            break
        # While playing, the position is never ahead of the music and at most a block behind it.
        ideal = int((t0 - origin) / step_frames * 2**20)
        lag = ((ideal - pos + 2**23) % 2**24) - 2**23        # on the 16-step pattern
        if playing and not -4 <= lag <= inc + 4:
            bad.append((b, "lag", lag))
            break
    res.check(name, not bad and jumps == [0, 710, 906], "%d blocks, jumps at blocks %s" % (len(left) // 64, jumps)
              if not bad else "first mismatch %r" % (bad[0],), log)


# getSample as research/samples.md describes the device (FUN_80100d78): pack id = init's arg2 = mem = crc32 of
# the plug's samples path; slice (note + 19) mod 24 clamped to the pack's keys; the slice words from the pack's
# op-1 JSON as tools/opz_samples.py reads it. Notes as in tests/fixtures/sample_probe.scn.

SAMPLE_PROBE = ([("opzk_testkit.aif", n) for n in list(range(53, 77)) + [0, 52, 127]]
                + [("opzk_testkit5.aif", n) for n in (53, 57, 58, 76, 0)]
                + [("opzk_plain.aif", n) for n in (53, 72)] + [("builtin", 60), (None, 60)]
                + [("opzk_synth.aif", n) for n in (60, 72)])


def expected_slices(build, name):
    """(samples path, [slice words per key]) of a pack the way the OS serves it: tools/opz_sampleplay.slices, the
    reference for every word (frames, knobs, rate, loopStart, loopEnd, pitch, editable), plus the samples."""
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import opz_samples as smp  # noqa: E402
    import opz_sampleplay as play  # noqa: E402
    if name is None:
        return "", []
    if name == "builtin":
        return "samplepacks/user/opzk_builtin.aif", None
    with open(os.path.join(build, "samples", name), "rb") as f:
        info = smp.parse(f.read())
    out = []
    for s in play.slices(info):
        data = info["samples"][s["start"]:s["start"] + s["frames"]]
        out.append([s["frames"], s["knobs"], s["rate"], s["loopStart"], s["loopEnd"], s["pitch"] & 0xffffffff,
                    s["editable"], data[0] & 0xffffffff if data else 0, data[-1] & 0xffffffff if data else 0,
                    sum(data) & 0xffffffff])
    return "samplepacks/user/" + name, out


def sample_check(build, res, tmp):
    import zlib
    name = "host: getSample serves OP-1 pack slices by pack id and key, as on the device (samples.md)"
    wav = os.path.join(tmp, "sample_probe.wav")
    rc, log = opzsim(build, os.path.join(build, "fixtures", "sample_probe.dylib"),
                     os.path.join(FIXTURES, "sample_probe.scn"), out=wav)
    if rc != 0:
        res.check(name, False, "exit %d" % rc, log)
        return
    x = [v & 0xffffffff for v in read_wav32(wav)]
    hits = [b * 128 for b in range(len(x) // 128) if any(x[b * 128:b * 128 + 13])]
    bad = []
    if len(hits) != len(SAMPLE_PROBE):
        bad.append(("hits", len(hits), len(SAMPLE_PROBE)))
    packs = {}
    for at, (pack, note) in zip(hits, SAMPLE_PROBE):
        if pack not in packs:
            packs[pack] = expected_slices(build, pack)
        path, slices = packs[pack]
        pid = zlib.crc32(path.encode()) if path else 0
        got = x[at:at + 13]
        if pack is None:
            want = [0] * 11
        elif slices is None:                                    # the built-in buffer: checked by its length only
            want = got[:1] + [44100] + got[2:11]
        else:
            key = min((note + 19) % 24, len(slices) - 1)
            want = [pid] + slices[key]
        want = [pid] + want[1:11] + [note, pid]
        if got != want:
            bad.append((pack, note, got, want))
            break
    res.check(name, not bad, "%d notes on %d packs" % (len(hits), len(packs)) if not bad else
              "first mismatch %r" % (bad[0],), log)


# The shared tables (engine-abi.md §5) and the opz_tables.h accessors, against tools/opz_tables.py.

def tables_check(build, res, tmp):
    name = "host: shared tables and opz_tables.h accessors match tools/opz_tables.py (engine-abi.md §5)"
    sys.path.insert(0, os.path.join(REPO, "tools"))
    import opz_tables as T  # noqa: E402
    wav = os.path.join(tmp, "tables_probe.wav")
    rc, log = opzsim(build, os.path.join(build, "fixtures", "tables_probe.dylib"),
                     os.path.join(FIXTURES, "tables_probe.scn"), out=wav)
    if rc != 0:
        res.check(name, False, "exit %d" % rc, log)
        return
    x = read_wav32(wav)

    def h(n):
        n = (n * 0x9E3779B1) & 0xffffffff
        n ^= n >> 15
        n = (n * 0x85EBCA77) & 0xffffffff
        return n ^ (n >> 13)

    def s32(v):
        return (v + 2**31) % 2**32 - 2**31

    sine, rf = T.g_sine(), T.rfilter_coeffs()
    want = T.k_lp_table() + sine + T.g_sine_table() + sum(rf, []) + T.release_table()
    want += [T.sine_lerp(sine, h(n)) for n in range(512)]
    want += [T.sine16_lerp(T.g_sine_table(), h(n)) for n in range(256)]
    for p in range(64):
        want += T.rf_lookup(rf, h(2 * p), h(2 * p + 1)) + T.rf_lookup(rf, h(2 * p), h(2 * p + 1), os_rows=True)
    c = T.rf_lookup(rf, param_word(60), param_word(100), os_rows=True)
    mul = lambda a, b: sat32((a * b) >> 31)                         # noqa: E731
    s1 = s2 = 0
    for n in range(256):
        xin = s32(h(n)) >> 3
        y = sat32(mul(c[0], xin) + s1)
        u = sat32(mul(c[1], xin) - mul(c[3], y))
        s1, s2 = sat32(s2 + sat32(2 * u)), sat32(mul(c[2], xin) - mul(c[4], y))
        want.append(y)
    want.append(31)
    bad = [(k, x[k], w) for k, w in enumerate(want) if k >= len(x) or x[k] != w]
    tail = any(x[len(want):len(want) + 256])
    detail = ("%d mismatches, first (index, got, want) %r" % (len(bad), bad[0]) if bad else
              "voice did not end" if tail else "%d words" % len(want))
    res.check(name, not bad and not tail, detail, log)


# --------------------------------------------------------------------------- golden renders

BFIN = os.path.expanduser("~/opt/bfin-elf")
SIZE_WARNING = "B exceeds the "                     # opzsim's host-estimate slot warning


def device_fit(name, tmp):
    """Build the engine for the device. Returns a note ("device build ... fits") if it fits its slot window, or
    saying the fit was not checked without the toolchain (CI, contributors without bfin-elf: opz_build.py checks it
    when the engine is built for the device); raises RuntimeError if the device build fails (e.g. it does not fit)."""
    if not all(os.access(os.path.join(BFIN, "bin", "bfin-elf-" + t), os.X_OK) for t in ("gcc", "objdump")):
        return "no Blackfin toolchain, device fit not checked"
    tools = os.path.join(REPO, "tools")
    r = subprocess.run([sys.executable, os.path.join(tools, "opz_build.py"), os.path.join(ENGINES, name),
                        "--toolchain", BFIN, "-o", os.path.join(tmp, "bfin")], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError((r.stdout + r.stderr).strip().splitlines()[-1] if (r.stdout + r.stderr).strip()
                           else "opz_build.py exit %d" % r.returncode)
    sys.path.insert(0, tools)
    import opz_pack  # noqa: E402
    with open(os.path.join(tmp, "bfin", name, name + ".elf"), "rb") as f:
        blocks = opz_pack.blocks_from_elf(f.read())
    used = sum(len(b["payload"]) if b["kind"] == "data" else b["count"] for b in blocks)
    return "device build %d B fits its slot" % used


def band_distance(a, b):
    pairs = [(x, y) for x, y in zip(a, b) if max(x, y) > -100.0]
    return max((abs(x - y) for x, y in pairs), default=0.0)


def golden_tests(build, res, tmp, update, spectral, tolerance, only):
    for name in sorted(os.listdir(ENGINES)):
        plug_path = os.path.join(ENGINES, name, "plug.json")
        if not os.path.isfile(plug_path) or (only and name not in only):
            continue
        with open(plug_path) as f:
            plug = json.load(f)
        dylib = os.path.join(build, "engines", name + ".dylib")
        tests = os.path.join(ENGINES, name, "tests")
        try:
            fit = None if plug.get("sim_only") else device_fit(name, tmp)   # sim_only: float code, never installed
        except RuntimeError as e:
            res.check("%s: device build" % name, False, str(e))
            fit = None
        golden = os.path.join(tests, "golden")
        scenarios = sorted(s for s in os.listdir(tests) if s.endswith(".scn")) if os.path.isdir(tests) else []
        if not scenarios:
            res.check("%s: has golden scenarios" % name, False, "no tests/*.scn")
            continue
        for scn in scenarios:
            stem = scn[:-4]
            label = "%s/%s" % (name, stem)
            wav = os.path.join(tmp, name, stem + ".wav")
            stats_path = os.path.join(tmp, name, stem + ".json")
            os.makedirs(os.path.dirname(wav), exist_ok=True)
            rc, log = opzsim(build, dylib, os.path.join(tests, scn),
                             "--slot", str(plug["memoryslot"]), "--trackmask", str(plug["plug"]["trackmask"]),
                             out=wav, stats=stats_path)
            if rc != 0:
                res.check(label, False, "opzsim exit %d" % rc, log)
                continue
            warnings = [l for l in log.splitlines() if l.startswith("warning") and not (fit and SIZE_WARNING in l)]
            digest = sha256(wav)
            with open(stats_path) as f:
                stats = json.load(f)
            spectrum = {"rms_dbfs": stats["rms_dbfs"], "peak_dbfs": stats["peak_dbfs"],
                        "bands_db": stats["bands_db"], "frames": stats["frames"]}
            sha_file = os.path.join(golden, stem + ".wav.sha256")
            spec_file = os.path.join(golden, stem + ".spectrum.json")
            if update:
                os.makedirs(golden, exist_ok=True)
                with open(sha_file, "w") as f:
                    f.write("%s  %s.wav\n" % (digest, stem))
                with open(spec_file, "w") as f:
                    json.dump(spectrum, f, indent=2)
                    f.write("\n")
                res.check(label, True, "golden written (%s…)" % digest[:12], log)
                continue
            if not os.path.isfile(sha_file):
                res.check(label, False, "no golden file; run make -C sim golden ENGINE=%s" % name, log)
                continue
            with open(sha_file) as f:
                want = f.read().split()[0]
            if digest == want:
                note = "; " + warnings[0] if warnings else ("; host size estimate over, " + fit
                                                            if fit and SIZE_WARNING in log else "")
                res.check(label, not warnings, "sha256 %s…%s" % (digest[:12], note), log)
                continue
            dist = None
            if os.path.isfile(spec_file):
                with open(spec_file) as f:
                    ref = json.load(f)
                dist = max(band_distance(ref["bands_db"], spectrum["bands_db"]),
                           abs(ref["rms_dbfs"] - spectrum["rms_dbfs"]))
            detail = "sha256 differs; spectral distance %s dB" % ("%.2f" % dist if dist is not None else "n/a")
            ok = spectral and dist is not None and dist <= tolerance
            res.check(label, ok, detail + (" (within %.2f dB)" % tolerance if ok else ""), log)
        if not update and plug["plug"]["trackmask"] & 0xff and not plug["plug"]["trackmask"] & 0xf00:
            engine_mix_check(build, res, tmp, name, plug, dylib, os.path.join(tests, scenarios[0]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", default=os.path.join(SIM, "build"))
    ap.add_argument("--update", action="store_true", help="write golden files instead of comparing")
    ap.add_argument("--spectral", action="store_true", help="accept hash mismatches within --tolerance")
    ap.add_argument("--tolerance", type=float, default=0.5, help="max octave-band / rms difference in dB")
    ap.add_argument("--engine", action="append", help="only test these engines")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    build = os.path.abspath(args.build)
    tmp = os.path.join(build, "renders")
    os.makedirs(tmp, exist_ok=True)
    res = Results(args.verbose)
    if not args.update:
        host_checks(build, res, tmp)
    golden_tests(build, res, tmp, args.update, args.spectral, args.tolerance, args.engine)
    print("\n%d passed, %d failed" % (res.passed, res.failed))
    return 1 if res.failed else 0


if __name__ == "__main__":
    sys.exit(main())
