#!/usr/bin/env python3
"""Generate engines/table/table_waves.h: table's 7 x 6 grid of band-limited waves.

    python3 engines/table/gen_tables.py               # rewrite table_waves.h
    python3 engines/table/gen_tables.py --check       # exit 1 if table_waves.h is out of date
    python3 engines/table/gen_tables.py --report      # crest factors, levels and an aliasing test per mip

Every wave is our own additive recipe (no sampled or copied tables): a list of complex Fourier coefficients
c[h] for harmonics h = 1 .. 127 of one period. Shapes made of straight lines (saw, square, pulse, triangle, hard
sync) get their exact coefficients from fourier_pwl, smooth shapes (FM) from an FFT of 4,096 points, the rest
(organ, vocal, resonance, metal) are written as spectra directly. Then each wave is

  1. shifted in time so its fundamental is in sine phase (morphs between waves keep their fundamentals in step,
     so the level does not dip half-way between two waves),
  2. normalised to the same RMS, with a cap on the peak (waves with a high crest factor are a little quieter),
  3. written once per mip level: mip k keeps the harmonics h <= H[k] and has L[k] = 2 (H[k] + 1) samples. Each
     harmonic is raised by 1 / |K(h / L)|, the loss of the engine's 4-point Hermite (Catmull-Rom) interpolation
     at that harmonic, so the passband is flat on every mip,
  4. and, for the notes above the last mip (fewer than 15 harmonics fit under F_TOP), written as the coefficients
     (a_h, b_h) of a_h cos(h t) + b_h sin(h t) for h = 1 .. ADD_H, which the engine sums itself (its additive
     path: no interpolation, so no images at all).

The engine reads mip k while the note's highest harmonic H[k] x f stays at or below F_TOP, so no harmonic of the
table is ever above half the sample rate; what remains is the interpolation's images, which --report measures
(inharmonic energy relative to the note, for mips at the top and the middle of their range, with the engine's own
integer arithmetic). Above the last mip the additive path fades each harmonic out between F_TOP and F_TOP + 2756 Hz.

Python 3 standard library only. MIT licence, see CONTRIBUTING.md.
"""

import cmath
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "table_waves.h")

FS = 44100.0
F_TOP = 16000.0     # highest harmonic allowed on a mip (Hz); table.c's k_mip_inc follows it
HMAX = 127
# mip levels: (samples, highest harmonic), one per octave, 2x oversampled (L = 2 (H + 1)). At 2x the
# interpolation's first images sit between F_TOP and 2 F_TOP, so what folds back lands above fs - 2 F_TOP = 12 kHz;
# 4x tables of the same size alias less in total but lower down (--report: below 11 kHz 2x is the cleaner one).
# Above the last mip (notes above 1,067 Hz) the engine sums the first ADD_H harmonics itself: with as few as 7, 3
# or 1 harmonics the second images of a 2x table near 44.1 kHz fold down to -40 dB under the note, an additive
# sum has none, and 7 coefficient pairs are smaller than the three tables they replace.
MIPS = [(256, 127), (128, 63), (64, 31), (32, 15)]
ADD_H = 7
PEAK = 16000        # largest table value (15 bits: differences of two samples always fit int16)
CREST_CAP = 1.6     # no wave peaks above 1.6 x the band-limited saw's peak at the common RMS

BANKS = ["analog", "organ", "vocal", "reso", "sync", "fm", "metal"]
WAVES = 6


# --------------------------------------------------------------------------- spectra

def fourier_pwl(segments, hmax=HMAX):
    """Exact Fourier coefficients c[1..hmax] of a periodic function on [0, 1) made of straight segments
    (t0, t1, v0, v1): f(t) = v0 + (v1 - v0) (t - t0) / (t1 - t0). Jumps are just consecutive segments."""
    c = [0j] * (hmax + 1)
    for h in range(1, hmax + 1):
        w = 2 * math.pi * h
        acc = 0j
        for t0, t1, v0, v1 in segments:
            if t1 <= t0:
                continue
            b = (v1 - v0) / (t1 - t0)
            a = v0 - b * t0

            def prim(t):
                e = cmath.exp(-1j * w * t)
                return e * ((a + b * t) / (-1j * w) + b / (w * w))
            acc += prim(t1) - prim(t0)
        c[h] = acc
    return c


