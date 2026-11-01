"""dx's patch bank: 32 patches of our own, written for this engine (not copied from any Yamaha or third-party bank).

Each patch uses the DX7's parameters, so the bank packs into the DX7 32-voice format (gen_dx.py writes dx_bank.h
and dx_bank.syx from it) and any DX7 editor can open it. Operators are listed 1 to 6. Per operator:

    ratio   frequency ratio (0.5, 1 .. 31, with fine steps of 1 %: 1.41 = coarse 1, fine 41), or hz for fixed mode
    ol      output level 0-99
    r, l    envelope rates and levels R1-R4, L1-L4 (key on: L4 -> L1 -> L2 -> L3, hold; key off: -> L4)
    vel     key velocity sensitivity 0-7          rs   rate scaling 0-7 (higher notes run faster)
    det     detune 0-14 (7 = none)                ams  amplitude modulation sensitivity 0-3
    bp, ld, rd, lc, rc   keyboard level scaling: break point (39 = C3, MIDI 60), left/right depth 0-99,
                         left/right curve (0 -LIN, 1 -EXP, 2 +EXP, 3 +LIN)

Per patch: alg 1-32, fb 0-7, sync (oscillator key sync), lfo (speed, delay, pmd, amd, sync, wave 0 tri 1 saw down
2 saw up 3 square 4 sine 5 S&H), pms 0-7, peg (pitch envelope rates and levels, 50 = no shift), transpose 24 = C3.
"""

DEFAULT_OP = dict(ratio=1.0, hz=None, ol=0, r=(99, 99, 99, 99), l=(99, 99, 99, 0), vel=0, rs=0, det=7, ams=0,
                  bp=39, ld=0, rd=0, lc=0, rc=0)
DEFAULT_LFO = dict(speed=35, delay=0, pmd=0, amd=0, sync=0, wave=4)


def op(**kw):
    o = dict(DEFAULT_OP)
    for k in kw:
        if k not in o:
            raise KeyError(k)
    o.update(kw)
    return o


OFF = op(ol=0)


def patch(name, alg, ops, fb=0, sync=1, lfo=None, pms=0, peg=None, transpose=24):
    assert 1 <= len(name) <= 10 and name.isascii(), name
    assert len(ops) == 6
    l = dict(DEFAULT_LFO)
    l.update(lfo or {})
    return dict(name=name, alg=alg, fb=fb, sync=sync, lfo=l, pms=pms,
                peg=peg or ((99, 99, 99, 99), (50, 50, 50, 50)), transpose=transpose, ops=ops)


def coarse_fine(ratio):
    """A ratio -> (coarse, fine): ratio = coarse x (1 + fine / 100), coarse 0 meaning 0.5."""
    best = None
    for c in range(32):
        base = 0.5 if c == 0 else float(c)
        f = round((ratio / base - 1.0) * 100)
        if 0 <= f <= 99:
            err = abs(base * (1 + f / 100.0) - ratio)
            if best is None or err < best[0] - 1e-9 or (err < best[0] + 1e-9 and f < best[2]):
                best = (err, c, f)                       # the closest; among equals the smallest fine
    assert best and best[0] < 0.006, ratio
    return best[1], best[2]


def fixed_coarse_fine(hz):
    """Fixed mode: f = 10^(coarse mod 4) x 10^(fine / 100) Hz."""
    import math
    x = math.log10(hz)
    c = min(3, int(x))
    f = round((x - c) * 100)
    assert 0 <= f <= 99, hz
    return c, f


