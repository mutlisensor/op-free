"""Offline tests for the mallet engine's generated tables. Run:
    python3 -m unittest discover -s tools/tests
"""

import importlib.util
import math
import os
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MALLET = os.path.join(ROOT, "engines", "mallet")
# loaded by path under its own name: several engines have a gen_tables.py
_spec = importlib.util.spec_from_file_location("mallet_gen_tables", os.path.join(MALLET, "gen_tables.py"))
gen_tables = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen_tables)


class GeneratedTables(unittest.TestCase):
    def test_up_to_date(self):
        """mallet_tables.h matches gen_tables.py (run python3 engines/mallet/gen_tables.py after edits)."""
        with open(gen_tables.OUT) as f:
            self.assertEqual(f.read(), gen_tables.generate(), "mallet_tables.h is out of date")

    def test_materials(self):
        """Every material has one ratio, weight and decay per mode, mode 0 is the note itself, and the values fit the
        engine's fixed-point maths (ratios Q16 below 2^22 for the mallet's f / fc product, decays as log2 in Q12, the
        level trim within the voice's headroom)."""
        self.assertEqual(len(gen_tables.MATERIALS), 9)
        for name, ratios, weights, decays, t60, h, hard, click, trim in gen_tables.MATERIALS:
            self.assertEqual(len(ratios), gen_tables.NMODES, name)
            self.assertEqual(len(weights), gen_tables.NMODES, name)
            self.assertEqual(len(decays), gen_tables.NMODES, name)
            self.assertEqual(ratios[0], 1, name)
            self.assertTrue(all(0.25 <= r < 64 for r in ratios), name)
            self.assertTrue(all(0 < w <= 1 for w in weights), name)
            self.assertTrue(all(1 / 256 < d < 8 for d in decays), name)
            self.assertTrue(0 < t60 <= 16 and h > 0 and 0 <= click <= 1, name)
            self.assertLessEqual(10 ** (trim / 20), 1.5, name)              # STRIKE_SUM x 1.5 + click < 0.6 FS

    def test_note_increments(self):
        """note n's increment, k_note_inc[n % 12] >> (10 - n / 12) with n / 12 as (n x 2731) >> 15, is within 1e-6 of
        440 x 2^((n - 69) / 12) / 44100 x 2^32 for every MIDI note (the engine's tuning)."""
        top = [gen_tables.note_inc(n) for n in range(120, 132)]
        self.assertLess(max(top), 1 << 32)
        for n in range(128):
            octave = (n * 2731) >> 15
            self.assertEqual(octave, n // 12)
            inc = top[n - 12 * octave] >> (10 - octave)
            want = 440.0 * 2 ** ((n - 69) / 12) / 44100 * 2 ** 32
            self.assertLess(abs(inc - want), want * 1e-6 + 1, n)
        self.assertLess(math.fabs(gen_tables.note_inc(69) / 2 ** 32 * 44100 - 440), 1e-4)


if __name__ == "__main__":
    unittest.main()
