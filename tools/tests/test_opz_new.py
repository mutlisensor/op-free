"""Tests for opz_new.py (`opz.py new`), the engine scaffold. Run:
    python3 -m unittest discover -s tools/tests

Each test works in a temporary copy of the repository's layout (engines/, sdk/templates/, sdk/docs/memory-slots.md).
The template tests also compile every scaffolded engine for the Mac and run its scenarios in opzsim when
sim/build/opzsim exists (make -C sim), and build it for the device when ~/opt/bfin-elf has the toolchain; they are
skipped otherwise.
"""

import argparse
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import opz  # noqa: E402
import opz_new  # noqa: E402

OPZSIM = os.path.join(REPO, "sim", "build", "opzsim")
BFIN = os.path.expanduser("~/opt/bfin-elf")

SLOTS_MD = """# Memory slots

| Slot | Engine | Plug id | Status |
|---|---|---|---|
| 87 | `a` | 60900 | planned |
| 57 | `b` (P3 proof) | 60901 | planned |

## Consequences
"""


def namespace(name, kind, **kw):
    a = argparse.Namespace(name=name, kind=kind, size="auto", slot=None, plug_id=None, dry_run=False)
    for k, v in kw.items():
        setattr(a, k, v)
    return a


class Base(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root)
        os.makedirs(os.path.join(self.root, "engines"))
        os.makedirs(os.path.join(self.root, "sdk", "docs"))
        shutil.copytree(os.path.join(REPO, "sdk", "templates"), os.path.join(self.root, "sdk", "templates"))
        with open(self.slots_md(), "w") as f:
            f.write(SLOTS_MD)

    def slots_md(self):
        return os.path.join(self.root, "sdk", "docs", "memory-slots.md")

    def add_engine(self, name, slot, pid):
        d = os.path.join(self.root, "engines", name)
        os.makedirs(d)
        with open(os.path.join(d, "plug.json"), "w") as f:
            json.dump({"engine": name, "memoryslot": slot, "plug": {"id": pid, "version": 1, "trackmask": 240}}, f)


class Allocation(Base):
    def test_te_names_match_opz(self):
        self.assertEqual(opz_new.TE_ENGINES, opz.TE_ENGINES)
        self.assertEqual(opz_new.MAX_SLOT, opz.MAX_SLOT)

    def test_used_slots_and_ids_come_from_engines_and_the_table(self):
        self.add_engine("c", 86, 61002)
        self.assertEqual(opz_new.used_slots(self.root), {87, 57, 86})
        self.assertEqual(opz_new.used_plug_ids(self.root), {60900, 60901, 61002})

    def test_slots_top_down_skipping_used(self):
        self.add_engine("c", 86, 61002)
        self.assertEqual(opz_new.pick_slot(opz_new.used_slots(self.root)), 85)
        self.assertEqual(opz_new.pick_slot(opz_new.used_slots(self.root), "10"), 56)
        self.assertEqual(opz_new.pick_slot(set(), "50"), 7)

    def test_auto_falls_back_to_10_kb_when_5_kb_is_full(self):
        used = set(opz_new.OUR_SLOTS[5])
        self.assertEqual(opz_new.pick_slot(used), 57)
        with self.assertRaises(opz_new.ScaffoldError):
            opz_new.pick_slot(used, "5")

    def test_our_ranges_hold_no_te_slot_and_nothing_above_89(self):
        for slots in opz_new.OUR_SLOTS.values():
            self.assertFalse(set(slots) & opz_new.TE_SLOTS)
            self.assertTrue(all(0 < s <= 89 for s in slots))
            self.assertEqual(slots, sorted(slots, reverse=True))
        for slots in opz_new.OUR_SLOTS.values():
            self.assertTrue(all(opz_new.slot_class(s) == c for c, ss in opz_new.OUR_SLOTS.items() for s in ss))

    def test_refused_slots(self):
        for slot in (0, -1, 90, 255, 256, 4, 60, 88, 89, 87):      # 0, out of range, TE's, used (87)
            with self.subTest(slot=slot), self.assertRaises(opz_new.ScaffoldError):
                opz_new.check_slot(slot, {87})
        opz_new.check_slot(71, {87})

    def test_plug_ids(self):
        self.assertEqual(opz_new.pick_plug_id({60900, 60901, 60903}), 60902)
        for pid in (0, 60000, 60899, 60900):
            with self.subTest(pid=pid), self.assertRaises(opz_new.ScaffoldError):
                opz_new.check_plug_id(pid, {60900})
        opz_new.check_plug_id(60999, set())
        opz_new.check_plug_id(61300, set())
        with self.assertRaises(opz_new.ScaffoldError):
            opz_new.pick_plug_id(set(opz_new.TEST_IDS))

    def test_names(self):
        for bad in ("Bad", "1abc", "a-b", "a b", "", "x" * 25, "chorus", "ep", "mutable_rings", "teenage_kick"):
            with self.subTest(name=bad), self.assertRaises(opz_new.ScaffoldError):
                opz_new.check_name(bad, self.root)
        self.add_engine("taken", 86, 61002)
        with self.assertRaises(opz_new.ScaffoldError):
            opz_new.check_name("taken", self.root)
        opz_new.check_name("wobble_2", self.root)

    def test_explicit_slot_and_id_are_checked(self):
        with self.assertRaises(opz_new.ScaffoldError):
            opz_new.plan("x", "synth", self.root, slot=90)
        with self.assertRaises(opz_new.ScaffoldError):
            opz_new.plan("x", "synth", self.root, slot=87)
        with self.assertRaises(opz_new.ScaffoldError):
            opz_new.plan("x", "synth", self.root, plug_id=60901)
        p = opz_new.plan("x", "fx", self.root, slot=12, plug_id=61100)
        self.assertEqual((p["slot"], p["slot_class"], p["plug_id"]), (12, 10, 61100))


