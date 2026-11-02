#!/usr/bin/env python3
"""The OS's shared DSP tables (callback entries +0x08 .. +0x18), regenerated from their formulas.

Reference for sdk/docs/engine-abi.md §5 "Shared tables": opz_cycles.py puts these tables into the cycle driver's
callback table, sim/tests/golden.py checks opzsim's C generator (sdk/include/opz_tables_host.h) against them, and
tools/tests/test_opz_tables.py checks their format. Formulas only: no TE data. Against the device tables of OS
1.2.45 (opz.ldr, read locally, never committed) they agree to within 1 LSB (g_sine, g_sineTable, rFilterCoeffs,
releaseTable) and 11 LSB (k_lp_table).

    python3 tools/opz_tables.py            # print a summary of every table
"""

import math

FS = 44100

SINE_N = 8192                 # g_sine: 8,192 points of one cycle + a guard point
SINE_AMP = 0x7FFF0000         # its peak: Q15 full scale << 16, not 0x7FFFFFFF
SINE16_N = 256                # g_sineTable: 256 x int16, one cycle
KLP_N = 129                   # k_lp_table
RF_ROWS, RF_COLS = 9, 65      # rFilterCoeffs: 5 pointers to [9][65] Q31
RELEASE_N = 128               # releaseTable

# ISO 266 nominal third-octave frequencies, 25 Hz .. 20 kHz: the cutoff columns of rFilterCoeffs
RF_FREQS = (25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000,
            2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000)
RF_Q_LP = (0.2, 0.4, 0.6, 0.8, 1.0, 4.0, 8.0, 16.0, 16.0)       # per row (resonance), low-pass side
RF_Q_HP = (0.2, 0.4, 0.6, 0.8, 1.0, 2.0, 3.0, 4.0, 4.0)         # high-pass side
# columns 30..34: unity filters shaped like their neighbours, (b0, b1/2, b2, a1/2, a2)
RF_FLAT = {30: (1, .25, .5, .25, .5), 31: (1, 0, 0, 0, 0), 32: (1, 0, 0, 0, 0), 33: (1, 0, 0, 0, 0),
           34: (1, -.75, .5, -.75, .5)}

# releaseTable: per-sample decay multiplier e^(-1/tau) with tau in samples; a least-squares fit (our constants)
REL_TAU0 = 33.64711
REL_A, REL_B, REL_C, REL_D = 0.9581, 248.8227, 11.72398, 0.079528


def q31(x):
    """x * 2^31 rounded to nearest (half away from zero), saturated."""
    v = x * 2147483648.0
    v = math.floor(v + 0.5) if v >= 0 else -math.floor(-v + 0.5)
    return max(-2**31, min(2**31 - 1, int(v)))


def g_sine():
    """+0x0C: 8,193 x Q31, one cycle, peak 0x7FFF0000. Index phase >> 19 of a Q32 phase."""
    out = []
    for i in range(SINE_N + 1):
        v = SINE_AMP * math.sin(2 * math.pi * i / SINE_N)
        out.append(int(math.floor(v + 0.5)) if v >= 0 else -int(math.floor(-v + 0.5)))
    return out


def g_sine_table():
    """+0x10: 256 x int16, one cycle, min(32767, round(32768 sin)). Natural index phase >> 24."""
    out = []
    for k in range(SINE16_N):
        v = 32768 * math.sin(2 * math.pi * k / SINE16_N)
        r = int(math.floor(v + 0.5)) if v >= 0 else -int(math.floor(-v + 0.5))
        out.append(max(-32768, min(32767, r)))
    return out


def klp_hz(i):
    """The frequency of k_lp_table[i]: 10 Hz * 2000^(i/128), 10 Hz .. 20 kHz."""
    return 10.0 * 2000.0 ** (i / 128.0)


def k_lp_table():
    """+0x08: 129 one-pole coefficients c (y += c (x - y)); two poles in series are -3 dB at klp_hz(i)."""
    out = []
    g2 = math.sqrt(0.5)                       # each pole -1.5 dB: |H|^2 = 2^(-1/2)
    for i in range(KLP_N):
        w = 2 * math.pi * klp_hz(i) / FS
        a = (1 - g2 * math.cos(w)) / (1 - g2)
        p = a - math.sqrt(a * a - 1)
        out.append(q31(1 - p))
    return out


def rf_column(col, row):
    """(b0, b1/2, b2, a1/2, a2) of rFilterCoeffs at [row][col], as floats (RBJ cookbook, a0 normalised)."""
    if col in RF_FLAT:
        return RF_FLAT[col]
    lp = col < 30
    f = RF_FREQS[col if lp else col - 35]
    q = RF_Q_LP[row] if lp else RF_Q_HP[row]
    w = 2 * math.pi * f / FS
    cw, al = math.cos(w), math.sin(w) / (2 * q)
    a0 = 1 + al
    b0 = (1 - cw) / 2 if lp else (1 + cw) / 2
    b1 = (1 - cw) if lp else -(1 + cw)
    return (b0 / a0, b1 / 2 / a0, b0 / a0, -cw / a0, (1 - al) / a0)


