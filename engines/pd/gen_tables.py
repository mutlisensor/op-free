#!/usr/bin/env python3
"""Generate engines/pd/pd_tables.h: the cosine table, a log2 table and every wave's DC offset and gain.

    python3 engines/pd/gen_tables.py            (writes pd_tables.h next to this file)
    python3 engines/pd/gen_tables.py --stats    (prints the level of every wave instead)

The eight waves are our own phase-distortion shapes in the Casio CZ tradition (pd.c has their definitions; WAVES
below is the same maths in floating point). Each wave is a cosine read through a distorted phase (or, for the
three resonance waves, a windowed cosine at k times the note), parameterised by its sharpness s (k for the
resonance waves), 1 = a plain cosine. For every wave and s = 2^(j/4), j = 0 .. 28 (s = 1 .. 128), the table holds
the wave's mean (removed by the engine, so no wave carries DC into the track) and a gain that gives it the RMS of
a sine, unless that would take its peak (without the DC) above PEAK_MAX. Double sine has no sharpness: its row is
indexed by the amount a = j / 28.

Python 3 standard library only. MIT licence, see CONTRIBUTING.md.
"""

import math
import os
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pd_tables.h")

NJ = 29                     # table points per wave: s = 2^(j / 4), j = 0 .. 28
N = 8192                    # points per cycle for the numerical mean and peak
PEAK_MAX = 1.41             # a wave may peak up to 3 dB over a sine (-9 dBFS per voice) to keep its loudness
COS_BITS = 9                # cosine table: 512 points + guard (linear interpolation within 2e-5)
LOG_POINTS = 16             # log2 table: 16 segments + guard


def theta_saw(ph, s):
    d = 0.5 / s
    return ph * s if ph < d else 0.5 + (ph - d) * 0.5 / (1 - d)


def theta_square(ph, s):
    h = 1 if ph >= 0.5 else 0
    u = 2 * ph - h
    return 0.5 * h + min(0.5, 0.5 * u * s)


def theta_pulse(ph, s):
    return min(1.0, ph * s)


def theta_dbl(ph, a):
    return ph + a * ((2 * ph) % 1.0 - ph)


def theta_sawpulse(ph, s):
    d = 0.5 / s
    return min(ph, d) * s + min(max(ph - d, 0.0), 0.5)


def window(kind, ph):
    if kind == 0:                                      # resonance I: decaying (saw-like) window
        return (1 - ph) ** 2
    if kind == 1:                                      # resonance II: bell (triangle-like) window
        return (4 * ph * (1 - ph)) ** 2
    if ph < 0.5:                                       # resonance III: flat, then a smooth fall (trapezoid)
        return 1.0
    t = 2 * (1 - ph)
    return t * t * (3 - 2 * t)


def res(kind):
    return lambda ph, k: 1 - window(kind, ph) * (1 - math.cos(2 * math.pi * ((ph * k) % 1.0)))


def cz(theta):
    return lambda ph, p: math.cos(2 * math.pi * theta(ph, p))


FS = 44100
# Per wave: the largest sharpness (P1 at 255) and the pitch limit: s x f0 stays at or below this many Hz (the fast
# half-cycle of the saw, square and saw-pulse, the pulse's cycle, the resonance frequency), which keeps aliasing
# low (README, tests/alias.c). Double sine has neither: its kink is smoothed by a polyBLAMP instead.
LIMITS = [(128, 6000), (128, 6000), (12, 4000), (1, FS / 2), (128, 6000), (32, 6000), (32, 6000), (32, 6000)]
DCW_TAU0 = 0.015            # DCW time constant at decay 0 and C4, seconds (x 2 every 32 decay steps)
C4_HZ = 440 * 2 ** (-9 / 12)

# name, function of (phase, parameter), parameter of table point j
WAVES = [
    ("saw", cz(theta_saw), lambda j: 2 ** (j / 4)),
    ("square", cz(theta_square), lambda j: 2 ** (j / 4)),
    ("pulse", cz(theta_pulse), lambda j: 2 ** (j / 4)),
    ("double sine", cz(theta_dbl), lambda j: j / (NJ - 1)),
    ("saw-pulse", cz(theta_sawpulse), lambda j: 2 ** (j / 4)),
    ("resonance I", res(0), lambda j: 2 ** (j / 4)),
    ("resonance II", res(1), lambda j: 2 ** (j / 4)),
    ("resonance III", res(2), lambda j: 2 ** (j / 4)),
]


def measure(f, p):
    """Mean, peak after removing the mean, RMS after removing the mean."""
    ys = [f(i / N, p) for i in range(N)]
    mean = sum(ys) / N
    peak = max(abs(y - mean) for y in ys)
    rms = math.sqrt(sum((y - mean) ** 2 for y in ys) / N)
    return mean, peak, rms


