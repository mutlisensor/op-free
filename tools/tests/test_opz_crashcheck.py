"""Tests for opz_crashcheck.py and the crash checks in the cycle driver (tools/cycles/checks.S, driver.c).

Run from the repository root:

    python3 -m unittest discover -s tools/tests -v

The listing and parser tests always run. The simulator tests build small engines with one deliberate fault each
(written to a temporary directory) and check that the fault is reported; they are skipped when ~/opt/bfin-elf
has no bfin-elf-gcc or bfin-elf-run (research/toolchain-setup.md).
"""

import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import opz_crashcheck  # noqa: E402
import opz_crashlog  # noqa: E402
import opz_cycles  # noqa: E402
import opz_verify  # noqa: E402

TOOLCHAIN = os.path.expanduser("~/opt/bfin-elf")

LISTING = """
81b00010 <_render>:
81b00010:\te3 05       \t[--SP] = (R7:4, P5:3);
81b00012:\t00 e8 03 00 \tLINK 0xc;\t\t/* (12) */
81b00016:\t62 00       \tCALL (P2);
81b00018:\tff e3 10 00 \tCALL 0x81b00040 <_helper>;
81b0001c:\t00 36       \tL0 = R0;
81b0001e:\t01 e8 00 00 \tUNLINK;
81b00022:\t10 00       \tRTS;

81b00040 <_helper>:
81b00040:\t67 01       \t[--SP] = RETS;
81b00042:\t06 6f       \tSP += -0x20;\t\t/* (-32) */
81b00044:\tff e3 40 00 \tCALL 0x81b000c0 <___divsi3>;
81b00048:\t25 00       \tEXCPT 0x5;
81b0004a:\t10 00       \tRTS;

81b00060 <_onNoteOn>:
81b00060:\t00 e2 70 00 \tJUMP.L 0x81b00040 <_helper>;

81b00070 <_init>:
81b00070:\t00 e2 00 00 \tJUMP.S 0x81b00074 <_init+0x4>;
81b00074:\t10 00       \tRTS;

81b000c0 <___divsi3>:
81b000c0:\t10 00       \tRTS;
"""


class Listing(unittest.TestCase):
    def test_parse_and_stack(self):
        funcs = opz_crashcheck.parse_listing(LISTING)
        self.assertEqual(sorted(funcs), ["__divsi3", "helper", "init", "onNoteOn", "render"])
        depth, indirect, recursive = opz_crashcheck.stack_depths(funcs)
        self.assertEqual(depth["helper"], 4 + 32)                    # RETS push + SP adjustment
        self.assertEqual(depth["render"], 7 * 4 + 12 + 8 + 36)       # pushes + LINK + helper below it
        self.assertEqual(depth["onNoteOn"], 36)                      # a tail jump: helper's depth, no frame
        self.assertEqual(depth["init"], 0)                           # a jump inside itself is no call
        self.assertTrue(indirect["render"])
        self.assertFalse(indirect["helper"])
        self.assertEqual(recursive, [])

    def test_findings(self):
        r = opz_crashcheck.static_checks(LISTING)
        checks = sorted((f["check"], f["function"]) for f in r["findings"])
        self.assertIn(("instruction", "helper"), checks)             # EXCPT
        self.assertIn(("L register", "render"), checks)
        self.assertEqual(r["divisions"], {"helper": ["__divsi3"]})
        self.assertEqual(r["stack"]["render"], 84)

    def test_recursion(self):
        text = ("81b00010 <_f>:\n81b00010:\t67 01 \t[--SP] = RETS;\n"
                "81b00012:\tff e3 00 00 \tCALL 0x81b00010 <_f>;\n")
        _, _, recursive = opz_crashcheck.stack_depths(opz_crashcheck.parse_listing(text))
        self.assertEqual(recursive, ["f"])