def rfilter_coeffs():
    """+0x14: five tables b0, b1/2, b2, a1/2, a2, each 585 x Q31 laid out [row 0..8][column 0..64]."""
    tabs = [[], [], [], [], []]
    for row in range(RF_ROWS):
        for col in range(RF_COLS):
            for k, v in enumerate(rf_column(col, row)):
                tabs[k].append(q31(v))
    return tabs


def release_tau(i):
    """Time constant of releaseTable[i] in samples."""
    return REL_TAU0 if i == 0 else REL_A + REL_B * i + REL_C * math.exp(REL_D * i)


def release_table():
    """+0x18: 128 x Q31 per-sample multipliers e^(-1/tau). Index parameter >> 24."""
    return [q31(math.exp(-1.0 / release_tau(i))) for i in range(RELEASE_N)]


def all_tables():
    """Name -> list of ints, in the order of the callback table. rFilterCoeffs as 5 lists."""
    return {"k_lp_table": k_lp_table(), "g_sine": g_sine(), "g_sineTable": g_sine_table(),
            "rFilterCoeffs": rfilter_coeffs(), "releaseTable": release_table()}


# ---- models of the SDK accessors (sdk/include/opz_tables.h), for tests ------------------------------------

def _s32(x):
    return (x + 2**31) % 2**32 - 2**31


def sine(tab, phase):
    """opz_sine: TE's lookup, no interpolation."""
    return tab[(phase & 0xffffffff) >> 19]


def sine_lerp(tab, phase):
    """opz_sine_lerp: linear interpolation between g_sine[i] and g_sine[i + 1] with a 15-bit fraction."""
    phase &= 0xffffffff
    i, f = phase >> 19, (phase >> 4) & 0x7fff
    d = tab[i + 1] - tab[i]
    return _s32(tab[i] + ((d * f) >> 15))


def sine16_lerp(tab, phase):
    """opz_host_sine16_lerp: g_sineTable interpolated (the next index wraps), Q31."""
    phase &= 0xffffffff
    a, b = tab[phase >> 24], tab[((phase >> 24) + 1) & 255]
    return _s32((a << 16) + 2 * (b - a) * ((phase >> 9) & 0x7fff))


def rf_lookup(tabs, cutoff, res, os_rows=False):
    """opz_rf_lookup: bilinear interpolation of the five tables. Column cutoff >> 25, fraction
    (cutoff & 0x1FFFFFF) << 6; rows as analoga (res >> 28, fraction (res & 0xFFFFFFF) << 3) or, with os_rows, as
    the OS track filter (min(res >> 27, 7), fraction (min(res, 0x3FFFFFFF) & 0x7FFFFFF) << 4). Truncating Q31
    products, saturating sums."""
    cutoff &= 0x7fffffff
    res &= 0x7fffffff
    col, cf = cutoff >> 25, (cutoff & 0x1FFFFFF) << 6
    if os_rows:
        row, rf = min(res >> 27, 7), (min(res, 0x3FFFFFFF) & 0x7FFFFFF) << 4
    else:
        row, rf = res >> 28, (res & 0xFFFFFFF) << 3
    sat = lambda x: max(-2**31, min(2**31 - 1, x))           # noqa: E731
    mul = lambda a, b: sat((a * b) >> 31)                       # noqa: E731
    out = []
    for t in tabs:
        a, b = t[row * RF_COLS + col], t[row * RF_COLS + col + 1]
        c, d = t[(row + 1) * RF_COLS + col], t[(row + 1) * RF_COLS + col + 1]
        lo = sat(a + mul(sat(b - a), cf))
        hi = sat(c + mul(sat(d - c), cf))
        out.append(sat(lo + mul(sat(hi - lo), rf)))
    return out


def c_source(prefix="bench_"):
    """The tables as C arrays (cycle driver's bench.h): <prefix>k_lp_table, g_sine, g_sineTable, rf[5][585],
    releaseTable, in the device formats."""
    def arr(ctype, name, values, per_line=8):
        rows = [", ".join(str(v) for v in values[i:i + per_line]) for i in range(0, len(values), per_line)]
        return "static const %s %s%s[%d] = {\n    %s\n};\n" % (ctype, prefix, name, len(values),
                                                              ",\n    ".join(rows))
    rf = rfilter_coeffs()
    out = ["/* The OS's shared tables, from tools/opz_tables.py (our formulas, device formats). */",
           arr("int32_t", "k_lp_table", k_lp_table()), arr("int32_t", "g_sine", g_sine()),
           arr("int16_t", "g_sineTable", g_sine_table())]
    out.append("static const int32_t %srf[5][%d] = {\n%s\n};\n" % (prefix, RF_ROWS * RF_COLS, ",\n".join(
        "    { %s }" % ", ".join(str(v) for v in t) for t in rf)))
    out.append(arr("int32_t", "releaseTable", release_table()))
    return "\n".join(out)


if __name__ == "__main__":
    for name, t in all_tables().items():
        if name == "rFilterCoeffs":
            print("%-14s 5 x %d words" % (name, len(t[0])))
        else:
            print("%-14s %5d words, min %d, max %d" % (name, len(t), min(t), max(t)))
