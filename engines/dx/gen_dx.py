#!/usr/bin/env python3
"""Generate dx's tables and its patch bank.

    python3 engines/dx/gen_dx.py            write dx_tables.h, dx_bank.h and dx_bank.syx
    python3 engines/dx/gen_dx.py --check    exit 1 if any of them is out of date (used by tools/tests)

dx_tables.h  lookup tables (sine, levels, velocity, LFO, pitch envelope, algorithms), see the comments in it.
dx_bank.h    the 32 patches of patches.py in the DX7 32-voice packed format (32 x 128 bytes = 4,096 bytes, the
             same size that getPresetData, callback +0x28, reads from engines/fmedit.preset), as a C array.
dx_bank.syx  the same bank as a standard DX7 32-voice bulk dump (F0 43 00 09 20 00, 4,096 bytes, checksum, F7), so
             the patches can be auditioned and edited in any DX7-compatible editor and sent back later (F-04).

Unit conversions follow the published behaviour of the DX7 as modelled by the 6-op FM code of an open-source
eurorack macro oscillator (MIT licence, Copyright 2021 Emilie Gillet; see engines/dx/README.md, Credits); the
fixed-point forms, tables and the patches are ours. Python 3 standard library only.
"""

import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import patches  # noqa: E402

FS, BLOCK = 44100, 64
SINE_BITS = 12                    # 4,096-entry sine table, looked up without interpolation

# ---------------------------------------------------------------------------------------------- algorithms
#
# The 32 DX7 algorithms as routing for block passes. Operators run from 6 down to 1 (every modulator has a higher
# number than the operator it modulates). Each operator reads its phase modulation from a bus (A or B), from its own
# output (feedback) or from nothing, and writes its output into a bus or into the carrier sum C, either replacing what
# is there (=) or adding to it (+). An operator that modulates several others writes one bus that all of them read.
# Algorithms 4 and 6 have a feedback loop over several operators (6-5-4 and 6-5): those operators run together in one
# per-sample loop (LOOP3, LOOP2) into C.
#
# Notation per operator, 6 first: in>out with in in {-, A, B, F} and out in {A=, A+, B=, B+, C=, C+}.