class DriverOutput(unittest.TestCase):
    def test_findings_and_stacks_parse(self):
        text = ("M ok=1 mem=0 cycles=20 overhead=1\nC case=0\n"
                "F case=0 code=5 entry=4 a=1 b=284\nF case=0 code=5 entry=4 a=2 b=284\n"
                "F case=65535 code=3 entry=0 a=0 b=0\nK module=0 init=4 on=8 off=0 render=128 scratch=0\nD\n")
        p = opz_cycles.parse_output(text)
        self.assertEqual(p["started"], 0)
        self.assertEqual(p["stacks"]["render"], 128)
        cases = [opz_cycles.case("held", opz_cycles.params(), [60], 4, 2)]
        f = opz_cycles.describe_findings(p["findings"], cases, False)
        self.assertEqual(len(f), 2)                                  # one per code and entry point
        self.assertEqual((f[0]["check"], f[0]["function"], f[0]["case"]), ("state", "render", "held"))
        self.assertIn("byte 284, past the device's 280-byte", f[0]["detail"])
        self.assertEqual((f[1]["check"], f[1]["case"]), ("callee-saved", "initModule"))
        self.assertIn("R4 =", f[1]["detail"])

    def test_crash_description(self):
        cases = [opz_cycles.case("a", opz_cycles.params(), [60], 4, 2), opz_cycles.case("b", opz_cycles.params(),
                                                                                     [60], 4, 2)]
        text = ("M ok=1 mem=0 cycles=20 overhead=1\nC case=0\nR case=0 voices=1\nC case=1\n"
                "Unhandled exception 0x24 at 0x81b00456 (Unaligned data access)\n"
                "program stopped with signal 4 (Illegal instruction: 4).\n")
        info = opz_cycles.describe_crash("x", text, cases, [(0x81b00400, "render"), (0x81b00500, "helper")])
        self.assertEqual(info["excause"], 0x24)
        self.assertEqual(info["where"], "render+0x56")
        self.assertEqual(info["message"], 'x: misaligned data access at render+0x56 in case "b"')
        info = opz_cycles.describe_crash("x", "C case=0\nprogram stopped with signal 11 (Segmentation fault: 11).",
                                         cases)
        self.assertIn("unmapped memory", info["message"])
        self.assertFalse(info["torn"])
        torn = "C case=0\nT case=0 phase=1 words=3\nprogram stopped with signal 11 (Segmentation fault: 11)."
        info = opz_cycles.describe_crash("x", torn, cases)
        self.assertTrue(info["torn"])
        self.assertIn("re-trigger; 3 words changed", info["message"])
        info = opz_cycles.describe_crash("x", torn.replace("words=3\n", "words=3\nU\n"), cases)
        self.assertFalse(info["torn"])

    def test_header_has_the_device_limits(self):
        h = opz_cycles.bench_header(0x81b00000, False, [], [], ro=[(0x81b00000, 0x102)], tail=(0x81b01000, 0x80))
        self.assertIn("#define BENCH_STATE_LIMIT 280", h)
        self.assertIn("{ 0x81b00000u, 256u }", h)                    # whole words only
        self.assertIn("#define BENCH_TAIL_WORDS 32u", h)
        self.assertIn("#define BENCH_STATE_LIMIT 200", opz_cycles.bench_header(0x81b00000, True, [], []))


class CrashLog(unittest.TestCase):
    def test_decode_maps_addresses_to_our_engines(self):
        engines = opz_crashlog.our_engines()
        name, slot, base, _ = [e for e in engines if e[0] == "swirl"][0]
        log = ("[20.10.19 21:03:44] main.cpp:773 exception caught iid: 0x00130024, arg: 0x00000000\n"
               "[20.10.19 21:03:44] main.cpp:778 exc reason: ADI_EXC_DATA_MISALIGNED\n"
               "[20.10.19 21:03:44] callstack.cpp:40 RETX: 0x%08x\n"
               "[20.10.19 21:03:44] callstack.cpp:52 0x8012e334\n" % (base + 0x1a6))
        with tempfile.TemporaryDirectory() as tmp:                 # no builds: offsets only
            out = opz_crashlog.decode(log, engines, tmp, "/nonexistent")
        self.assertEqual(out[0], "exception, EXCAUSE 0x24: misaligned data access")
        self.assertEqual(out[1], "reason: ADI_EXC_DATA_MISALIGNED")
        self.assertTrue(out[2].startswith("faulting instruction (RETX) 0x%08x = swirl +0x1a6" % (base + 0x1a6)), out[2])
        self.assertIn("(slot %d)" % slot, out[2])
        self.assertEqual(sum(1 for line in out if line.startswith("  0x")), 1)   # the OS address is not listed

    def test_overload_lines_counted(self):
        out = opz_crashlog.decode("cpu overload: kill notes 1\ncpu overload: kill notes 2\n", [], "/x", "/x")
        self.assertEqual(out, ["cpu overload: 2 line(s), up to 2 note(s) killed"])