class Scaffold(Base):
    def scaffold(self, name, kind, **kw):
        out = io.StringIO()
        rc = opz_new.run(namespace(name, kind, **kw), self.root, out)
        return rc, out.getvalue()

    def test_dry_run_writes_nothing(self):
        rc, text = self.scaffold("dry", "synth", dry_run=True)
        self.assertEqual(rc, 0)
        self.assertIn("plug id 60902, memory slot 86 (5 KB)", text)
        self.assertFalse(os.path.exists(os.path.join(self.root, "engines", "dry")))
        with open(self.slots_md()) as f:
            self.assertEqual(f.read(), SLOTS_MD)

    def test_every_kind(self):
        masks = {"synth": 240, "drum": 15, "fx": 768}
        for n, kind in enumerate(opz_new.KINDS):
            name = "new_" + kind
            rc, _ = self.scaffold(name, kind)
            self.assertEqual(rc, 0)
            d = os.path.join(self.root, "engines", name)
            self.assertEqual(sorted(os.listdir(d)), ["README.md", name + ".c", "plug.json", "presets.json", "tests"])
            with open(os.path.join(d, "plug.json")) as f:
                plug = json.load(f)
            self.assertEqual(plug["engine"], name)
            self.assertEqual(plug["plug"]["name"], name)
            self.assertEqual(plug["memoryslot"], [86, 85, 84][n])
            self.assertEqual(plug["plug"]["id"], [60902, 60903, 60904][n])
            self.assertEqual(plug["plug"]["trackmask"], masks[kind])
            for dirpath, _, files in os.walk(d):
                for fn in files:
                    with open(os.path.join(dirpath, fn)) as f:
                        text = f.read()
                    for token in (opz_new.TEMPLATE_NAME, "@SLOT@", "@SLOT_CLASS@", "@PLUG_ID@"):
                        self.assertNotIn(token, text, "%s in %s" % (token, fn))
            self.assertTrue(os.listdir(os.path.join(d, "tests")))
            engine = opz.load_manifest(d)
            errors, warnings = opz.preset_problems(engine["presets"], plug["plug"]["trackmask"])
            self.assertEqual((errors, warnings), ([], []))
        with open(self.slots_md()) as f:
            text = f.read()
        self.assertIn("| 84 | `new_fx` (fx, from the SDK template) | 60904 | scaffolded |\n\n## Consequences", text)
        self.assertTrue(opz_new.used_slots(self.root) >= {86, 85, 84})

    def test_existing_engine_is_refused(self):
        self.assertEqual(self.scaffold("twice", "fx")[0], 0)
        self.assertEqual(self.scaffold("twice", "fx")[0], 2)

    def test_opz_cli_has_new(self):
        out = io.StringIO()
        old, sys.stdout = sys.stdout, out
        try:
            rc = opz.main(["new", "cli_check", "--kind", "drum", "--dry-run"])
        finally:
            sys.stdout = old
        self.assertEqual(rc, 0)
        self.assertIn("dry run: nothing written", out.getvalue())


@unittest.skipUnless(os.access(OPZSIM, os.X_OK) and shutil.which("clang"), "needs make -C sim and clang")
class TemplatesRun(Base):
    """Every template, scaffolded, compiles for the Mac and passes its own scenarios in opzsim."""

    def test_scenarios_pass(self):
        for kind in opz_new.KINDS:
            name = "run_" + kind
            opz_new.run(namespace(name, kind), self.root, io.StringIO())
            d = os.path.join(self.root, "engines", name)
            dylib = os.path.join(self.root, name + ".dylib")
            subprocess.run(["clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
                            "-fno-common", "-ffp-contract=off", "-fno-stack-protector",
                            "-I" + os.path.join(REPO, "sdk", "include"), "-dynamiclib", "-o", dylib,
                            os.path.join(d, name + ".c")], check=True)
            with open(os.path.join(d, "plug.json")) as f:
                plug = json.load(f)
            tests = os.path.join(d, "tests")
            for scn in sorted(os.listdir(tests)):
                with self.subTest(kind=kind, scenario=scn):
                    r = subprocess.run([OPZSIM, "-e", dylib, "--slot", str(plug["memoryslot"]), "--trackmask",
                                        str(plug["plug"]["trackmask"]), os.path.join(tests, scn)],
                                       capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, r.stderr)
                    self.assertNotIn("warning", r.stderr)

    @unittest.skipUnless(os.access(os.path.join(BFIN, "bin", "bfin-elf-gcc"), os.X_OK), "needs ~/opt/bfin-elf")
    def test_device_build(self):
        for kind in opz_new.KINDS:
            name = "dev_" + kind
            opz_new.run(namespace(name, kind), self.root, io.StringIO())
            r = subprocess.run([sys.executable, os.path.join(TOOLS, "opz_build.py"),
                                os.path.join(self.root, "engines", name), "--toolchain", BFIN,
                                "-o", os.path.join(self.root, "bfin")], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(os.path.isfile(os.path.join(self.root, "bfin", name, name + ".engine")))


if __name__ == "__main__":
    unittest.main()