ALGORITHMS = [
    "F>A= A>A= A>A= A>C= ->A= A>C+",   # 1   6-5-4-3, 2-1; feedback 6
    "->A= A>A= A>A= A>C= F>A= A>C+",   # 2   6-5-4-3, 2-1; feedback 2
    "F>A= A>A= A>C= ->A= A>A= A>C+",   # 3   6-5-4, 3-2-1; feedback 6
    "LOOP3 ->A= A>A= A>C+",            # 4   6-5-4 with feedback 4 -> 6, 3-2-1
    "F>A= A>C= ->A= A>C+ ->A= A>C+",   # 5   6-5, 4-3, 2-1; feedback 6
    "LOOP2 ->A= A>C+ ->A= A>C+",       # 6   6-5 with feedback 5 -> 6, 4-3, 2-1
    "F>A= A>A= ->A+ A>C= ->A= A>C+",   # 7   6-5-3, 4-3, 2-1; feedback 6
    "->A= A>A= F>A+ A>C= ->A= A>C+",   # 8   as 7, feedback 4
    "->A= A>A= ->A+ A>C= F>A= A>C+",   # 9   as 7, feedback 2
    "->A= ->A+ A>C= F>A= A>A= A>C+",   # 10  3-2-1, 6-4, 5-4; feedback 3
    "F>A= ->A+ A>C= ->A= A>A= A>C+",   # 11  as 10, feedback 6
    "->A= ->A+ ->A+ A>C= F>A= A>C+",   # 12  2-1, 6-3, 5-3, 4-3; feedback 2
    "F>A= ->A+ ->A+ A>C= ->A= A>C+",   # 13  as 12, feedback 6
    "F>A= ->A+ A>A= A>C= ->A= A>C+",   # 14  2-1, 6-4, 5-4, 4-3; feedback 6
    "->A= ->A+ A>A= A>C= F>A= A>C+",   # 15  as 14, feedback 2
    "F>A= A>B= ->A= A>B+ ->B+ B>C=",   # 16  6-5-1, 4-3-1, 2-1; feedback 6
    "->A= A>B= ->A= A>B+ F>B+ B>C=",   # 17  as 16, feedback 2
    "->A= A>A= A>A= F>A+ ->A+ A>C=",   # 18  6-5-4-1, 3-1, 2-1; feedback 3
    "F>A= A>C= A>C+ ->A= A>A= A>C+",   # 19  3-2-1, 6-5, 6-4; feedback 6
    "->A= ->A+ A>C= F>A= A>C+ A>C+",   # 20  3-1, 3-2, 6-4, 5-4; feedback 3
    "->A= A>C= A>C+ F>A= A>C+ A>C+",   # 21  3-1, 3-2, 6-5, 6-4; feedback 3
    "F>A= A>C= A>C+ A>C+ ->A= A>C+",   # 22  2-1, 6-5, 6-4, 6-3; feedback 6
    "F>A= A>C= A>C+ ->A= A>C+ ->C+",   # 23  1, 3-2, 6-5, 6-4; feedback 6
    "F>A= A>C= A>C+ A>C+ ->C+ ->C+",   # 24  1, 2, 6-5, 6-4, 6-3; feedback 6
    "F>A= A>C= A>C+ ->C+ ->C+ ->C+",   # 25  1, 2, 3, 6-5, 6-4; feedback 6
    "F>A= ->A+ A>C= ->A= A>C+ ->C+",   # 26  1, 3-2, 6-4, 5-4; feedback 6
    "->A= ->A+ A>C= F>A= A>C+ ->C+",   # 27  as 26, feedback 3
    "->C= F>A= A>A= A>C+ ->A= A>C+",   # 28  2-1, 5-4-3, 6; feedback 5
    "F>A= A>C= ->A= A>C+ ->C+ ->C+",   # 29  1, 2, 4-3, 6-5; feedback 6
    "->C= F>A= A>A= A>C+ ->C+ ->C+",   # 30  1, 2, 5-4-3, 6; feedback 5
    "F>A= A>C= ->C+ ->C+ ->C+ ->C+",   # 31  1, 2, 3, 4, 6-5; feedback 6
    "F>C= ->C+ ->C+ ->C+ ->C+ ->C+",   # 32  all carriers; feedback 6
]

IN_CODE = {"-": 0, "A": 1, "B": 2, "F": 3}
OUT_CODE = {"A": 0, "B": 1, "C": 2}


def encode_algorithm(text):
    """-> (six op codes for operators 6..1, loop size, carrier mask over operators 1..6 as bits 0..5).
    Op code: bits 0-1 input, bits 2-3 output bus, bit 4 add. Loop operators get code 0xff."""
    tokens = text.split()
    codes, loop = [], 0
    if tokens[0].startswith("LOOP"):
        loop = int(tokens[0][4:])
        codes = [0xff] * loop
        tokens = tokens[1:]
    for t in tokens:
        i, o = t.split(">")
        codes.append(IN_CODE[i] | OUT_CODE[o[0]] << 2 | (16 if o[1] == "+" else 0))
    assert len(codes) == 6, text
    carriers = 0
    for k, c in enumerate(codes):          # k = 0 is operator 6
        if c != 0xff and (c >> 2 & 3) == 2:
            carriers |= 1 << (5 - k)
    if loop:
        carriers |= 1 << (6 - loop)          # the loop's last operator (4 or 5) is a carrier
    return codes, loop, carriers