def gain_of(peak, rms):
    """The sine's RMS, unless that would take the peak above PEAK_MAX; below 2 (the table holds gain / 2)."""
    return min(PEAK_MAX / peak, math.sqrt(0.5) / rms, 65535 / 32768)


def q15(x):
    v = int(round(x * 32768))
    return max(-32768, min(32767, v))


def rows():
    """[(name, [dc Q15], [gain Q15])] per wave."""
    out = []
    for name, f, param in WAVES:
        dc, gain = [], []
        for j in range(NJ):
            mean, peak, rms = measure(f, param(j))
            dc.append(q15(mean))
            gain.append(q15(gain_of(peak, rms) / 2))
        out.append((name, dc, gain))
    return out


def fmt(vals, per_line=12, indent="    "):
    lines = []
    for i in range(0, len(vals), per_line):
        lines.append(indent + ", ".join(str(v) for v in vals[i:i + per_line]) + ",")
    return "\n".join(lines)


def generate():
    o = []
    o.append("/* Generated by gen_tables.py; do not edit. MIT licence, see CONTRIBUTING.md. */")
    o.append("#ifndef PD_TABLES_H")
    o.append("#define PD_TABLES_H")
    o.append("")
    n = 1 << COS_BITS
    o.append("/* cos(2 pi i / %d) in Q15, i = 0 .. %d (a guard point for the interpolation). */" % (n, n))
    o.append("static const int16_t k_cos[%d] = {" % (n + 1))
    o.append(fmt([q15(min(1 - 2 ** -15, math.cos(2 * math.pi * i / n))) for i in range(n + 1)], 16))
    o.append("};")
    o.append("")
    o.append("/* log2(1 + i / %d) in Q16, i = 0 .. %d. */" % (LOG_POINTS, LOG_POINTS))
    o.append("static const int32_t k_log2[%d] = {" % (LOG_POINTS + 1))
    o.append(fmt([int(round(math.log2(1 + i / LOG_POINTS) * 65536)) for i in range(LOG_POINTS + 1)], 9))
    o.append("};")
    o.append("")
    o.append("/* Per wave: log2 of the largest sharpness, log2 of the pitch limit as a phase increment (Q32 turns per sample),")
    o.append(" * both Q16. */")
    o.append("static const int32_t k_limit[8][2] = {")
    for (smax, hz), (name, _, _) in zip(LIMITS, WAVES):
        o.append("    { %d, %d },   /* %s: %g, %g Hz */" % (
            int(round(math.log2(smax) * 65536)), int(round(math.log2(hz / FS * 2 ** 32) * 65536)), name, smax, hz))
    o.append("};")
    o.append("")
    o.append("/* DCW envelope: log2 of its per-block exponent 64 / (fs ln2 tau) at decay 0 and C4 (tau %g ms), and log2 of"
             % (DCW_TAU0 * 1000))
    o.append(" * C4's phase increment; Q16. */")
    o.append("#define DCW_LOG2X0 (%d)" % int(round(math.log2(64 / (FS * math.log(2) * DCW_TAU0)) * 65536)))
    o.append("#define LOG2_C4 %d" % int(round(math.log2(C4_HZ / FS * 2 ** 32) * 65536)))
    o.append("")
    o.append("#define NJ %d" % NJ)
    o.append("")
    o.append("/* Per wave (P2 order) and sharpness s = 2^(j / 4) (double sine: amount j / %d): the wave's mean (DC) and" % (NJ - 1))
    o.append(" * half the gain that gives it a sine's RMS (its peak, without the DC, at most %.2f). Q15, { dc, gain / 2 }"
             " pairs. */" % PEAK_MAX)
    o.append("static const int16_t k_level[8][NJ][2] = {")
    for name, dc, gain in rows():
        o.append("    {   /* %s */" % name)
        pairs = ["{ %d, %d }" % (d, g) for d, g in zip(dc, gain)]
        o.append(fmt(pairs, 6, "        "))
        o.append("    },")
    o.append("};")
    o.append("")
    o.append("#endif")
    return "\n".join(o) + "\n"


def stats():
    """Level of every wave after the engine's DC removal and gain, in dB re a sine (RMS)."""
    sine_rms = math.sqrt(0.5)
    for name, f, param in WAVES:
        line = []
        for j in (0, 4, 8, 12, 16, 20, 24, 28):
            mean, peak, rms = measure(f, param(j))
            g = gain_of(peak, rms)
            line.append("%6.1f" % (20 * math.log10(rms * g / sine_rms)))
        print("%-14s %s" % (name, " ".join(line)))


if __name__ == "__main__":
    if "--stats" in sys.argv:
        stats()
    else:
        with open(OUT, "w") as fh:
            fh.write(generate())
