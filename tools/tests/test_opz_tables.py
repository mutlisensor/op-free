"""Tests for tools/opz_tables.py and sdk/include/opz_tables.h: the OS's shared DSP tables (engine-abi.md §5).

Run from the repository root:

    python3 -m unittest discover -s tools/tests -v

The format tests always run. The Blackfin test streams the tables and the accessors' outputs through the cycle
driver's callback table in bfin-elf-run (skipped without ~/opt/bfin-elf). The device test compares the formulas
with the tables in a local, decrypted OS image (decrypted/firmware/blackfin/bin/opz.ldr, or $OPZ_OS_IMAGE; never
committed) and is skipped without it.
"""

import cmath
import math
import os
import shutil
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import opz_cycles  # noqa: E402
import opz_tables as T  # noqa: E402

TOOLCHAIN = os.path.expanduser("~/opt/bfin-elf")
OS_IMAGE = os.environ.get("OPZ_OS_IMAGE") or os.path.join(REPO, "decrypted", "firmware", "blackfin", "bin",
                                                          "opz.ldr")
CALLBACKS_1_2_45 = 0x81003850       # the device's ModuleCallbackInterface in OS 1.2.45 (engine-abi.md §5)


class Formats(unittest.TestCase):
    def test_g_sine(self):
        s = T.g_sine()
        self.assertEqual(len(s), 8193)
        self.assertEqual(s[0], 0)
        self.assertEqual(s[8192], s[0])                               # guard point
        self.assertEqual(s[2048], 0x7FFF0000)
        self.assertEqual(s[6144], -0x7FFF0000)
        self.assertEqual(max(s), 0x7FFF0000)
        for i in (1, 1000, 3000, 5000, 8000):
            self.assertEqual(s[i], -s[8192 - i])                       # odd symmetry

    def test_g_sine_table(self):
        s = T.g_sine_table()
        self.assertEqual(len(s), 256)
        self.assertEqual((s[0], s[64], s[128], s[192]), (0, 32767, 0, -32768))
        self.assertTrue(all(-32768 <= v <= 32767 for v in s))

    def test_k_lp_table_is_two_poles_at_minus_3_db(self):
        k = T.k_lp_table()
        self.assertEqual(len(k), 129)
        self.assertTrue(all(0 < a < b < 2**31 for a, b in zip(k, k[1:])))
        for i in (0, 39, 64, 78, 100, 116, 128):
            c = k[i] / 2**31
            z = cmath.exp(-1j * 2 * math.pi * T.klp_hz(i) / T.FS)
            h = c / (1 - (1 - c) * z)
            self.assertAlmostEqual(abs(h) ** 4, 0.5, delta=1e-6)       # |H|^2 of two poles in series
        self.assertAlmostEqual(T.klp_hz(0), 10.0)
        self.assertAlmostEqual(T.klp_hz(128), 20000.0, places=6)

    def test_rfilter_coeffs(self):
        rf = T.rfilter_coeffs()
        self.assertEqual([len(t) for t in rf], [585] * 5)
        at = lambda k, row, col: rf[k][row * 65 + col] / 2**31        # noqa: E731
        for row in range(9):
            for col in (31, 32, 33):                                   # flat
                self.assertEqual([rf[k][row * 65 + col] for k in range(5)], [0x7FFFFFFF, 0, 0, 0, 0])
            for col in list(range(30)) + list(range(35, 65)):
                b0, b1, b2, a1, a2 = (at(k, row, col) for k in range(5))
                b1, a1 = 2 * b1, 2 * a1
                dc = (b0 + b1 + b2) / (1 + a1 + a2)
                nyq = (b0 - b1 + b2) / (1 - a1 + a2)
                self.assertAlmostEqual(dc if col < 30 else nyq, 1.0, delta=1e-4)
                self.assertLess(abs(a2), 1.0)                          # stable
        self.assertEqual([t[7 * 65:8 * 65] for t in rf], [t[8 * 65:9 * 65] for t in rf])   # row 8 = row 7

    def test_release_table(self):
        r = T.release_table()
        self.assertEqual(len(r), 128)
        self.assertTrue(all(0 < a < b < 2**31 for a, b in zip(r, r[1:])))
        self.assertAlmostEqual(T.release_tau(127) / T.FS, 7.19, places=2)   # the longest release, ~7 s