def fft(x):
    """Iterative radix-2 FFT of a list of complex numbers (length a power of two)."""
    n = len(x)
    a = list(x)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    size = 2
    while size <= n:
        wn = cmath.exp(-2j * math.pi / size)
        half = size // 2
        tw = [wn ** k for k in range(half)]
        for start in range(0, n, size):
            for k in range(half):
                u = a[start + k]
                v = a[start + k + half] * tw[k]
                a[start + k] = u + v
                a[start + k + half] = u - v
        size *= 2
    return a


def fourier_samples(f, n=4096, hmax=HMAX):
    """Fourier coefficients of a smooth periodic function f(t), t in [0, 1), from n samples."""
    X = fft([complex(f(i / n)) for i in range(n)])
    return [0j] + [X[h] / n for h in range(1, hmax + 1)]


def sine_spectrum(amps, phases=None):
    """c[h] for sum_h amps[h] sin(2 pi h t + phases[h])."""
    c = [0j] * (HMAX + 1)
    for h, a in amps.items():
        if 1 <= h <= HMAX:
            p = phases.get(h, 0.0) if phases else 0.0
            c[h] += a * cmath.exp(1j * p) / 2j
    return c


def saw():
    return fourier_pwl([(0, 1, 1, -1)])                      # falling ramp: sum sin(h t) / h, fundamental in sine phase


def pulse(width):
    return fourier_pwl([(0, width, 1, 1), (width, 1, -1, -1)])


def triangle():
    return fourier_pwl([(0, 0.25, 0, 1), (0.25, 0.75, 1, -1), (0.75, 1, -1, 0)])


def sync_saw(ratio):
    """A rising saw at `ratio` x the note, reset at every period of the note (hard sync)."""
    segs = []
    t = 0.0
    step = 1.0 / ratio
    while t < 1.0 - 1e-12:
        t1 = min(t + step, 1.0)
        segs.append((t, t1, -1.0, -1.0 + 2.0 * (t1 - t) / step))
        t = t1
    return fourier_pwl(segs)


def resonator(f, fc, bw):
    """Complex response of a two-pole resonance at fc (bandwidth bw), unity at its peak; f, fc, bw in Hz."""
    s = 1j * f
    p = complex(-bw / 2.0, fc)
    pc = p.conjugate()
    return (s * bw) / ((s - p) * (s - pc))


def drawbars(levels):
    """Organ: harmonics 1, 2, 3, 4, 5, 6, 8 (drawbars 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1'), each bar 0..8 with
    8 = 0 dB and 3 dB less per step, as on a tonewheel organ."""
    amps = {}
    for h, lv in zip((1, 2, 3, 4, 5, 6, 8), levels):
        if lv:
            amps[h] = 10 ** (-(8 - lv) * 3 / 20.0)
    return sine_spectrum(amps)


# Male vowel formants (Peterson & Barney 1952, average values), F1..F3 in Hz, dark to bright.
VOWELS = [("u", (300, 870, 2240)), ("o", (570, 840, 2410)), ("a", (730, 1090, 2440)),
          ("ae", (660, 1720, 2410)), ("e", (530, 1840, 2480)), ("i", (270, 2290, 3010))]
VOICE_F0 = 110.0        # the pitch the formants are laid out for (A2); they move with the note, as in any wavetable


def vowel(formants):
    """A buzzy source (sum sin(h t) / h) through three parallel formant resonances (minimum phase), plus a little
    of the source itself so the fundamental stays."""
    c = [0j] * (HMAX + 1)
    gains = (1.0, 0.7, 0.35)
    bws = (90.0, 110.0, 160.0)
    src = saw()
    for h in range(1, HMAX + 1):
        f = h * VOICE_F0
        r = sum(g * resonator(f, fc, bw) for g, fc, bw in zip(gains, formants, bws))
        c[h] = src[h] * (0.12 + r)
    return c


def reso(peak, q=6.0):
    """A saw through a resonant two-pole low-pass with its cutoff at harmonic `peak` (a frozen filter sweep, the
    classic digital-wavetable 'resonant' set)."""
    src = saw()
    c = [0j] * (HMAX + 1)
    for h in range(1, HMAX + 1):
        x = h / peak
        hlp = 1.0 / complex(1.0 - x * x, x / q)
        c[h] = src[h] * hlp
    return c


def fm(index, ratio=1.0):
    return fourier_samples(lambda t: math.sin(2 * math.pi * t + index * math.sin(2 * math.pi * ratio * t)))


def metal(partials):
    """Sparse harmonic partials (h, dB) in sine phase: glassy and bell-like but still harmonic, so it stays in
    tune and never beats."""
    return sine_spectrum({h: 10 ** (db / 20.0) for h, db in partials})


