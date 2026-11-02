"""Offline tests for the pd engine's generated tables. Run:
    python3 -m unittest discover -s tools/tests
"""

import importlib.util
import math
import os
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PD = os.path.join(ROOT, "engines", "pd")
# loaded by path under its own name: several engines have a gen_tables.py
_spec = importlib.util.spec_from_file_location("pd_gen_tables", os.path.join(PD, "gen_tables.py"))
gen_tables = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen_tables)


class GeneratedTables(unittest.TestCase):
    def test_up_to_date(self):
        """pd_tables.h matches gen_tables.py (run python3 engines/pd/gen_tables.py after edits)."""
        with open(gen_tables.OUT) as f:
            self.assertEqual(f.read(), gen_tables.generate(), "pd_tables.h is out of date")

    def test_every_wave_starts_as_a_sine(self):
        """At the lowest sharpness (and double sine's amount 0) every wave except the resonance waves is a plain
        cosine: no DC, gain 1. (The resonance waves start from the sine by a crossfade in the engine.)"""
        for name, f, param in gen_tables.WAVES[:5]:
            for i in range(64):
                ph = i / 64
                self.assertAlmostEqual(f(ph, param(0)), math.cos(2 * math.pi * ph), places=9, msg=name)

    def test_waves_are_continuous(self):
        """No wave jumps anywhere in its period or at the wrap (the bends sit where the cosine is flat, the
        resonance windows end at zero), at every table point."""
        for name, f, param in gen_tables.WAVES:
            for j in range(gen_tables.NJ):
                p = param(j)
                n = 4096
                ys = [f(i / n, p) for i in range(n)] + [f(0.0, p)]
                step = max(abs(ys[i + 1] - ys[i]) for i in range(n))
                # the steepest part of any wave: a half-cycle in 1 / (2 s) of the period, s <= 128
                self.assertLess(step, 2 * math.pi * 128 / n * 1.01, "%s j %d" % (name, j))

    def test_levels(self):
        """DC removed and gain applied, every wave peaks at most PEAK_MAX (3 dB over the sine) and has at least
        the RMS of a sine at -20 dB."""
        for name, f, param in gen_tables.WAVES:
            for j in range(0, gen_tables.NJ, 4):
                mean, peak, rms = gen_tables.measure(f, param(j))
                g = gen_tables.gain_of(peak, rms)
                self.assertLessEqual(peak * g, gen_tables.PEAK_MAX + 1e-9, name)
                self.assertGreater(20 * math.log10(rms * g / math.sqrt(0.5)), -20, name)

    def test_limits(self):
        """The largest sharpness and the pitch limit of every wave fit the engine's fixed point (log2 s <= 7 in
        Q16; a phase increment below 2^32)."""
        for smax, hz in gen_tables.LIMITS:
            self.assertTrue(1 <= smax <= 128)
            self.assertTrue(0 < hz <= gen_tables.FS / 2)


if __name__ == "__main__":
    unittest.main()
