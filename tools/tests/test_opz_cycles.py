"""Tests for opz_cycles.py, the Blackfin cycle benchmark.

Run from the repository root:

    python3 -m unittest discover -s tools/tests -v

The generator and parser tests always run. The simulator tests build and run the two test engines in
bfin-elf-run and are skipped when ~/opt/bfin-elf has no bfin-elf-gcc or bfin-elf-run
(research/toolchain-setup.md), or when the test engines are not in engines/ (the public copy has only released
engines).
"""

import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import opz_cycles  # noqa: E402

TOOLCHAIN = os.path.expanduser("~/opt/bfin-elf")
BFP_RUN = opz_cycles.BFP_RUN   # our simulator with the Blackfin+ multiply
TEST_ENGINES = ("opz_test_sine", "opz_test_passthrough")


class Generators(unittest.TestCase):
    def test_pitch_word(self):
        self.assertEqual(opz_cycles.note_pitch(69), round(440 / 44100 * 2 ** 32))
        self.assertAlmostEqual(opz_cycles.note_pitch(81) / opz_cycles.note_pitch(69), 2.0, places=6)

    def test_drum_tracks_get_the_unity_pitch(self):
        c = opz_cycles.case("k", opz_cycles.params(), [53, 54], 0, 4)
        self.assertEqual(c["pitch"], [opz_cycles.DRUM_PITCH] * 2)
        c = opz_cycles.case("s", opz_cycles.params(), [60], 4, 4)
        self.assertEqual(c["pitch"], [opz_cycles.note_pitch(60)])

    def test_adsr_tables(self):
        attack, decay = opz_cycles.adsr_tables()
        self.assertEqual(len(attack), 128)
        self.assertTrue(all(a > b for a, b in zip(attack[1:], attack[2:])))      # slower attack, smaller step
        self.assertTrue(all(0 < d <= 0x7fffffff for d in decay))
        self.assertTrue(all(a <= b for a, b in zip(decay, decay[1:])))          # longer time, closer to 1

    def test_header_has_every_case(self):
        cases = opz_cycles.cases_for("macro", False, quick=True)
        h = opz_cycles.bench_header(0x81bd9800, False, [(0x81bda000, 64)], cases)
        self.assertIn("#define ENGINE_BASE 0x81bd9800u", h)
        self.assertIn("static const BenchCase bench_cases[%d]" % len(cases), h)
        self.assertEqual(len(re.findall(r"^    \{ \{", h, re.M)), len(cases))

    def test_knob_cases_move_both_knobs(self):
        for is_fx in (False, True):
            ks = opz_cycles.knob_cases(is_fx, 24)
            self.assertEqual([c["knobs"] for c in ks], [8, 40])
            self.assertTrue(all(c["blocks"] >= 3 * c["knobs"] for c in ks))
        names = [c["name"] for c in opz_cycles.cases_for("unknown", False, quick=True)]
        self.assertIn("knobs moving every block, period 8", names)

    def test_unknown_drum_engines_play_on_a_drum_track(self):
        cases = opz_cycles.cases_for("unknown", False, quick=True, drum=True)
        self.assertTrue(cases)
        self.assertTrue(all(c["track"] == 0 and c["pitch"][0] == opz_cycles.DRUM_PITCH for c in cases))
        self.assertTrue(all(53 <= n <= 76 for c in cases for n in c["notes"]))
        self.assertEqual(opz_cycles.plug_kind({"plug": {"trackmask": 15}}), (False, True))
        self.assertEqual(opz_cycles.plug_kind({"plug": {"trackmask": 240}}), (False, False))
        self.assertEqual(opz_cycles.plug_kind({"plug": {"trackmask": 768}}), (True, False))
        h = opz_cycles.bench_header(0x81bd9800, False, [], opz_cycles.knob_cases(False, 24))
        self.assertIn(", 8, 0, 0, 0, 0, { 0u, 0u }, 0, 0 },", h)

    def test_known_engines_have_cases(self):
        self.assertEqual(len(opz_cycles.cases_for("kit", False)), 2 * 24 + 3)
        for name in ("macro", "fm2", "fold", "dx", "pluck", "super", "table", "modal", "vowel", "swirl", "clouds",
                     "shimmer", "sync", "pd", "ensemble", "swarm", "chip", "fmperc", "mallet", "hats", "8bit", "snap",
                     "opz_test_sine", "opz_test_passthrough", "unknown"):
            for c in opz_cycles.cases_for(name, name in ("swirl", "clouds", "shimmer", "opz_test_passthrough")):
                self.assertLessEqual(len(c["notes"]), opz_cycles.MAX_VOICES)
                self.assertEqual(len(c["params"]), 18)

    def test_parse_output(self):
        text = ("M ok=1 mem=0 cycles=23 overhead=1\n"
                "R case=0 voices=1 blocks=2 last=1 render_n=2 render_min=10 render_max=12 render_sum=22 "
                "steady_n=2 steady_min=10 steady_max=12 steady_sum=22 block_n=2 block_min=10 block_max=12 "
                "block_sum=22 on_n=1 on_min=5 on_max=5 on_sum=5 off_n=0 off_min=0 off_max=0 off_sum=0 cb=4\n"
                "D\n")
        p = opz_cycles.parse_output(text)
        self.assertTrue(p["done"])
        self.assertEqual(p["module"]["cycles"], 23)
        s = opz_cycles.summarise(opz_cycles.case("x", opz_cycles.params(), [60], 4, 2), p["results"][0], False)
        self.assertEqual(s["per_call_mean"], 11.0)
        self.assertEqual(s["callback_per_call"], 2.0)
        self.assertEqual(s["verdict"], "ok")

    def test_budget_subtracts_the_os_share(self):
        # research/cpu-budget.md: (265,000 - OS) / 19 voices / 64 frames, and the 290,000 line for one block
        self.assertEqual(opz_cycles.OS_WINDOW_CYCLES, 1330 + 8 * 6600 + 770 + 19 * 350)
        self.assertAlmostEqual(opz_cycles.BUDGET_PER_SAMPLE, (265000 - 61550) / 19 / 64, places=6)
        self.assertAlmostEqual(opz_cycles.BUDGET_PER_SAMPLE_HARD, (290000 - 61550) / 19 / 64, places=6)
        self.assertLess(opz_cycles.TARGET_PER_SAMPLE, opz_cycles.BUDGET_PER_SAMPLE)
        self.assertLess(opz_cycles.BUDGET_PER_SAMPLE, opz_cycles.BUDGET_PER_SAMPLE_HARD)

    def test_verdict_uses_both_overload_lines(self):
        c = opz_cycles.case("x", opz_cycles.params(), [60], 4, 2)

        def verdict(mean, worst, is_fx=False):
            r = {"voices": 1, "blocks": 2, "render_n": 2, "render_sum": 2 * mean * 64, "render_max": worst * 64,
                 "render_min": 0, "block_max": 0, "on_max": 0, "off_max": 0, "cb": 0}
            return opz_cycles.summarise(c, r, is_fx)["verdict"]
        self.assertEqual(verdict(140, 150), "ok")
        self.assertEqual(verdict(160, 185), "tight")
        self.assertEqual(verdict(170, 172), "over budget")   # steady state over the 265,000 line
        self.assertEqual(verdict(100, 195), "over budget")   # one block over the 290,000 line
        self.assertEqual(verdict(500, 590, is_fx=True), "ok")
        self.assertEqual(verdict(500, 610, is_fx=True), "over FX target")