def pack(p):
    """One patch -> the DX7 packed 128-byte voice."""
    b = bytearray(128)
    for i, o in enumerate(reversed(p["ops"])):          # operator 6 first
        base = i * 17
        if o["hz"] is not None:
            mode = 1
            coarse, fine = fixed_coarse_fine(o["hz"])
        else:
            mode = 0
            coarse, fine = coarse_fine(o["ratio"])
        vals = list(o["r"]) + list(o["l"])
        b[base:base + 8] = bytes(vals)
        b[base + 8] = o["bp"]
        b[base + 9] = o["ld"]
        b[base + 10] = o["rd"]
        b[base + 11] = (o["rc"] << 2) | o["lc"]
        b[base + 12] = (o["det"] << 3) | o["rs"]
        b[base + 13] = (o["vel"] << 2) | o["ams"]
        b[base + 14] = o["ol"]
        b[base + 15] = (coarse << 1) | mode
        b[base + 16] = fine
    rates, levels = p["peg"]
    b[102:106] = bytes(rates)
    b[106:110] = bytes(levels)
    b[110] = p["alg"] - 1
    b[111] = (p["sync"] << 3) | p["fb"]
    lfo = p["lfo"]
    b[112] = lfo["speed"]
    b[113] = lfo["delay"]
    b[114] = lfo["pmd"]
    b[115] = lfo["amd"]
    b[116] = (p["pms"] << 4) | (lfo["wave"] << 1) | lfo["sync"]
    b[117] = p["transpose"]
    b[118:128] = p["name"].upper().ljust(10).encode("ascii")
    for i, v in enumerate(b):
        assert 0 <= v < 128, (p["name"], i, v)
    for i in range(6):
        for k in range(8):
            assert b[i * 17 + k] <= 99
    return bytes(b)


# Envelope shapes used by several patches (rates, levels)
HOLD = ((99, 99, 99, 80), (99, 99, 99, 0))                 # organ gate
VIB = dict(speed=34, delay=55, pmd=6, wave=4)              # delayed vibrato, used with pms 3