# The DX7's algorithm chart as a modulation graph, written independently of the bus programs above:
# "a>b" = operator a modulates b, then the carriers, then the operator with feedback (or a loop "4>6").
TOPOLOGY = [
    "6>5 5>4 4>3 2>1 | 1 3 | 6",      "6>5 5>4 4>3 2>1 | 1 3 | 2",
    "6>5 5>4 3>2 2>1 | 1 4 | 6",      "6>5 5>4 3>2 2>1 | 1 4 | 4>6",
    "6>5 4>3 2>1 | 1 3 5 | 6",        "6>5 4>3 2>1 | 1 3 5 | 5>6",
    "6>5 5>3 4>3 2>1 | 1 3 | 6",      "6>5 5>3 4>3 2>1 | 1 3 | 4",
    "6>5 5>3 4>3 2>1 | 1 3 | 2",      "6>4 5>4 3>2 2>1 | 1 4 | 3",
    "6>4 5>4 3>2 2>1 | 1 4 | 6",      "6>3 5>3 4>3 2>1 | 1 3 | 2",
    "6>3 5>3 4>3 2>1 | 1 3 | 6",      "6>4 5>4 4>3 2>1 | 1 3 | 6",
    "6>4 5>4 4>3 2>1 | 1 3 | 2",      "6>5 5>1 4>3 3>1 2>1 | 1 | 6",
    "6>5 5>1 4>3 3>1 2>1 | 1 | 2",    "6>5 5>4 4>1 3>1 2>1 | 1 | 3",
    "6>5 6>4 3>2 2>1 | 1 4 5 | 6",    "6>4 5>4 3>2 3>1 | 1 2 4 | 3",
    "6>5 6>4 3>2 3>1 | 1 2 4 5 | 3",  "6>5 6>4 6>3 2>1 | 1 3 4 5 | 6",
    "6>5 6>4 3>2 | 1 2 4 5 | 6",      "6>5 6>4 6>3 | 1 2 3 4 5 | 6",
    "6>5 6>4 | 1 2 3 4 5 | 6",        "6>4 5>4 3>2 | 1 2 4 | 6",
    "6>4 5>4 3>2 | 1 2 4 | 3",        "5>4 4>3 2>1 | 1 3 6 | 5",
    "6>5 4>3 | 1 2 3 5 | 6",          "5>4 4>3 | 1 2 3 6 | 5",
    "6>5 | 1 2 3 4 5 | 6",            " | 1 2 3 4 5 6 | 6",
]


def simulate(text):
    """Run a bus program symbolically -> (set of edges a>b, carriers, feedback op or (from, to) loop)."""
    codes, loop, _ = encode_algorithm(text)
    bus = {0: None, 1: None, 2: None}            # None = never written; else the set of operators summed there
    edges, fb = set(), None
    if loop:
        ops = list(range(6, 6 - loop, -1))         # 6, 5(, 4)
        for a, b in zip(ops, ops[1:]):
            edges.add((a, b))
        fb = (ops[-1], 6)
        bus[2] = {ops[-1]}
    for k, c in enumerate(codes):
        o = 6 - k
        if c == 0xff:
            continue
        i, out, add = c & 3, c >> 2 & 3, c >> 4
        if i in (1, 2):
            assert bus[i - 1], "operator %d reads an empty bus" % o
            edges.update((m, o) for m in bus[i - 1])
        if i == 3:
            assert fb is None
            fb = o
        if add:
            assert bus[out] is not None, "operator %d adds to an empty bus" % o
            bus[out] = bus[out] | {o}
        else:
            bus[out] = {o}
    return edges, bus[2] or set(), fb


def check_algorithms():
    """Each bus program produces exactly the DX7 algorithm's modulation graph, carriers and feedback."""
    assert len(ALGORITHMS) == len(TOPOLOGY) == 32
    for n, (text, topo) in enumerate(zip(ALGORITHMS, TOPOLOGY)):
        e, c, f = (x.split() for x in topo.split("|"))
        want_edges = {tuple(int(v) for v in x.split(">")) for x in e}
        want_fb = tuple(int(v) for v in f[0].split(">")) if ">" in f[0] else int(f[0])
        edges, carriers, fb = simulate(text)
        assert edges == want_edges, "algorithm %d: %s != %s" % (n + 1, sorted(edges), sorted(want_edges))
        assert carriers == {int(v) for v in c}, "algorithm %d carriers" % (n + 1)
        assert fb == want_fb, "algorithm %d feedback %r != %r" % (n + 1, fb, want_fb)
        _, _, mask = encode_algorithm(text)
        assert mask == sum(1 << (int(v) - 1) for v in c)


# ---------------------------------------------------------------------------------------------- unit tables

def operator_level(level):
    """A 0-99 output or envelope level -> 0-127 'total level' steps of 0.75 dB (127 = full)."""
    tlc = int(level)
    if level < 20:
        tlc = (tlc * (36 - tlc)) >> 3 if tlc < 15 else 27 + tlc
    else:
        tlc += 28
    return tlc