class BlackfinPlus(unittest.TestCase):
    def test_finds_the_mult32_words(self):
        # R0 = R0 * R1 (T) and R5 = R2 * R7 (T) between NOPs; a classic 16-bit multiply word is not one
        img = b"\x00\x00" + b"\x31\xc2\x01\x20" + b"\x00\x00" + b"\x31\xc2\x57\x21" + b"\x31\xc2\x01\x40"
        self.assertEqual(opz_cycles.blackfin_plus_multiplies(img), [2, 8])


class Layout(unittest.TestCase):
    """--layout: a whole set of tracks against the shared pool (research/cpu-headroom.md §2.1)."""

    def test_parse(self):
        L = opz_cycles.parse_layout("bass=table, chord=modal*2 lead=super:mono kick=kit")
        self.assertEqual(L, {4: ("table", 2), 7: ("modal", 2), 5: ("super", 1), 0: ("kit", 2)})
        for bad in ("drums=kit", "bass", "bass=table*0", "bass=table*9", ""):
            with self.assertRaises(ValueError):
                opz_cycles.parse_layout(bad)

    def test_full_layout_os_share_is_the_budget_constant(self):
        L = {t: ("e", v) for t, v in enumerate(opz_cycles.TRACK_VOICES)}
        b = opz_cycles.layout_budget(L, {"e": (0, 0)}, {})
        self.assertEqual(b["voices"], opz_cycles.MAX_VOICES_DEVICE)
        self.assertEqual(b["os"], opz_cycles.OS_WINDOW_CYCLES)

    def test_flag_0x04_skips_the_track_filter(self):
        L = {4: ("e", 2), 7: ("e", 4)}
        with_os = opz_cycles.layout_budget(L, {"e": (0, 0)}, {"e": 0})["os"]
        own = opz_cycles.layout_budget(L, {"e": (0, 0)}, {"e": 0x04})["os"]
        self.assertEqual(with_os - own, 2 * opz_cycles.OS_TRACK_FILTER + opz_cycles.OS_BASS_EXTRA)

    def test_verdicts_follow_both_lines(self):
        L = {7: ("e", 4)}
        os_share = opz_cycles.layout_budget(L, {"e": (0, 0)}, {})["os"]
        per = (opz_cycles.KILL_SOFT - os_share) // 4
        self.assertEqual(opz_cycles.layout_budget(L, {"e": (per, per)}, {})["verdict"], "fits")
        self.assertTrue(opz_cycles.layout_budget(L, {"e": (per, per + 7_000)}, {})["verdict"].startswith("tight"))
        self.assertTrue(opz_cycles.layout_budget(L, {"e": (per + 10, per + 10)}, {})["verdict"].startswith("over"))
        raised = opz_cycles.layout_budget(L, {"e": (per + 10, per + 10)}, {}, line=330_000)
        self.assertEqual((raised["verdict"], raised["hard"]), ("fits", 355_000))


