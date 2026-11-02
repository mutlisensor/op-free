"""Offline tests for the table engine: its generated wave tables, and its additive path giving the same bits on the
host and on the Blackfin (skipped without ~/opt/bfin-elf). Run:
    python3 -m unittest discover -s tools/tests
"""

import importlib.util
import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TABLE = os.path.join(ROOT, "engines", "table")
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(TOOLS, "tests"))
# loaded by path under its own name: table, modal and vowel each have a gen_tables.py
_spec = importlib.util.spec_from_file_location("table_gen_tables", os.path.join(TABLE, "gen_tables.py"))
gen_tables = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen_tables)
import opz_cycles  # noqa: E402
from test_opz_cycles import SDK_CHECK_LD, TOOLCHAIN  # noqa: E402


class GeneratedFiles(unittest.TestCase):
    def test_up_to_date(self):
        """table_waves.h matches gen_tables.py (run python3 engines/table/gen_tables.py after edits)."""
        data, _, _, _ = gen_tables.outputs()
        for name, blob in data.items():
            with open(os.path.join(TABLE, name), "rb") as f:
                self.assertEqual(f.read(), blob, "%s is out of date: python3 engines/table/gen_tables.py" % name)

    def test_tables_fit_15_bits(self):
        """Every table value fits 15 bits, so the engine's differences of two samples fit 16."""
        _, _, waves, _ = gen_tables.outputs()
        peak = max(abs(v) for bank in waves for wave in bank for mip in wave for v in mip)
        self.assertLessEqual(peak, 16383)

    def test_coefficients_fit(self):
        """The additive path's coefficients fit 15 bits (bilinear differences fit 16), and each lane's sum stays
        below 32,767 at the largest velocity gain, so its saturating 16-bit sums never clip."""
        data, _, _, coefs = gen_tables.outputs()
        saw_peak = int(re.search(rb"#define TW_SAW_PEAK (\d+)", data["table_waves.h"]).group(1))
        amp = (0x10000000 // saw_peak) / 32768.0                # table.c's OUT_AMP, the largest velocity gain
        for bank in coefs:
            for wave in bank:
                self.assertLessEqual(max(max(abs(a), abs(b)) for a, b in wave), 16383)
                self.assertLess(sum(abs(a) for a, _ in wave) * amp, 32767)
                self.assertLess(sum(abs(b) for _, b in wave) * amp, 32767)

    def test_every_wave_has_its_fundamental(self):
        """No wave loses the note's pitch (a whole sync ratio would): the fundamental is within 21 dB of the RMS."""
        _, info, _, _ = gen_tables.outputs()
        for bank, w, _, _, fund in info:
            self.assertGreater(fund, -21.0, "%s wave %d" % (bank, w))



@unittest.skipUnless(opz_cycles.available(TOOLCHAIN), "bfin-elf-gcc / bfin-elf-run not installed in %s" % TOOLCHAIN)
class HostMatchesDevice(unittest.TestCase):
    """engines/table/tests/add_check.c: the additive path's lane helpers, sine and kernels give the same bits on the
    host (opzsim) and on the Blackfin."""

    def test_additive_path_bit_identical(self):
        src = os.path.join(TABLE, "tests", "add_check.c")
        inc = os.path.join(ROOT, "sdk", "include")
        with tempfile.TemporaryDirectory() as tmp:
            host = os.path.join(tmp, "add_check")
            subprocess.run(["cc", "-std=c11", "-O2", "-I", inc, src, "-o", host], check=True)
            want = subprocess.run([host], capture_output=True, text=True, check=True).stdout
            ld = os.path.join(tmp, "check.ld")
            with open(ld, "w") as f:
                f.write(SDK_CHECK_LD)
            elf = os.path.join(tmp, "add_check.elf")
            subprocess.run([opz_cycles.tool(TOOLCHAIN, "gcc")] + opz_cycles.opz_build.CFLAGS
                           + ["-I", inc, os.path.join(TOOLS, "cycles", "crt0.S"), src, "-nostdlib", "-nostartfiles",
                              "-T", ld, "-o", elf, "-lgcc"], check=True, capture_output=True)
            got = subprocess.run([opz_cycles.tool(TOOLCHAIN, "run"), "--memory-region", "0x80000000,0x100000", elf],
                                 capture_output=True, text=True, timeout=300).stdout
        self.assertTrue(want.endswith("done\n"), want)
        self.assertEqual(got.splitlines(), want.splitlines())


if __name__ == "__main__":
    unittest.main()