def lfo_frequency(rate):
    rs = 1 if rate == 0 else (rate * 165) >> 6
    rs *= 11 if rs < 160 else 11 + ((rs - 160) >> 4)
    return rs * 0.005865


def pitch_env_level(level):
    """0-99 -> octaves, 50 = 0, about +-4 octaves at the ends."""
    x = (level - 50) / 32.0
    tail = max(abs(x + 0.02) - 1.0, 0.0)
    return x * (1.0 + tail * tail * 5.3056)


def pitch_env_speed(rate):
    """0-99 -> octaves per second: 0.01 at 0, about 1 at 50, 100 at 99 (an exponential curve, ours)."""
    return 0.01 * 2.0 ** (rate * 13.3 / 99.0)


def lfo_delay_seconds(delay):
    """0-99 -> fade-in time of the LFO after note-on (ours): 0 .. 5 s, quadratic."""
    return 5.0 * (delay / 99.0) ** 2


def q(x, bits):
    return int(round(x * (1 << bits)))


def c_array(ctype, name, values, per_line=16, comment=""):
    rows = [", ".join(str(v) for v in values[i:i + per_line]) for i in range(0, len(values), per_line)]
    head = ("/* %s */\n" % comment) if comment else ""
    return "%sstatic const %s %s[%d] = {\n    %s\n};\n" % (head, ctype, name, len(values), ",\n    ".join(rows))


def tables_header():
    check_algorithms()
    out = ["/* Generated by engines/dx/gen_dx.py: do not edit. */",
           "#ifndef DX_TABLES_H", "#define DX_TABLES_H", "", "#include <stdint.h>", ""]

    n = 1 << SINE_BITS
    sine = [int(round(32767 * math.sin(2 * math.pi * (i + 0.5) / n))) for i in range(n)]
    out.append(c_array("int16_t", "k_sin", sine, comment=(
        "sin(2 pi (i + 0.5) / %d) in Q15: looked up with the top %d bits of the phase, no interpolation (the half-step "
        "offset rounds the phase instead of truncating it)" % (n, SINE_BITS))))

    out.append(c_array("uint8_t", "k_tl", [operator_level(i) for i in range(100)], comment=(
        "output and envelope level 0-99 -> 0-127 steps of 0.75 dB (127 = full level)")))

    vel = [int(round(16 * 16 * (1.0 - (v / 127.0) ** (1.0 / 3.0)))) for v in range(128)]
    out.append(c_array("uint16_t", "k_vel", vel, comment=(
        "velocity -> attenuation per step of velocity sensitivity (0-7), in 1/16 steps of 0.75 dB: "
        "16 steps x (1 - cbrt(v / 127)); 0 at velocity 127")))

    lfo = [int(round(lfo_frequency(r) * BLOCK / FS * 2 ** 32)) for r in range(100)]
    out.append(c_array("uint32_t", "k_lfo_inc", lfo, per_line=8, comment=(
        "LFO speed 0-99 -> phase increment per block, Q32 (0.065 Hz .. 24 Hz)")))

    delay = []
    for d in range(100):
        t = lfo_delay_seconds(d)
        blocks = t * FS / BLOCK
        delay.append(1 << 24 if blocks < 1 else int(round((1 << 24) / blocks)))
    out.append(c_array("uint32_t", "k_lfo_fade", delay, per_line=8, comment=(
        "LFO delay 0-99 -> fade-in step per block, Q24 (instant .. 5 s)")))

    out.append(c_array("int32_t", "k_peg_level", [q(pitch_env_level(l), 24) for l in range(100)], per_line=8,
                       comment="pitch envelope level 0-99 -> octaves, Q24 (50 = 0)"))
    out.append(c_array("uint32_t", "k_peg_rate", [q(pitch_env_speed(r) * BLOCK / FS, 24) for r in range(100)],
                       per_line=8, comment="pitch envelope rate 0-99 -> octaves per block, Q24"))

    pms = [0.0, 0.0264, 0.0418, 0.0662, 0.1043, 0.1652, 0.2617, 0.4146]
    out.append(c_array("int32_t", "k_pms", [q(x, 24) for x in pms], comment=(
        "pitch modulation sensitivity 0-7 -> vibrato depth at LFO PMD 99, octaves Q24")))
    ams = [0, 6, 16, 48]
    out.append(c_array("int16_t", "k_ams", [a * 16 for a in ams], comment=(
        "amplitude modulation sensitivity 0-3 -> tremolo depth at LFO AMD 99, 1/16 steps of 0.75 dB")))

    out.append(c_array("uint32_t", "k_recip", [0] + [int(round(65536 / d)) for d in range(1, 128)], per_line=8,
                       comment="65536 / d for envelope distances d = 1 .. 127 steps"))
    out.append(c_array("int32_t", "k_log2", [q(math.log2(1 + i / 32.0), 16) for i in range(33)], per_line=8,
                       comment="log2(1 + i / 32) in Q16, i = 0 .. 32 (per-note level normalisation)"))

    codes, loops, carriers = [], [], []
    for text in ALGORITHMS:
        c, l, m = encode_algorithm(text)
        codes.append(c)
        loops.append(l)
        carriers.append(m)
    out.append("/* Algorithms: per operator, 6 first: bits 0-1 input (0 none, 1 bus A, 2 bus B, 3 own feedback),")
    out.append(" * bits 2-3 output (0 bus A, 1 bus B, 2 carrier sum), bit 4 add instead of replace; 0xff = part of the")
    out.append(" * algorithm's feedback loop. k_alg_loop: operators in that loop (0, 2 or 3); k_alg_car: carriers, bit")
    out.append(" * k = operator k + 1. See ALGORITHMS in gen_dx.py for the readable form. */")
    out.append("static const uint8_t k_alg[32][6] = {")
    for n, (c, text) in enumerate(zip(codes, ALGORITHMS)):
        out.append("    { %s },  /* %2d: %s */" % (", ".join("0x%02x" % v for v in c), n + 1, text))
    out.append("};")
    out.append(c_array("uint8_t", "k_alg_loop", loops))
    out.append(c_array("uint8_t", "k_alg_car", carriers))
    out.append("#endif")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------------------------- the bank