@unittest.skipUnless(opz_cycles.available(TOOLCHAIN), "bfin-elf-gcc / bfin-elf-run not installed in %s" % TOOLCHAIN)
@unittest.skipUnless(all(os.path.isdir(os.path.join(REPO, "engines", n)) for n in TEST_ENGINES),
                     "the test engines %s are not in engines/" % " and ".join(TEST_ENGINES))
class Simulator(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def measure(self, name, cases=None):
        return opz_cycles.measure(os.path.join(REPO, "engines", name), TOOLCHAIN, self.tmp.name, quick=True,
                                  cases=cases, quiet=True, timeout=120)

    def test_passthrough(self):
        r = self.measure("opz_test_passthrough")
        self.assertEqual(r["kind"], "effect")
        (c,) = r["cases"]
        self.assertEqual(c["blocks"], 24)
        self.assertGreater(c["per_call_mean"], 64)          # at least one instruction per frame
        self.assertLess(c["per_call_max"], 2000)
        self.assertEqual(c["per_call_min"], c["per_call_max"])   # no data-dependent work

    def test_sine_voices_and_envelope(self):
        cases = [opz_cycles.case("held", opz_cycles.params(), [60, 64], 4, 8),
                 opz_cycles.case("released", opz_cycles.params(release=0), [60], 4, 400, release_at=2)]
        r = self.measure("opz_test_sine", cases)
        held, released = r["cases"]
        self.assertEqual((held["voices"], held["blocks"]), (2, 8))
        self.assertGreater(held["per_call_mean"], 1000)
        self.assertGreater(held["note_on_max"], 0)
        self.assertGreater(held["callback_per_call"], 0)    # the host's envelope callback ran inside render
        self.assertLess(released["blocks"], 400)            # the host envelope freed the voice after note-off
        self.assertGreater(released["note_off_max"], 0)

    def test_deterministic(self):
        cases = [opz_cycles.case("a", opz_cycles.params(p1=0, p2=255), [48, 60], 4, 6)]
        a = self.measure("opz_test_sine", cases)["cases"][0]
        b = self.measure("opz_test_sine", cases)["cases"][0]
        self.assertEqual(a, b)


SDK_CHECK_LD = """OUTPUT_FORMAT("elf32-bfin")
OUTPUT_ARCH(bfin)
ENTRY(__start)
MEMORY { HOST (rwx) : ORIGIN = 0x80000000, LENGTH = 0x100000 }
SECTIONS
{
  .text   : { *(.text.start) *(.text .text.*) } > HOST
  .rodata : { *(.rodata .rodata.*) } > HOST
  .data   : { *(.data .data.* .sdata .sdata.*) } > HOST
  .bss (NOLOAD) : { *(.bss .bss.* .sbss .sbss.* COMMON) } > HOST
  __stack_top = ORIGIN(HOST) + LENGTH(HOST) - 256;
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.debug*) }
}
"""


@unittest.skipUnless(opz_cycles.available(TOOLCHAIN), "bfin-elf-gcc / bfin-elf-run not installed in %s" % TOOLCHAIN)
class HostMatchesDevice(unittest.TestCase):
    """tools/cycles/sdk_check.c: the SDK's per-sample helpers give the same bits on the host (opzsim) and on the
    Blackfin, so the golden renders show what the device plays."""

    def check(self, run, defines=()):
        src = os.path.join(TOOLS, "cycles", "sdk_check.c")
        inc = os.path.join(REPO, "sdk", "include")
        with tempfile.TemporaryDirectory() as tmp:
            host = os.path.join(tmp, "sdk_check")
            subprocess.run(["cc", "-std=c11", "-O2", "-I", inc, src, "-o", host], check=True)
            want = subprocess.run([host], capture_output=True, text=True, check=True).stdout
            ld = os.path.join(tmp, "check.ld")
            with open(ld, "w") as f:
                f.write(SDK_CHECK_LD)
            elf = os.path.join(tmp, "sdk_check.elf")
            subprocess.run([opz_cycles.tool(TOOLCHAIN, "gcc")] + opz_cycles.opz_build.CFLAGS
                           + ["-D" + d for d in defines]
                           + ["-I", inc, os.path.join(TOOLS, "cycles", "crt0.S"), src, "-nostdlib", "-nostartfiles",
                              "-T", ld, "-o", elf, "-lgcc"], check=True, capture_output=True)
            got = subprocess.run([run, "--memory-region", "0x80000000,0x100000", elf],
                                 capture_output=True, text=True, timeout=300).stdout
        self.assertTrue(want.endswith("done\n"), want)
        self.assertEqual(got.splitlines(), want.splitlines())

    def test_sdk_helpers_bit_identical(self):
        self.check(opz_cycles.tool(TOOLCHAIN, "run"))

    @unittest.skipUnless(os.access(BFP_RUN, os.X_OK), "no Blackfin+ simulator at %s (research/toolchain-setup.md)"
                         % BFP_RUN)
    def test_blackfin_plus_multiply_bit_identical(self):
        """opz_mul_q31 as the one-instruction Blackfin+ multiply (OPZ_BLACKFIN_PLUS) gives the host's bits."""
        self.check(BFP_RUN, defines=["OPZ_BLACKFIN_PLUS"])

if __name__ == "__main__":
    unittest.main()