FAULTY = r"""
#include "opz_engine.h"
#define FAULT_%(fault)s 1
typedef struct State { uint32_t phase; volatile uint32_t base, offset; } State;   /* base + offset: a pointer */
static int32_t s_word[2];
static const ModuleCallbackInterface *s_cb;
static volatile uint32_t s_sink;
static const uint32_t *volatile s_null;
static volatile uint32_t s_offset = 2;

uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = 0;
    return 1;
}

void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)track; (void)arg2; (void)mem; (void)scratch;
    State *s = state;
    s->phase = 0;
    s->base = (uint32_t)s_word;
    s->offset = 0;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)velocity; (void)params;
    ((State *)state)->phase = 0;
#ifdef FAULT_null
    s_sink = s_null[2];
#endif
#ifdef FAULT_torn
    ((State *)state)->base = (uint32_t)s_word + 2;  /* aligned again only once both words are written */
    ((State *)state)->offset = (uint32_t)-2;
#endif
#ifdef FAULT_excpt
    if (note == 1000) __asm__ volatile("EXCPT 3;");
#endif
    (void)note;
}

void onNoteOff(int note, void *state) { (void)note; (void)state; }

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    State *s = state;
    (void)pitchSlope;
    s_cb->updateParametersADSR(env, params[2], params[3], params[4], params[5]);
    s_sink = *(volatile const int32_t *)(s->base + s->offset);
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        s->phase += pitch;
        out[2 * i] += (fract32)(s->phase >> 8);
    }
#ifdef FAULT_state
    ((volatile uint8_t *)state)[280] = 1;
#endif
#ifdef FAULT_lreg
    __asm__ volatile("L1 = %%0;" : : "d"(4));
#endif
#ifdef FAULT_loop
    __asm__ volatile("LC1 = %%0;" : : "d"(3));
#endif
#ifdef FAULT_callee
    __asm__ volatile("R5 = 0;");                       /* not declared as clobbered: never restored */
#endif
#ifdef FAULT_misaligned
    s_sink = *(volatile const uint32_t *)((const uint8_t *)out + s_offset);
#endif
#ifdef FAULT_params
    ((uint32_t *)params)[1] = 0;
#endif
#ifdef FAULT_out
    out[2 * OPZ_BLOCK_FRAMES + s_offset] = 1;
#endif
#ifdef FAULT_tail
    *(volatile uint32_t *)(%(tail)su) = 1;
#endif
}

OPZ_EXPORTS_INSTRUMENT();
"""

SLOT = 57


@unittest.skipUnless(opz_cycles.available(TOOLCHAIN), "bfin-elf-gcc / bfin-elf-run not installed in %s" % TOOLCHAIN)
class FaultyEngines(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def check(self, fault):
        edir = os.path.join(self.tmp.name, "src", "faulty_" + fault)
        os.makedirs(edir, exist_ok=True)
        base, size, _ = opz_verify.slot_window(SLOT)
        with open(os.path.join(edir, "faulty.c"), "w") as f:
            f.write(FAULTY % {"fault": fault, "tail": "0x%08x" % (base + size - 16)})
        with open(os.path.join(edir, "plug.json"), "w") as f:
            json.dump({"engine": "faulty_" + fault, "memoryslot": SLOT, "engine_version": 1,
                       "plug": {"id": 60999, "version": 1, "name": "faulty", "trackmask": 240, "flags": 0,
                                "parameter_names": ["none", "none"]}}, f)
        r = opz_crashcheck.check_engine(edir, TOOLCHAIN, os.path.join(self.tmp.name, "out"))
        found = r["static"]["findings"] + r["runtime"]["findings"]
        return r, {(f["level"], f["check"], f["function"]) for f in found}

    def test_clean_engine(self):
        r, found = self.check("none")
        self.assertEqual(found, set())
        self.assertGreater(r["runtime"]["stack"]["render"], 0)
        self.assertEqual(r["runtime"]["scratch"], 0)

    def test_state_past_280_bytes(self):
        _, found = self.check("state")
        self.assertIn(("error", "state", "render"), found)

    def test_l_register_left_set(self):
        _, found = self.check("lreg")
        self.assertIn(("error", "L register", "render"), found)
        self.assertIn(("warning", "L register", "render"), found)   # the static check sees the write too

    def test_loop_counter_left_set(self):
        _, found = self.check("loop")
        self.assertIn(("note", "loop counter", "render"), found)

    def test_callee_saved_register_clobbered(self):
        _, found = self.check("callee")
        self.assertIn(("error", "callee-saved", "render"), found)

    def test_params_written(self):
        _, found = self.check("params")
        self.assertIn(("error", "params", "render"), found)

    def test_output_overrun(self):
        _, found = self.check("out")
        self.assertIn(("error", "output buffer", "render"), found)

    def test_write_past_bss(self):
        _, found = self.check("tail")
        self.assertIn(("error", "slot window", "after the case"), found)

    def test_misaligned_access_stops_the_run(self):
        r, found = self.check("misaligned")
        self.assertEqual(r["runtime"]["crash"]["excause"], 0x24)
        self.assertTrue(r["runtime"]["crash"]["where"].startswith("render+"))
        self.assertIn(("error", "exception", r["runtime"]["crash"]["where"]), found)

    def test_null_pointer_stops_the_run(self):
        r, _ = self.check("null")
        self.assertIn("unmapped memory", r["runtime"]["crash"]["message"])
        self.assertIn(' in case "', r["runtime"]["crash"]["message"])

    def test_half_written_state_is_rendered(self):
        r, found = self.check("torn")
        crash = r["runtime"]["crash"]
        self.assertEqual(crash["excause"], 0x24)
        self.assertTrue(crash["torn"])
        self.assertIn("only partly written", crash["message"])

    def test_excpt_is_a_static_error(self):
        r, found = self.check("excpt")
        self.assertIn(("error", "instruction", "onNoteOn"), found)
        self.assertGreater(r["errors"], 0)


if __name__ == "__main__":
    unittest.main()