class Accessors(unittest.TestCase):
    def test_sine_lerp_is_close_to_sin(self):
        s = T.g_sine()
        worst = 0
        for n in range(4096):
            ph = (n * 0x9E3779B1) & 0xffffffff
            want = 0x7FFF0000 * math.sin(2 * math.pi * ph / 2**32)
            worst = max(worst, abs(T.sine_lerp(s, ph) - want))
        self.assertLess(worst / 2**31, 2e-7)
        self.assertEqual(T.sine(s, 0x40000000), 0x7FFF0000)

    def test_sine16_lerp(self):
        t = T.g_sine_table()
        worst = max(abs(T.sine16_lerp(t, ph) / 2**31 - math.sin(2 * math.pi * ph / 2**32))
                    for ph in range(0, 2**32, 2**32 // 4099))
        self.assertLess(worst, 2e-4)

    def test_rf_lookup_hits_the_table_at_grid_points(self):
        rf = T.rfilter_coeffs()
        # column 16 (LP 1 kHz), analoga row 4 (Q 1): cutoff 16 << 25, res 4 << 28
        self.assertEqual(T.rf_lookup(rf, 16 << 25, 4 << 28), [t[4 * 65 + 16] for t in rf])
        # OS rule: rows over the lower half of the resonance knob, the top half stays at row 7
        self.assertEqual(T.rf_lookup(rf, 51 << 25, 0x7fffffff, os_rows=True),
                         T.rf_lookup(rf, 51 << 25, 0x40000000, os_rows=True))


class Driver(unittest.TestCase):
    def test_bench_header_carries_the_tables(self):
        h = opz_cycles.bench_header(0x81bd9800, False, [], opz_cycles.knob_cases(False, 24))
        for decl in ("int32_t bench_k_lp_table[129]", "int32_t bench_g_sine[8193]", "int16_t bench_g_sineTable[256]",
                     "int32_t bench_rf[5][585]", "int32_t bench_releaseTable[128]"):
            self.assertIn(decl, h)

    @unittest.skipUnless(opz_cycles.available(TOOLCHAIN), "no bfin-elf toolchain in %s" % TOOLCHAIN)
    def test_driver_supplies_the_tables_and_accessors_match_on_blackfin(self):
        """sim/tests/fixtures/tables_probe.c built for the device, run by the cycle driver: every table word and every
        accessor output matches tools/opz_tables.py (so the driver's callback table has the device formats and the
        accessors give the same bits on the Blackfin as on the host)."""
        with tempfile.TemporaryDirectory() as tmp:
            edir = os.path.join(tmp, "tables_probe")
            os.makedirs(edir)
            shutil.copy(os.path.join(REPO, "sim", "tests", "fixtures", "tables_probe.c"), edir)
            with open(os.path.join(edir, "plug.json"), "w") as f:
                f.write('{ "engine": "tables_probe", "memoryslot": 87, "engine_version": 1, "plug": { "id": 60999, '
                        '"version": 1, "name": "tprobe", "trackmask": 240, "flags": 0, "parameter_names": ["a", "b"] } }')
            cases = [opz_cycles.case("tables", opz_cycles.params(), [60], 5, 110)]
            opz_cycles.measure(edir, TOOLCHAIN, os.path.join(tmp, "out"), cases=cases, quiet=True,
                               dump=os.path.join(tmp, "dump"))
            got = opz_cycles.load_dump(os.path.join(tmp, "dump", "tables_probe"))
        words, want = got["tables"], probe_stream()
        self.assertEqual(list(words[:len(want)]), want)


def probe_stream():
    """What tables_probe writes, from the formulas (the same model as sim/tests/golden.py's tables_check)."""
    def h(n):
        n = (n * 0x9E3779B1) & 0xffffffff
        n ^= n >> 15
        n = (n * 0x85EBCA77) & 0xffffffff
        return n ^ (n >> 13)
    sat = lambda x: max(-2**31, min(2**31 - 1, x))                     # noqa: E731
    mul = lambda a, b: sat((a * b) >> 31)                               # noqa: E731
    s32 = lambda v: (v + 2**31) % 2**32 - 2**31                         # noqa: E731
    sine, rf = T.g_sine(), T.rfilter_coeffs()
    want = T.k_lp_table() + sine + T.g_sine_table() + sum(rf, []) + T.release_table()
    want += [T.sine_lerp(sine, h(n)) for n in range(512)]
    want += [T.sine16_lerp(T.g_sine_table(), h(n)) for n in range(256)]
    for p in range(64):
        want += T.rf_lookup(rf, h(2 * p), h(2 * p + 1)) + T.rf_lookup(rf, h(2 * p), h(2 * p + 1), os_rows=True)
    c = T.rf_lookup(rf, 60 * 0x808080 + 30, 100 * 0x808080 + 50, os_rows=True)
    s1 = s2 = 0
    for n in range(256):
        x = s32(h(n)) >> 3
        y = sat(mul(c[0], x) + s1)
        u = sat(mul(c[1], x) - mul(c[3], y))
        s1, s2 = sat(s2 + sat(2 * u)), sat(mul(c[2], x) - mul(c[4], y))
        want.append(y)
    return want + [31]


def ldr_reader(path):
    """addr -> int32 reader for an ADI LDR image (zero for FILL blocks and unmapped addresses)."""
    with open(path, "rb") as f:
        data = f.read()
    segs, off = [], 0
    while off + 16 <= len(data):
        flags, _, mark, target, count, _ = struct.unpack_from("<HBBIII", data, off)
        if mark != 0xAD:
            raise ValueError("not an LDR block at 0x%x" % off)
        off += 16
        if flags & 0x0100:
            segs.append((None, count, target))
            continue
        if flags & 0x4000 and flags & 0x1000:
            continue
        segs.append((off, count, target))
        off += count

    def rd(addr, n, fmt="<i", size=4):
        out = []
        for i in range(n):
            a, v = addr + size * i, 0
            for o, c, t in segs:
                if t <= a < t + c:
                    v = 0 if o is None else struct.unpack_from(fmt, data, o + a - t)[0]
                    break
            out.append(v)
        return out
    return rd


@unittest.skipUnless(os.path.exists(OS_IMAGE), "no local OS image at %s" % OS_IMAGE)
class Device(unittest.TestCase):
    """Formulas against OS 1.2.45's own tables, read through its callback table. Prints only differences."""

    def test_formulas_match_the_device(self):
        rd = ldr_reader(OS_IMAGE)
        cb = [v & 0xffffffff for v in rd(CALLBACKS_1_2_45, 13)]
        ptrs = [v & 0xffffffff for v in rd(cb[5], 5)]
        checks = [("k_lp_table", rd(cb[2], 129), T.k_lp_table(), 11),
                  ("g_sine", rd(cb[3], 8193), T.g_sine(), 1),
                  ("g_sineTable", rd(cb[4], 256, "<h", 2), T.g_sine_table(), 1),
                  ("releaseTable", rd(cb[6], 128), T.release_table(), 1)]
        checks += [("rFilterCoeffs[%d]" % k, rd(ptrs[k], 585), t, 1) for k, t in enumerate(T.rfilter_coeffs())]
        for name, dev, gen, tol in checks:
            worst = max(abs(a - b) for a, b in zip(dev, gen))
            self.assertLessEqual(worst, tol, "%s: %d LSB" % (name, worst))


if __name__ == "__main__":
    unittest.main()