def bank():
    return [
        # ---- keys ----------------------------------------------------------------------------------------
        patch("TINE EP", 5, fb=6, ops=[
            op(ratio=1, ol=99, r=(96, 25, 25, 67), l=(99, 75, 0, 0), vel=2, rs=3),
            op(ratio=1, ol=82, r=(95, 55, 30, 78), l=(99, 80, 0, 0), vel=6, rs=3),
            op(ratio=1, ol=86, r=(95, 29, 20, 50), l=(99, 95, 0, 0), vel=2, rs=3, det=10),
            op(ratio=14, ol=68, r=(98, 60, 40, 60), l=(99, 0, 0, 0), vel=4, rs=3, bp=60, rd=40),
            op(ratio=1, ol=80, r=(95, 29, 20, 50), l=(99, 95, 0, 0), rs=3, det=4),
            op(ratio=1, ol=58, r=(95, 29, 20, 50), l=(99, 95, 0, 0), vel=3, rs=3),
        ]),
        patch("REED EP", 5, fb=6, ops=[
            op(ratio=1, ol=99, r=(95, 35, 25, 70), l=(99, 88, 0, 0), vel=2, rs=2),
            op(ratio=1, ol=88, r=(92, 50, 30, 75), l=(99, 85, 70, 0), vel=7, rs=2),
            op(ratio=1, ol=88, r=(95, 35, 25, 70), l=(99, 88, 0, 0), vel=2, rs=2, det=9),
            op(ratio=3, ol=72, r=(97, 60, 35, 70), l=(99, 60, 0, 0), vel=5, rs=2),
            op(ratio=2, ol=70, r=(97, 55, 30, 70), l=(99, 60, 0, 0), vel=2, rs=2),
            op(ratio=1, ol=70, r=(95, 50, 30, 70), l=(99, 75, 0, 0), vel=4, rs=2),
        ]),
        patch("BELL EP", 5, fb=4, ops=[
            op(ratio=1, ol=99, r=(96, 28, 25, 62), l=(99, 80, 0, 0), vel=2, rs=2),
            op(ratio=1, ol=80, r=(95, 45, 30, 70), l=(99, 78, 0, 0), vel=5, rs=2),
            op(ratio=4, ol=82, r=(98, 45, 30, 60), l=(99, 70, 0, 0), vel=3, rs=3, bp=55, rd=40),
            op(ratio=1, ol=70, r=(98, 50, 30, 60), l=(99, 70, 0, 0), vel=4, rs=3),
            op(ratio=1, ol=86, r=(96, 28, 25, 62), l=(99, 80, 0, 0), rs=2, det=5),
            op(ratio=1, ol=62, r=(95, 40, 30, 60), l=(99, 80, 0, 0), vel=3, rs=2),
        ]),
        patch("CLAV", 3, fb=7, ops=[
            op(ratio=1, ol=99, r=(99, 45, 30, 70), l=(99, 85, 0, 0), vel=2, rs=2),
            op(ratio=1, ol=90, r=(99, 50, 35, 75), l=(99, 90, 85, 0), vel=3, rs=2),
            op(ratio=3, ol=75, r=(99, 60, 40, 75), l=(99, 80, 0, 0), vel=4, rs=2),
            op(ratio=2, ol=88, r=(99, 45, 30, 70), l=(99, 85, 0, 0), vel=2, rs=2),
            op(ratio=1, ol=85, r=(99, 50, 35, 70), l=(99, 85, 75, 0), vel=4, rs=2),
            op(ratio=1, ol=75, r=(99, 50, 40, 70), l=(99, 80, 0, 0), vel=3, rs=2),
        ]),
        patch("HARPSI", 5, fb=7, ops=[
            op(ratio=1, ol=99, r=(99, 40, 28, 60), l=(99, 82, 0, 0), vel=1, rs=4),
            op(ratio=3, ol=86, r=(99, 50, 30, 65), l=(99, 80, 0, 0), vel=3, rs=4),
            op(ratio=2, ol=90, r=(99, 42, 28, 60), l=(99, 80, 0, 0), vel=1, rs=4),
            op(ratio=5, ol=80, r=(99, 55, 30, 65), l=(99, 70, 0, 0), vel=3, rs=4),
            op(ratio=1, ol=88, r=(99, 40, 28, 60), l=(99, 82, 0, 0), vel=1, rs=4, det=4),
            op(ratio=1, ol=80, r=(99, 50, 30, 60), l=(99, 80, 0, 0), vel=3, rs=4),
        ]),
        # ---- basses ----------------------------------------------------------------------------------------
        patch("SOLID BASS", 16, fb=6, ops=[
            op(ratio=1, ol=99, r=(99, 30, 25, 65), l=(99, 92, 0, 0), vel=1, rs=2),
            op(ratio=1, ol=86, r=(99, 60, 40, 70), l=(99, 85, 75, 0), vel=5, rs=2),
            op(ratio=1, ol=72, r=(99, 55, 40, 70), l=(99, 85, 75, 0), vel=3, rs=2),
            op(ratio=3, ol=62, r=(99, 65, 40, 70), l=(99, 70, 0, 0), vel=4, rs=2),
            op(ratio=2, ol=74, r=(99, 60, 40, 70), l=(99, 75, 65, 0), vel=4, rs=2),
            op(ratio=1, ol=62, r=(99, 60, 40, 70), l=(99, 80, 70, 0), vel=2, rs=2),
        ]),
        patch("PICK BASS", 16, fb=6, ops=[
            op(ratio=1, ol=99, r=(99, 35, 25, 60), l=(99, 88, 0, 0), vel=2, rs=2),
            op(ratio=1, ol=84, r=(99, 55, 35, 70), l=(99, 82, 70, 0), vel=5, rs=2),
            op(ratio=2, ol=78, r=(99, 70, 40, 70), l=(99, 60, 0, 0), vel=4, rs=2),
            op(ratio=7, ol=72, r=(99, 80, 40, 70), l=(99, 0, 0, 0), vel=5, rs=2),
            op(ratio=1, ol=72, r=(99, 50, 35, 70), l=(99, 80, 70, 0), vel=3, rs=2),
            op(ratio=1, ol=70, r=(99, 50, 35, 70), l=(99, 80, 70, 0), vel=2, rs=2),
        ]),
        patch("WOW BASS", 5, fb=7, ops=[
            op(ratio=1, ol=99, r=(99, 30, 25, 60), l=(99, 92, 85, 0), vel=1),
            op(ratio=1, ol=86, r=(75, 45, 40, 60), l=(99, 70, 55, 0), vel=5),
            op(ratio=1, ol=90, r=(99, 30, 25, 60), l=(99, 92, 85, 0), vel=1, det=9),
            op(ratio=1, ol=80, r=(70, 40, 40, 60), l=(99, 65, 50, 0), vel=5),
            op(ratio=0.5, ol=75, r=(99, 30, 25, 60), l=(99, 92, 85, 0)),
            op(ratio=1, ol=70, r=(80, 40, 40, 60), l=(99, 70, 55, 0), vel=3),
        ]),
        patch("SLAP BASS", 1, fb=5, ops=[
            op(ratio=1, ol=99, r=(99, 40, 25, 60), l=(99, 85, 0, 0), vel=3, rs=2),
            op(ratio=1, ol=88, r=(99, 65, 40, 70), l=(99, 80, 70, 0), vel=6, rs=2),
            op(ratio=3, ol=84, r=(99, 72, 45, 65), l=(99, 40, 0, 0), vel=4, rs=2),
            op(ratio=5, ol=76, r=(99, 80, 50, 70), l=(99, 30, 0, 0), vel=5, rs=2),
            op(ratio=1, ol=62, r=(99, 80, 40, 70), l=(99, 40, 0, 0), vel=2, rs=2),
            op(ratio=1, ol=60, r=(99, 80, 40, 70), l=(99, 40, 0, 0), rs=2),
        ]),
        patch("SUB BASS", 5, fb=0, ops=[
            op(ratio=1, ol=99, r=(99, 25, 20, 60), l=(99, 95, 90, 0), vel=1),
            op(ratio=1, ol=68, r=(99, 55, 40, 65), l=(99, 85, 75, 0), vel=4),
            op(ratio=2, ol=60, r=(99, 40, 30, 60), l=(99, 85, 75, 0), vel=2),
            OFF, OFF, OFF,
        ]),
        patch("FRETLESS", 5, fb=3, ops=[
            op(ratio=1, ol=99, r=(80, 35, 25, 60), l=(99, 90, 0, 0), vel=2, rs=2),
            op(ratio=1, ol=76, r=(60, 35, 30, 65), l=(99, 80, 50, 0), vel=4, rs=2),
            op(ratio=2, ol=62, r=(75, 40, 30, 60), l=(99, 80, 0, 0), vel=2, rs=2),
            op(ratio=1, ol=58, r=(60, 40, 30, 65), l=(99, 80, 50, 0), vel=3, rs=2),
            OFF, OFF,
        ], lfo=dict(speed=33, delay=60, pmd=5, wave=4), pms=3),
        # ---- brass, winds, leads -------------------------------------------------------------------------------
        patch("BRASS", 22, fb=6, ops=[
            op(ratio=1, ol=99, r=(72, 40, 30, 60), l=(99, 95, 90, 0), vel=1, rs=1),
            op(ratio=1, ol=82, r=(62, 45, 30, 60), l=(99, 90, 84, 0), vel=4, rs=1),
            op(ratio=1, ol=94, r=(72, 40, 30, 60), l=(99, 95, 90, 0), vel=1, rs=1, det=4),
            op(ratio=1, ol=94, r=(72, 40, 30, 60), l=(99, 95, 90, 0), vel=1, rs=1, det=10),
            op(ratio=0.5, ol=80, r=(72, 40, 30, 60), l=(99, 95, 90, 0), vel=1, rs=1),
            op(ratio=1, ol=80, r=(62, 45, 30, 60), l=(99, 90, 84, 0), vel=4, rs=1),
        ], lfo=dict(speed=35, delay=50, pmd=6, wave=4), pms=3,
            peg=((65, 60, 99, 30), (50, 50, 50, 47))),
        patch("SOFT HORNS", 5, fb=3, ops=[
            op(ratio=1, ol=99, r=(60, 40, 30, 55), l=(99, 95, 90, 0), vel=1),
            op(ratio=1, ol=74, r=(52, 40, 30, 55), l=(99, 90, 82, 0), vel=3),
            op(ratio=1, ol=95, r=(60, 40, 30, 55), l=(99, 95, 90, 0), vel=1, det=4),
            op(ratio=1, ol=72, r=(52, 40, 30, 55), l=(99, 90, 82, 0), vel=3),
            op(ratio=1, ol=95, r=(60, 40, 30, 55), l=(99, 95, 90, 0), vel=1, det=10),
            op(ratio=1, ol=70, r=(52, 40, 30, 55), l=(99, 90, 82, 0), vel=3),
        ], lfo=dict(speed=33, delay=60, pmd=5, wave=4), pms=3),
        patch("SYNTH LEAD", 5, fb=7, ops=[
            op(ratio=1, ol=99, r=(99, 40, 30, 60), l=(99, 95, 92, 0), vel=1),
            op(ratio=2, ol=78, r=(99, 45, 30, 60), l=(99, 85, 80, 0), vel=3),
            op(ratio=1, ol=93, r=(99, 40, 30, 60), l=(99, 95, 92, 0), vel=1, det=9),
            op(ratio=1, ol=82, r=(99, 45, 30, 60), l=(99, 88, 84, 0), vel=3),
            op(ratio=2, ol=72, r=(99, 40, 30, 60), l=(99, 95, 92, 0), vel=1),
            op(ratio=1, ol=75, r=(99, 45, 30, 60), l=(99, 88, 84, 0), vel=2),
        ], lfo=dict(speed=38, delay=60, pmd=8, wave=4), pms=4),
        patch("FLUTE", 5, fb=0, ops=[
            op(ratio=1, ol=99, r=(62, 45, 30, 60), l=(99, 95, 92, 0), vel=1),
            op(ratio=1, ol=62, r=(55, 40, 30, 60), l=(99, 85, 80, 0), vel=3),
            op(ratio=2, ol=64, r=(65, 50, 35, 60), l=(99, 80, 72, 0), vel=1),
            op(ratio=1, ol=50, r=(55, 40, 30, 60), l=(99, 85, 80, 0), vel=2),
            op(ratio=1, ol=70, r=(90, 70, 50, 60), l=(99, 50, 0, 0), vel=3),
            op(ratio=3.03, ol=82, r=(95, 72, 50, 60), l=(99, 40, 0, 0), vel=2),
        ], lfo=VIB, pms=3),
        patch("CLARINET", 5, fb=0, ops=[
            op(ratio=1, ol=99, r=(70, 45, 30, 60), l=(99, 95, 90, 0), vel=1),
            op(ratio=2, ol=80, r=(65, 40, 30, 60), l=(99, 90, 84, 0), vel=3),
            op(ratio=3, ol=68, r=(70, 45, 30, 60), l=(99, 90, 85, 0), vel=1),
            op(ratio=2, ol=60, r=(65, 40, 30, 60), l=(99, 90, 84, 0), vel=2),
            OFF, OFF,
        ], lfo=VIB, pms=3),
        patch("OBOE", 3, fb=0, ops=[
            op(ratio=1, ol=99, r=(68, 45, 30, 60), l=(99, 95, 90, 0), vel=1),
            op(ratio=1, ol=83, r=(62, 40, 30, 60), l=(99, 92, 86, 0), vel=3),
            op(ratio=4, ol=60, r=(62, 40, 30, 60), l=(99, 92, 86, 0), vel=2),
            op(ratio=4, ol=75, r=(68, 45, 30, 60), l=(99, 95, 90, 0), vel=1),
            op(ratio=1, ol=70, r=(62, 40, 30, 60), l=(99, 92, 86, 0), vel=2),
            OFF,
        ], lfo=VIB, pms=3),
        # ---- strings, pads, organs -------------------------------------------------------------------------
        patch("STRINGS", 2, fb=7, ops=[
            op(ratio=1, ol=99, r=(52, 35, 30, 48), l=(99, 95, 92, 0), rs=1),
            op(ratio=1, ol=82, r=(48, 30, 30, 50), l=(99, 92, 90, 0), vel=2, rs=1),
            op(ratio=1, ol=96, r=(52, 35, 30, 48), l=(99, 95, 92, 0), rs=1, det=10),
            op(ratio=1, ol=80, r=(48, 30, 30, 50), l=(99, 92, 90, 0), vel=2, rs=1),
            op(ratio=1, ol=70, r=(48, 30, 30, 50), l=(99, 92, 90, 0), rs=1, det=4),
            op(ratio=3, ol=55, r=(48, 30, 30, 50), l=(99, 92, 90, 0), rs=1),
        ], lfo=dict(speed=30, delay=60, pmd=5, wave=4), pms=3),
        patch("WARM PAD", 5, fb=5, ops=[
            op(ratio=1, ol=99, r=(46, 30, 25, 38), l=(99, 95, 92, 0)),
            op(ratio=1, ol=76, r=(40, 25, 25, 38), l=(99, 92, 88, 0), vel=2),
            op(ratio=1, ol=96, r=(46, 30, 25, 38), l=(99, 95, 92, 0), det=3),
            op(ratio=2, ol=66, r=(40, 25, 25, 38), l=(99, 92, 88, 0), vel=2),
            op(ratio=0.5, ol=88, r=(46, 30, 25, 38), l=(99, 95, 92, 0), det=11),
            op(ratio=1, ol=72, r=(40, 25, 25, 38), l=(99, 92, 88, 0), vel=2),
        ], lfo=dict(speed=25, delay=40, pmd=4, wave=0), pms=3),
        patch("GLASS PAD", 5, fb=0, ops=[
            op(ratio=1, ol=99, r=(40, 30, 25, 38), l=(99, 95, 90, 0)),
            op(ratio=3.5, ol=62, r=(45, 28, 25, 38), l=(99, 80, 70, 0), vel=2),
            op(ratio=1, ol=95, r=(40, 30, 25, 38), l=(99, 95, 90, 0), det=10),
            op(ratio=7, ol=58, r=(50, 30, 25, 38), l=(99, 75, 65, 0), vel=2, bp=60, rd=40),
            op(ratio=4, ol=72, r=(35, 30, 25, 38), l=(99, 90, 80, 0), det=5, bp=60, rd=30),
            op(ratio=1, ol=58, r=(40, 30, 25, 38), l=(99, 85, 80, 0)),
        ], lfo=dict(speed=28, delay=50, pmd=3, wave=4), pms=3),
        patch("CHOIR", 5, fb=0, ops=[
            op(ratio=1, ol=99, r=(40, 35, 30, 45), l=(99, 95, 92, 0)),
            op(ratio=1, ol=68, r=(38, 35, 30, 45), l=(99, 90, 86, 0), vel=2),
            op(ratio=3, ol=84, r=(42, 35, 30, 45), l=(99, 95, 90, 0), det=9, bp=60, rd=35),
            op(ratio=1, ol=60, r=(38, 35, 30, 45), l=(99, 90, 86, 0)),
            op(ratio=5, ol=72, r=(42, 35, 30, 45), l=(99, 95, 90, 0), det=5, bp=55, rd=45),
            op(ratio=1, ol=55, r=(38, 35, 30, 45), l=(99, 90, 86, 0)),
        ], lfo=dict(speed=32, delay=45, pmd=7, wave=4), pms=3),
        patch("ORGAN", 32, fb=0, ops=[
            op(ratio=0.5, ol=99, r=HOLD[0], l=HOLD[1]),
            op(ratio=1, ol=99, r=HOLD[0], l=HOLD[1]),
            op(ratio=1.5, ol=95, r=HOLD[0], l=HOLD[1]),
            op(ratio=2, ol=86, r=HOLD[0], l=HOLD[1]),
            op(ratio=3, ol=80, r=HOLD[0], l=HOLD[1]),
            op(ratio=4, ol=78, r=HOLD[0], l=HOLD[1]),
        ], lfo=dict(speed=40, delay=0, pmd=2, wave=4), pms=2),
        patch("PERC ORGAN", 32, fb=0, ops=[
            op(ratio=1, ol=99, r=HOLD[0], l=HOLD[1]),
            op(ratio=2, ol=90, r=HOLD[0], l=HOLD[1]),
            op(ratio=3, ol=92, r=(99, 52, 99, 80), l=(99, 0, 0, 0), vel=2),
            op(ratio=0.5, ol=88, r=HOLD[0], l=HOLD[1]),
            op(ratio=4, ol=80, r=(99, 60, 99, 80), l=(99, 0, 0, 0), vel=2),
            op(ratio=8, ol=60, r=HOLD[0], l=HOLD[1]),
        ]),
        # ---- bells, mallets, plucks, percussion -------------------------------------------------------------
        patch("TUBE BELL", 5, fb=0, ops=[
            op(ratio=1, ol=99, r=(99, 28, 20, 35), l=(99, 80, 0, 0), vel=2, rs=3),
            op(ratio=3.5, ol=78, r=(99, 35, 20, 40), l=(99, 70, 0, 0), vel=4, rs=3),
            op(ratio=1, ol=92, r=(99, 28, 20, 35), l=(99, 80, 0, 0), vel=2, rs=3, det=9),
            op(ratio=3.5, ol=74, r=(99, 35, 20, 40), l=(99, 70, 0, 0), vel=4, rs=3, det=5),
            op(ratio=5, ol=70, r=(99, 45, 25, 40), l=(99, 60, 0, 0), vel=2, rs=3),
            op(ratio=1.41, ol=62, r=(99, 40, 25, 40), l=(99, 60, 0, 0), vel=3, rs=3),
        ]),
        patch("GLOCK", 5, fb=0, ops=[
            op(ratio=1, ol=99, r=(99, 40, 25, 45), l=(99, 75, 0, 0), vel=2, rs=4),
            op(ratio=3.5, ol=70, r=(99, 55, 30, 45), l=(99, 60, 0, 0), vel=4, rs=4),
            op(ratio=2.7, ol=80, r=(99, 55, 30, 45), l=(99, 60, 0, 0), vel=2, rs=4),
            OFF,
            op(ratio=5.4, ol=70, r=(99, 65, 35, 45), l=(99, 50, 0, 0), vel=2, rs=4),
            OFF,
        ]),
        patch("MARIMBA", 5, fb=0, ops=[
            op(ratio=1, ol=99, r=(99, 50, 30, 55), l=(99, 70, 0, 0), vel=2, rs=4),
            op(ratio=1, ol=68, r=(99, 75, 40, 55), l=(99, 40, 0, 0), vel=5, rs=4),
            op(ratio=4, ol=86, r=(99, 58, 40, 55), l=(99, 60, 0, 0), vel=3, rs=4),
            OFF,
            op(ratio=10, ol=72, r=(99, 80, 50, 60), l=(99, 30, 0, 0), vel=4, rs=4),
            OFF,
        ]),
        patch("VIBES", 5, fb=0, ops=[
            op(ratio=1, ol=99, r=(99, 32, 25, 40), l=(99, 80, 0, 0), vel=2, rs=3, ams=2),
            op(ratio=4, ol=62, r=(99, 50, 30, 45), l=(99, 60, 0, 0), vel=4, rs=3),
            op(ratio=4, ol=72, r=(99, 45, 30, 45), l=(99, 60, 0, 0), vel=2, rs=3, ams=2),
            op(ratio=1, ol=50, r=(99, 60, 30, 45), l=(99, 50, 0, 0), vel=4, rs=3),
            OFF, OFF,
        ], lfo=dict(speed=33, delay=0, amd=40, wave=4)),
        patch("KALIMBA", 5, fb=0, ops=[
            op(ratio=1, ol=99, r=(99, 48, 30, 50), l=(99, 75, 0, 0), vel=2, rs=3),
            op(ratio=1, ol=72, r=(99, 60, 40, 50), l=(99, 60, 0, 0), vel=5, rs=3),
            op(ratio=6, ol=84, r=(99, 68, 40, 50), l=(99, 45, 0, 0), vel=3, rs=3),
            op(ratio=1, ol=60, r=(99, 70, 40, 50), l=(99, 40, 0, 0), vel=3, rs=3),
            OFF, OFF,
        ]),
        patch("HARP", 5, fb=0, ops=[
            op(ratio=1, ol=99, r=(99, 38, 25, 40), l=(99, 85, 0, 0), vel=2, rs=3),
            op(ratio=1, ol=68, r=(99, 55, 30, 50), l=(99, 65, 0, 0), vel=4, rs=3),
            op(ratio=2, ol=80, r=(99, 45, 25, 45), l=(99, 75, 0, 0), vel=2, rs=3),
            op(ratio=3, ol=55, r=(99, 70, 35, 50), l=(99, 50, 0, 0), vel=4, rs=3),
            op(ratio=1, ol=88, r=(99, 38, 25, 40), l=(99, 85, 0, 0), vel=2, rs=3, det=9),
            op(ratio=1, ol=50, r=(99, 55, 30, 50), l=(99, 60, 0, 0), vel=3, rs=3),
        ]),
        patch("KOTO", 5, fb=6, ops=[
            op(ratio=1, ol=99, r=(99, 45, 30, 45), l=(99, 75, 0, 0), vel=2, rs=3),
            op(ratio=3, ol=76, r=(99, 60, 35, 50), l=(99, 55, 0, 0), vel=5, rs=3),
            op(ratio=1, ol=85, r=(99, 50, 30, 45), l=(99, 70, 0, 0), vel=2, rs=3, det=10),
            op(ratio=5, ol=60, r=(99, 70, 40, 50), l=(99, 40, 0, 0), vel=5, rs=3),
            op(ratio=2, ol=75, r=(99, 55, 30, 45), l=(99, 65, 0, 0), vel=2, rs=3),
            op(ratio=1, ol=66, r=(99, 60, 35, 50), l=(99, 50, 0, 0), vel=3, rs=3),
        ], peg=((60, 60, 99, 0), (50, 50, 50, 52))),
        patch("STEEL DRUM", 5, fb=0, ops=[
            op(ratio=1, ol=99, r=(99, 45, 30, 50), l=(99, 80, 0, 0), vel=2, rs=3),
            op(ratio=3.5, ol=66, r=(99, 55, 35, 50), l=(99, 60, 0, 0), vel=4, rs=3),
            op(ratio=2, ol=85, r=(99, 50, 30, 50), l=(99, 70, 0, 0), vel=2, rs=3),
            op(ratio=1, ol=60, r=(99, 60, 35, 50), l=(99, 50, 0, 0), vel=4, rs=3),
            op(ratio=3, ol=72, r=(99, 60, 35, 50), l=(99, 55, 0, 0), vel=2, rs=3),
            op(ratio=0.5, ol=55, r=(99, 70, 40, 50), l=(99, 40, 0, 0), vel=3, rs=3),
        ]),
        patch("FM TOM", 5, fb=7, ops=[
            op(ratio=1, ol=99, r=(99, 50, 35, 55), l=(99, 70, 0, 0), vel=2, rs=2),
            op(ratio=1.41, ol=78, r=(99, 70, 40, 55), l=(99, 50, 0, 0), vel=4, rs=2),
            op(ratio=1.6, ol=86, r=(99, 60, 40, 55), l=(99, 55, 0, 0), vel=2, rs=2),
            op(ratio=1, ol=66, r=(99, 70, 40, 55), l=(99, 50, 0, 0), vel=3, rs=2),
            op(ratio=1, ol=84, r=(99, 72, 50, 60), l=(99, 0, 0, 0), vel=3, rs=2),
            op(ratio=9, ol=99, r=(99, 72, 50, 60), l=(99, 0, 0, 0), rs=2),
        ], peg=((70, 99, 99, 0), (50, 50, 50, 62))),
    ]
