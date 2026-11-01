"""Offline tests for the dx engine's generated tables, patch bank and presets. Run:
    python3 -m unittest discover -s tools/tests
"""

import json
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DX = os.path.join(ROOT, "engines", "dx")
sys.path.insert(0, DX)
import gen_dx  # noqa: E402
import patches  # noqa: E402


class GeneratedFiles(unittest.TestCase):
    def test_up_to_date(self):
        """dx_tables.h, dx_bank.h and dx_bank.syx match gen_dx.py and patches.py (run gen_dx.py after edits)."""
        for name, data in gen_dx.outputs().items():
            with open(os.path.join(DX, name), "rb") as f:
                self.assertEqual(f.read(), data, "%s is out of date: python3 engines/dx/gen_dx.py" % name)


class Algorithms(unittest.TestCase):
    def test_bus_programs_match_the_dx7_chart(self):
        gen_dx.check_algorithms()          # raises on any difference

    def test_a_wrong_program_is_caught(self):
        saved = list(gen_dx.ALGORITHMS)
        try:
            gen_dx.ALGORITHMS[0] = "F>A= A>A= A>A= A>C= ->A= A>C="        # op 1 replaces op 3's output
            with self.assertRaises(AssertionError):
                gen_dx.check_algorithms()
        finally:
            gen_dx.ALGORITHMS[:] = saved


class Bank(unittest.TestCase):
    def test_bank(self):
        bank, data = gen_dx.bank_bytes()
        self.assertEqual(len(bank), 32)
        self.assertEqual(len(data), 4096)                     # what getPresetData (+0x28) reads at most
        names = [p["name"] for p in bank]
        self.assertEqual(len(set(names)), 32)
        for n, p in enumerate(bank):
            voice = data[n * 128:(n + 1) * 128]
            self.assertEqual(voice[118:128].decode("ascii").rstrip(), p["name"].upper())
            self.assertEqual(voice[110], p["alg"] - 1)
            carriers = gen_dx.encode_algorithm(gen_dx.ALGORITHMS[p["alg"] - 1])[2]
            self.assertTrue(any(p["ops"][k]["ol"] > 0 for k in range(6) if carriers >> k & 1),
                            "%s: no audible carrier" % p["name"])

    def test_syx(self):
        syx = gen_dx.bank_syx()
        self.assertEqual(syx[:6], bytes([0xF0, 0x43, 0x00, 0x09, 0x20, 0x00]))
        self.assertEqual(syx[-1], 0xF7)
        self.assertEqual((sum(syx[6:-2]) + syx[-2]) & 0x7f, 0)
        self.assertTrue(all(b < 128 for b in syx[1:-1]))

    def test_ratio_packing(self):
        self.assertEqual(patches.coarse_fine(0.5), (0, 0))
        self.assertEqual(patches.coarse_fine(1.41), (1, 41))
        self.assertEqual(patches.coarse_fine(3.5), (2, 75))     # 2 x 1.75, exact
        self.assertEqual(patches.coarse_fine(14), (14, 0))


class Presets(unittest.TestCase):
    def test_presets_point_at_patches(self):
        with open(os.path.join(DX, "presets.json")) as f:
            presets = json.load(f)
        bank = patches.bank()
        per_slot = {}
        for p in presets:
            per_slot[p["slot"]] = per_slot.get(p["slot"], 0) + 1
            self.assertLessEqual(len(p["name"]), 11)
            p1 = p["values"][0]
            self.assertEqual(p1 % 8, 4, "%s: P1 in the middle of a patch step" % p["name"])
            self.assertLess(p1 // 8, len(bank))
            self.assertEqual(p["values"][8:10], [128, 0])     # the OS track filter stays neutral
        self.assertEqual(sorted(per_slot), [0, 1, 2, 3])
        self.assertTrue(all(4 <= n <= 5 for n in per_slot.values()))


if __name__ == "__main__":
    unittest.main()