# Free-free bar partial ratios 1, 2.76, 5.40, 8.93, 13.34, 18.64 on a base of two harmonics, rounded: 2, 6, 11, 18,
# 27, 37; the fundamental stays on top so the pitch is the note.
def bar(n, tilt):
    ratios = (2.756, 5.404, 8.933, 13.344, 18.638, 24.81)
    parts = [(1, 0.0)]
    for k in range(n):
        h = int(round(2 * ratios[k]))
        parts.append((h, -tilt * (k + 1)))
    parts.append((2, -9.0))
    return parts


def recipes():
    """The grid: BANKS x WAVES spectra. Along each bank (P1) the waves get brighter or more intense; the banks
    (P2) are ordered so neighbours blend well."""
    g = {}
    g["analog"] = [sine_spectrum({1: 1.0}), triangle(), saw(), pulse(0.5), pulse(0.25), pulse(0.1)]
    g["organ"] = [drawbars(b) for b in ((8, 0, 4, 0, 0, 0, 0), (8, 6, 0, 4, 0, 0, 0), (8, 8, 6, 0, 0, 0, 0),
                                        (8, 8, 8, 6, 0, 4, 0), (8, 8, 8, 8, 6, 6, 4), (8, 8, 8, 8, 8, 8, 8))]
    g["vocal"] = [vowel(f) for _, f in VOWELS]
    g["reso"] = [reso(p) for p in (1.5, 3.0, 5.0, 9.0, 16.0, 28.0)]
    g["sync"] = [sync_saw(r) for r in (1.3, 1.9, 2.6, 3.5, 4.6, 6.4)]     # never whole: the note would vanish
    g["fm"] = [fm(i) for i in (0.6, 1.2, 2.0, 3.0, 4.4, 6.2)]
    g["metal"] = [metal(bar(n, t)) for n, t in ((1, 6.0), (2, 5.0), (3, 4.0), (4, 3.0), (5, 2.5), (6, 2.0))]
    return [g[b] for b in BANKS]


# --------------------------------------------------------------------------- processing

def align(c):
    """Shift the wave in time so the fundamental is in sine phase (c[1] = -i |c[1]| / ... positive imaginary
    part negative), when there is a fundamental to speak of."""
    big = max(abs(x) for x in c[1:])
    if abs(c[1]) < 1e-3 * big:
        return c
    tau = (-math.pi / 2 - cmath.phase(c[1])) / (2 * math.pi)
    return [x * cmath.exp(2j * math.pi * h * tau) for h, x in enumerate(c)]


def rms(c, hmax=HMAX):
    return math.sqrt(sum(2 * abs(c[h]) ** 2 for h in range(1, hmax + 1)))


def catmull_rom(t):
    t = abs(t)
    if t < 1:
        return 1.5 * t ** 3 - 2.5 * t ** 2 + 1
    if t < 2:
        return -0.5 * t ** 3 + 2.5 * t ** 2 - 4 * t + 2
    return 0.0


_K = {}


def kernel_gain(nu):
    """Frequency response of the Catmull-Rom kernel at nu cycles per table sample (real: the kernel is even)."""
    if nu not in _K:
        steps = 4000
        acc = 0.0
        for i in range(-2 * steps, 2 * steps):
            t = (i + 0.5) / steps
            acc += catmull_rom(t) * math.cos(2 * math.pi * nu * t)
        _K[nu] = acc / steps
    return _K[nu]


def synth(c, n, hmax, emphasis=True):
    """n samples of one period from c[1..hmax], each harmonic raised by 1 / K(h / n), the Hermite read's loss."""
    terms = []
    for h in range(1, min(hmax, len(c) - 1) + 1):
        if abs(c[h]) < 1e-12:
            continue
        g = 1.0 / kernel_gain(h / n) if emphasis else 1.0
        terms.append((h, 2 * c[h] * g))
    out = []
    for i in range(n):
        t = i / n
        out.append(sum((a * cmath.exp(2j * math.pi * h * t)).real for h, a in terms))
    return out


def peak_of(c):
    """The largest value the engine plays from wave c: every mip as read (with the emphasis), and the additive
    path's sums of harmonics 1 .. 7, 1 .. 3 and the fundamental alone (the old mips' ranges, no emphasis)."""
    tables = max(max(abs(v) for v in synth(c, n, h)) for n, h in MIPS)
    sums = max(max(abs(v) for v in synth(c, 64, h, emphasis=False)) for h in (ADD_H, 3, 1))
    return max(tables, sums)