def bank_bytes():
    bank = patches.bank()
    assert len(bank) == 32, "the bank holds 32 patches"
    data = b"".join(patches.pack(p) for p in bank)
    assert len(data) == 4096
    return bank, data


def bank_header():
    bank, data = bank_bytes()
    out = ["/* Generated by engines/dx/gen_dx.py from patches.py: do not edit. */",
           "#ifndef DX_BANK_H", "#define DX_BANK_H", "", "#include <stdint.h>", "",
           "/* 32 patches in the DX7 32-voice packed format (128 bytes each). Writable: a later version can replace",
           " * it from engines/fmedit.preset through getPresetData (+0x28), which reads up to 4,096 bytes. */",
           "#define DX_BANK_INIT { \\"]
    for n, p in enumerate(bank):
        chunk = data[n * 128:(n + 1) * 128]
        out.append("    /* %2d %-10s */ \\" % (n, p["name"]))
        for i in range(0, 128, 16):
            out.append("    %s, \\" % ", ".join("0x%02x" % b for b in chunk[i:i + 16]))
    out.append("}")
    out.append("")
    out.append("#endif")
    return "\n".join(out) + "\n"


def bank_syx():
    _, data = bank_bytes()
    checksum = (-sum(data)) & 0x7f
    return bytes([0xF0, 0x43, 0x00, 0x09, 0x20, 0x00]) + data + bytes([checksum, 0xF7])


def outputs():
    return {"dx_tables.h": tables_header().encode(), "dx_bank.h": bank_header().encode(), "dx_bank.syx": bank_syx()}


def main():
    check = "--check" in sys.argv[1:]
    stale = []
    for name, data in outputs().items():
        path = os.path.join(HERE, name)
        old = open(path, "rb").read() if os.path.exists(path) else None
        if old != data:
            stale.append(name)
            if not check:
                with open(path, "wb") as f:
                    f.write(data)
    if check:
        if stale:
            print("out of date: %s (run python3 engines/dx/gen_dx.py)" % ", ".join(stale))
            return 1
        print("dx tables and bank up to date")
        return 0
    print("wrote %s" % (", ".join(stale) if stale else "nothing (up to date)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
