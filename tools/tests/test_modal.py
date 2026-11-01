"""Offline tests for the modal engine's generated tables. Run:
    python3 -m unittest discover -s tools/tests
"""

import importlib.util
import os
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MODAL = os.path.join(ROOT, "engines", "modal")
# loaded by path under its own name: table, modal and vowel each have a gen_tables.py
_spec = importlib.util.spec_from_file_location("modal_gen_tables", os.path.join(MODAL, "gen_tables.py"))
gen_tables = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen_tables)


class GeneratedTables(unittest.TestCase):
    def test_up_to_date(self):
        """modal_tables.h matches gen_tables.py (run python3 engines/modal/gen_tables.py after edits)."""
        with open(gen_tables.OUT) as f:
            self.assertEqual(f.read(), gen_tables.generate(), "modal_tables.h is out of date")

    def test_anchors(self):
        """Every anchor has one ratio, weight and decay per mode, mode 0 is the note itself, and the values fit the
        engine's fixed-point tables (ratios Q16, decays as log2 in Q12)."""
        for name, ratios, weights, decays, click in gen_tables.ANCHORS:
            self.assertEqual(len(ratios), gen_tables.NMODES, name)
            self.assertEqual(len(weights), gen_tables.NMODES, name)
            self.assertEqual(len(decays), gen_tables.NMODES, name)
            self.assertEqual(ratios[0], 1, name)
            self.assertTrue(all(0.25 <= r < 256 for r in ratios), name)       # Q16 ratio fits the engine's maths
            self.assertTrue(all(0 < w <= 1 for w in weights), name)
            self.assertTrue(all(1 / 256 < d < 256 for d in decays), name)   # log2 within the Q12 int16 table
            self.assertTrue(0 <= click <= 1, name)

    def test_sine_table_interpolates(self):
        """The packed sine (value and step, odd entries holding the next value) interpolates to within 4 LSB of
        32767 sin(2 pi phase) (-78 dBFS) the way the engine reads it."""
        import math
        a = [int(round(32767 * math.sin(2 * math.pi * i / 256))) for i in range(256)]
        words = []
        for i in range(256):
            d = a[(i + 1) % 256] - a[i]
            words.append((a[i] if i % 2 == 0 else a[(i + 1) % 256], d))
        worst = 0
        for ph in range(0, 1 << 32, 7919 * 4099):
            base, d = words[ph >> 24]
            fr = (ph >> 9) & 0xffff
            fr = fr - 0x10000 if fr & 0x8000 else fr                             # signed low half
            v = base + ((d * fr * 2) >> 16)
            worst = max(worst, abs(v - 32767 * math.sin(2 * math.pi * ph / 2 ** 32)))
        self.assertLess(worst, 4)


if __name__ == "__main__":
    unittest.main()