def coefficients(c, k):
    """(a_h, b_h) for h = 1 .. ADD_H, the wave as sum a_h cos(2 pi h t) + b_h sin(2 pi h t), in table units."""
    return [(int(round(2 * c[h].real * k)), int(round(-2 * c[h].imag * k))) for h in range(1, ADD_H + 1)]


def build():
    """Returns (waves, info, saw_peak, coefs): waves[bank][wave][mip] = list of int16, info = per-wave level facts,
    coefs[bank][wave] = ADD_H pairs (a_h, b_h)."""
    grid = recipes()
    ref = align(saw())
    ref_rms = rms(ref)
    ref_peak = max(abs(v) for v in synth([x / ref_rms for x in ref], 1024, HMAX, emphasis=False))
    norm = []
    info = []
    for b, bank in enumerate(grid):
        row = []
        for w, c in enumerate(bank):
            c = align(c)
            c = [x / rms(c) for x in c]
            peak = peak_of(c)
            scale = min(1.0, CREST_CAP * ref_peak / peak)
            row.append([x * scale for x in c])
            info.append((BANKS[b], w, peak / ref_peak, 20 * math.log10(scale),
                         20 * math.log10(max(math.sqrt(2) * abs(c[1]), 1e-9))))
        norm.append(row)
    top = max(peak_of(c) for row in norm for c in row)
    k = PEAK / top
    waves = [[[[int(round(v * k)) for v in synth(c, n, h)] for n, h in MIPS] for c in row] for row in norm]
    coefs = [[coefficients(c, k) for c in row] for row in norm]
    return waves, info, k * ref_peak, coefs


def outputs():
    waves, info, saw_peak, coefs = build()
    nb, nw = len(waves), len(waves[0])
    lines = ["/* Generated by gen_tables.py; do not edit. Our own additive waves, MIT licence, see CONTRIBUTING.md. */",
             "#ifndef TABLE_WAVES_H", "#define TABLE_WAVES_H", "",
             "#define TW_BANKS %d" % nb, "#define TW_WAVES %d" % nw, "#define TW_MIPS %d" % len(MIPS),
             "#define TW_ADD_H %d       /* harmonics of the additive path (notes above the last mip) */" % ADD_H,
             "#define TW_SAW_PEAK %d     /* the band-limited saw's peak in table units */" % round(saw_peak), "",
             "/* Banks (P2): %s. */" % ", ".join(BANKS), ""]
    offs = []
    total = 0
    for n, h in MIPS:
        offs.append(total)
        total += n * nb * nw
    lines.append("/* Mip k: wave (bank, w) starts at k_tw_off[k] + (bank * TW_WAVES + w) * L, L = 1 << k_tw_log2[k]. */")
    lines.append("static const uint16_t k_tw_off[TW_MIPS] = { %s };" % ", ".join(str(o) for o in offs))
    lines.append("static const uint8_t k_tw_log2[TW_MIPS] = { %s };"
                 % ", ".join(str(int(math.log2(n))) for n, h in MIPS))
    lines.append("/* Highest phase increment (Q32 per sample) for each mip: its top harmonic at F_TOP = %d Hz. */" % F_TOP)
    lines.append("static const uint32_t k_tw_inc[TW_MIPS] = { %s };"
                 % ", ".join("%du" % min(int(F_TOP / h / FS * 2 ** 32), 0xffffffff) for n, h in MIPS))
    lines.append("")
    lines.append("static const int16_t k_tw[%d] = {" % total)
    for k, (n, h) in enumerate(MIPS):
        lines.append("    /* mip %d: %d samples, harmonics 1 .. %d */" % (k, n, h))
        for b in range(nb):
            for w in range(nw):
                vals = waves[b][w][k]
                lines.append("    /* %s %d */" % (BANKS[b], w))
                for i in range(0, n, 16):
                    lines.append("    " + " ".join("%d," % v for v in vals[i:i + 16]))
    lines.append("};")
    lines.append("")
    lines.append("/* The additive path: wave (bank, w) is sum a_h cos(h t) + b_h sin(h t), h = 1 .. TW_ADD_H;")
    lines.append(" * word h - 1 of row bank * TW_WAVES + w holds b_h in its high, a_h in its low 16 bits (table units). */")
    lines.append("static const uint32_t k_tw_add[%d][TW_ADD_H] = {" % (nb * nw))
    for b in range(nb):
        for w in range(nw):
            words = ["0x%08x" % (((bh & 0xffff) << 16) | (ah & 0xffff)) for ah, bh in coefs[b][w]]
            lines.append("    { %s },   /* %s %d */" % (", ".join(words), BANKS[b], w))
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    return {"table_waves.h": ("\n".join(lines) + "\n").encode()}, info, waves, coefs


