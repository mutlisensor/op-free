"""Tests for opz_params.py, the model of the OS's parameter path (research/param-paths.md).

Run from the repository root:

    python3 -m unittest discover -s tools/tests -v

The model tests always run. The simulator test plays two streams on opz_test_param in bfin-elf-run and is skipped
when ~/opt/bfin-elf has no bfin-elf-gcc or bfin-elf-run (research/toolchain-setup.md), or when engines/ has no
opz_test_param (the public copy has only released engines).
"""

import os
import random
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import opz_cycles  # noqa: E402
import opz_params as P  # noqa: E402

TOOLCHAIN = os.path.expanduser("~/opt/bfin-elf")
TEST_ENGINE = os.path.join(REPO, "engines", "opz_test_param")


class Model(unittest.TestCase):
    def test_bytes_become_words_in_range(self):
        self.assertEqual(P.word(0), 0)
        self.assertEqual(P.word(255), P.Q31_MAX)
        self.assertEqual(P.word(128), 0x40404040)
        self.assertTrue(all(0 <= P.word(b) <= P.Q31_MAX for b in range(256)))

    def test_smoother_stays_between_start_and_target(self):
        rnd = random.Random(5)
        pairs = [(0, 255), (255, 0), (1, 254), (128, 129)] + [(rnd.randrange(256), rnd.randrange(256))
                                                              for _ in range(150)]
        for a, b in pairs:
            w, t = P.word(a), P.word(b)
            lo, hi = min(w, t), max(w, t) + 1
            for _ in range(1200):
                w = P.smooth_step(w, t)
                self.assertTrue(lo <= w <= hi and w <= P.Q31_MAX, (a, b, hex(w)))
            self.assertLess(abs(w - t), 4100, (a, b))

    def test_lfo_output_never_leaves_the_range(self):
        rnd = random.Random(7)
        for _ in range(300):
            b = [rnd.randrange(256) for _ in range(P.NUM_PARAMS)]
            lfo = P.Lfo(seed=rnd.randrange(100))
            lfo.phase = rnd.randrange(1 << 32)
            lfo.tilt = rnd.randrange(-0x8000, 0x8000)
            out = lfo.render([P.word(x) for x in b])
            self.assertEqual(len(out), P.NUM_PARAMS + 1)
            self.assertTrue(all(0 <= w <= P.Q31_MAX for w in out), b)

    def test_lfo_depth_dead_zone_and_extremes(self):
        b = list(P.DEFAULT_BYTES)
        b[P.P_LFO_SPEED], b[P.P_LFO_TARGET] = 255, P.lfo_target_byte(0)
        b[P.P_LFO_SHAPE] = P.lfo_shape_byte(2)                            # triangle
        for depth in range(124, 131):                                     # around 128: no modulation
            b[P.P_LFO_DEPTH] = depth
            lfo = P.Lfo()
            self.assertEqual({lfo.render([P.word(x) for x in b])[0] for _ in range(16)}, {P.word(128)})
        b[P.P_P1], b[P.P_LFO_DEPTH] = 0, 255
        lfo = P.Lfo()
        vals = [lfo.render([P.word(x) for x in b])[0] for _ in range(16)]
        self.assertEqual(min(vals), 0)
        self.assertGreater(max(vals), 0.95 * P.Q31_MAX)

    def test_fastest_lfo_is_eight_blocks(self):
        b = list(P.DEFAULT_BYTES)
        b[P.P_P1], b[P.P_LFO_DEPTH], b[P.P_LFO_SPEED] = 0, 255, 255
        b[P.P_LFO_TARGET], b[P.P_LFO_SHAPE] = P.lfo_target_byte(0), P.lfo_shape_byte(3)    # ramp
        lfo = P.Lfo()
        vals = [lfo.render([P.word(x) for x in b])[0] for _ in range(33)]
        resets = [i for i in range(1, len(vals)) if vals[i] < vals[i - 1]]
        self.assertEqual([resets[i + 1] - resets[i] for i in range(len(resets) - 1)], [8] * (len(resets) - 1))

    def test_pitch_extremes_and_portamento(self):
        self.assertEqual(P.note_pitch(0, drum=True), 1024)
        hi = P.detune(P.note_pitch(127, 1199), 391)
        self.assertGreater(hi, 0x80000000)
        self.assertLess(hi, 0xC0000000)
        self.assertEqual(P.portamento_rate(P.word(255)), 32)
        self.assertGreater(P.portamento_rate(P.word(1)), 1600)
        path = P.glide(P.note_pitch(0), P.note_pitch(127), P.word(1), 40)
        self.assertEqual(path[-1], P.note_pitch(127))
        self.assertEqual(path, sorted(path))


class Streams(unittest.TestCase):
    def test_rows_and_ranges(self):
        for is_fx, drum in ((False, False), (False, True), (True, False)):
            cases = P.stream_cases(is_fx, drum, quick=True)
            self.assertGreaterEqual(len(cases), 9)
            for c in cases:
                self.assertEqual(c["group"], "param streams")
                self.assertTrue(c["stream"])
                for r in c["stream"]:
                    self.assertEqual(len(r), P.STREAM_WORDS)
                    self.assertTrue(all(0 <= w <= P.Q31_MAX for w in r[:P.NUM_PARAMS]), c["name"])
                if not is_fx:
                    self.assertGreaterEqual(c["blocks"], len(c["stream"]))
        bends = {r[P.NUM_PARAMS] for c in P.stream_cases(True, False) if "bend" in c["name"] for r in c["stream"]}
        self.assertIn(0x80000000, bends)
        pitches = {r[P.NUM_PARAMS + 1] for c in P.stream_cases(False, False) for r in c["stream"]}
        self.assertTrue(any(p > 0x80000000 for p in pitches))

    def test_bench_header_emits_the_streams(self):
        cases = P.stream_cases(False, False, quick=True)[:2] + [opz_cycles.case("plain", opz_cycles.params(),
                                                                                [60], 4, 4)]
        h = opz_cycles.bench_header(0x81bf2800, False, [], cases)
        self.assertIn("static const uint32_t bench_stream_0[%d]" % (P.STREAM_WORDS * len(cases[0]["stream"])), h)
        self.assertIn("bench_stream_1 },", h)
        self.assertIn(", 0, 0 },", h)                      # the plain case: no stream


@unittest.skipUnless(opz_cycles.available(TOOLCHAIN), "bfin-elf toolchain not installed")
@unittest.skipUnless(os.path.isdir(TEST_ENGINE), "the test engine opz_test_param is not in engines/")
class Simulator(unittest.TestCase):
    def test_streams_run_on_the_test_engine(self):
        cases = [c for c in P.stream_cases(False, False, quick=True)
                 if "extremes" in c["name"] or "0x80000000" in c["name"]]
        with tempfile.TemporaryDirectory() as tmp:
            rep = opz_cycles.measure(TEST_ENGINE, TOOLCHAIN, tmp, cases=cases,
                                     quiet=True, torn=True)
        self.assertEqual(len(rep["cases"]), len(cases))
        self.assertEqual(rep["checks"]["findings"], [])
        self.assertTrue(all(c["blocks"] > 0 for c in rep["cases"]))


if __name__ == "__main__":
    unittest.main()