# --------------------------------------------------------------------------- aliasing report

def hermite_q15(xm, x0, x1, x2, f):
    """table.c's read: Catmull-Rom with Q15 weights on the differences to x0 (16 x 16 products)."""
    f2 = (f * f) >> 15
    f3 = (f2 * f) >> 15
    wm = (-f3 + 2 * f2 - f) >> 1
    w1 = (-3 * f3 + 4 * f2 + f) >> 1
    w2 = (f3 - f2) >> 1
    return x0 + ((wm * (xm - x0) + w1 * (x1 - x0) + w2 * (x2 - x0)) >> 15)


def play(table, log2l, inc, n):
    """The engine's read of one wave: Q32 phase, index = phase >> (32 - log2 L), Q15 fraction, Hermite."""
    sh = 32 - log2l
    mask = (1 << log2l) - 1
    ph = 0
    out = []
    for _ in range(n):
        i = ph >> sh
        f = (ph >> (sh - 15)) & 0x7fff
        out.append(hermite_q15(table[(i - 1) & mask], table[i], table[(i + 1) & mask], table[(i + 2) & mask], f))
        ph = (ph + inc) & 0xffffffff
    return out


def alias_db(table, log2l, f_target, n=4096, f_band=11000.0):
    """Inharmonic energy relative to the harmonic energy, in dB, in total and below f_band, for a note near
    f_target that fits a whole number of periods into n samples."""
    periods = max(1, int(round(f_target * n / FS)))
    inc = periods * (1 << 32) // n
    x = play(table, log2l, inc, n)
    X = fft([complex(v) for v in x])
    harm = sum(abs(X[k]) ** 2 for k in range(periods, n // 2, periods))
    other = sum(abs(X[k]) ** 2 for k in range(1, n // 2) if k % periods)
    low = sum(abs(X[k]) ** 2 for k in range(1, int(f_band * n / FS)) if k % periods)
    return 10 * math.log10(max(other, 1e-30) / harm), 10 * math.log10(max(low, 1e-30) / harm)


def report():
    data, info, waves, _ = outputs()
    print("peak (relative to the band-limited saw's at equal RMS), gain from the peak cap, fundamental re RMS:")
    for bank, w, crest, db, fund in info:
        print("  %-7s %d  peak x%.2f  %+.1f dB  fundamental %+.1f dB" % (bank, w, crest, db, fund))
    print("aliasing (inharmonic / harmonic energy; all, and below 11 kHz) at the top and the middle (-6 semitones)"
          " of each mip's range:")
    for k, (n, h) in enumerate(MIPS):
        f_top = F_TOP / h
        rows = []
        for b, w in ((0, 2), (0, 5), (2, 3), (4, 5), (5, 5), (6, 5)):
            r = []
            for frac in (1.0, 2 ** -0.5):
                # an odd number of periods in 4096 samples: the period is not a whole number of samples, so
                # aliases fall between the harmonics' bins instead of on them
                per = int(f_top * frac * 4096 / FS)
                per -= 1 - per % 2
                r.extend(alias_db(waves[b][w][k], int(math.log2(n)), per * FS / 4096))
            rows.append("%s%d %.0f,%.0f/%.0f,%.0f" % (BANKS[b][:3], w, r[0], r[1], r[2], r[3]))
        print("  mip %d (L %3d, H %3d, f <= %5.0f Hz): %s dB" % (k, n, h, f_top, "  ".join(rows)))
    print("  above %.0f Hz: the additive path (harmonics 1 .. %d, no images; engines/table/tests/alias.c measures it)"
          % (F_TOP / MIPS[-1][1], ADD_H))


def main(argv):
    data, _, _, _ = outputs()
    if "--report" in argv:
        report()
        return 0
    for name, blob in data.items():
        path = os.path.join(HERE, name)
        if "--check" in argv:
            with open(path, "rb") as f:
                if f.read() != blob:
                    print("%s is out of date: python3 engines/table/gen_tables.py" % name)
                    return 1
        else:
            with open(path, "wb") as f:
                f.write(blob)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
