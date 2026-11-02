"""Offline tests for project files and patterns (tools/opz_project.py) and `opz.py projects`. Run:
    python3 -m unittest discover -s tools/tests

Synthetic projects only. The user's own files are read (never written) when OPZ_PROJECTS names a folder of .opz
files, or when `opz.py projects backup` has left some under backups/ (git-ignored).
"""

import glob
import hashlib
import json
import os
import random
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "third_party", "opzsysex"))
import opz  # noqa: E402
import opz_project as op  # noqa: E402
from fake_opz import FakeOpz  # noqa: E402
from opzsysex.project import Note, PatternBankEditor  # noqa: E402
from opzsysex.sysex import decode_frame  # noqa: E402


def old_file(major=71) -> bytes:
    """A pre-73 file: no minor word at the end (as the user's 2023 backups of versions 71 and 72)."""
    pr = op.Project()
    return struct.pack("<I", major) + bytes(pr.glob) + bytes(pr.bank)


class Format(unittest.TestCase):
    def test_empty_project(self):
        pr = op.Project()
        data = pr.to_bytes()
        self.assertEqual(len(data), op.FILE_SIZE)
        self.assertEqual(len(data), 342_848)
        self.assertEqual(data[:4], b"\x49\0\0\0")
        self.assertEqual(data[-4:], b"\x07\0\0\0")
        self.assertEqual(pr.problems(), [])
        self.assertEqual(op.Project.parse(data).to_bytes(), data)
        self.assertEqual(pr.tempo, 120)
        self.assertEqual(pr.track(0, 5)["step_count"], 16)
        self.assertEqual(pr.track(0, 5)["step_length"], 1)
        self.assertEqual(pr.notes(3, 6, 7), [])
        self.assertEqual(pr.routing(0), (0x00FF, 0x00F0))
        self.assertEqual(pr.summary()["chains"], {})

    def test_old_versions_round_trip(self):
        for major in (70, 71, 72):
            data = old_file(major)
            pr = op.Project.parse(data)
            self.assertEqual((pr.major, pr.minor), (major, None))
            self.assertEqual(pr.to_bytes(), data)
            self.assertTrue(pr.problems()[0].startswith("version %d.0" % major))

    def test_rejects_malformed(self):
        data = op.Project().to_bytes()
        for bad in (data[:-1], data + b"\0", b"", struct.pack("<I", 74) + data[4:], struct.pack("<I", 0) + data[4:]):
            with self.assertRaises(op.FormatError):
                op.Project.parse(bad)
        with self.assertRaises(op.FormatError):
            op.Project.parse(data[:-4])          # version 73 without its minor word

    def test_edits_match_opzsysex(self):
        """Our setters write the same bytes as the vendored, device-proven PatternBankEditor."""
        pr = op.Project()
        ed = PatternBankEditor(bytes(pr.bank))
        pr.set_notes(2, 6, 5, [{"note": 60, "velocity": 90, "duration": 3100, "micro": -3},
                               {"note": 67, "velocity": 1, "duration": 1, "micro": 11, "age": 9}])
        ed.set_notes(2, 6, 5, [Note(60, 90, 3100, -3), Note(67, 1, 1, 11, 9)])
        pr.set_notes(2, 6, 6, [{"note": 1}])
        pr.set_notes(2, 6, 6, [])
        ed.set_notes(2, 6, 6, [Note(1, 100, 6200)])
        ed.set_notes(2, 6, 6, [])
        pr.set_lock(4, 5, 0, "filter", 200)
        ed.set_parameter_lock(4, 5, 0, 8, 200)
        pr.set_lock(4, 5, 1, "level", 3)
        pr.set_lock(4, 5, 1, "level", None)
        ed.set_parameter_lock(4, 5, 1, 11, 3)
        ed.set_parameter_lock(4, 5, 1, 11, None)
        pr.set_component(4, 4, 2, "glide", 7)
        ed.set_component(4, 4, 2, 7, 7)
        pr.set_component(4, 4, 3, "component_spark", 36)
        pr.set_component(4, 4, 3, "component_spark", None)
        ed.set_component(4, 4, 3, 13, 36)
        ed.set_component(4, 4, 3, 13, None)
        pr.set_sound(15, 0, range(18))
        for i in range(18):
            ed.set_sound_parameter(15, 0, i, i)
        pr.set_track(1, 9, step_count=7, plug=61004)
        ed.set_step_count(1, 9, 7)
        ed.set_active_plug(1, 9, 61004)
        pr.set_muted(0, 13, True)
        ed.set_muted(0, 13, True)
        self.assertEqual(bytes(pr.bank), ed.bytes())

    def test_reads_back_edits(self):
        pr = op.Project()
        pr.set_notes(0, 4, 0, [{"note": 40, "velocity": 70, "duration": 9300, "micro": 2}])
        self.assertEqual(pr.notes(0, 4, 0), [{"note": 40, "velocity": 70, "duration": 9300, "micro": 16, "age": 0}])
        pr.set_component(0, 4, 0, "pulse", 4)
        pr.set_lock(0, 4, 0, "resonance", 99)
        self.assertEqual(pr.step(0, 4, 0), {"mask": 1, "components": {"pulse": 4}, "locks": {"resonance": 99}})
        pr.set_chain(14, [15, 0])
        self.assertEqual(pr.chain(14), [15, 0])
        pr.set_chain(14, [])
        self.assertEqual(pr.chain(14), [])
        self.assertEqual(pr.problems(), [])

    def test_edit_limits(self):
        pr = op.Project()
        bad = [lambda: pr.set_notes(0, 8, 0, [{"note": 1}, {"note": 2}]),     # FX1 holds one note per step
               lambda: pr.set_notes(0, 0, 0, [{"note": 128}]),
               lambda: pr.set_notes(0, 0, 0, [{"note": 1, "velocity": 0}]),
               lambda: pr.set_notes(0, 0, 0, [{"note": 1, "micro": 12}]),
               lambda: pr.set_notes(16, 0, 0, []),
               lambda: pr.set_track(0, 0, step_count=17),
               lambda: pr.set_track(0, 0, step_length=11),
               lambda: pr.set_chain(15, [0]),                                 # record 16 is the live chain
               lambda: pr.set_chain(0, [0] * 32),
               lambda: pr.set_tempo(201),
               lambda: pr.set_lock(0, 0, 0, "filter", 256)]
        for f in bad:
            with self.assertRaises((op.FormatError, ValueError)):
                f()

    def test_problems_catch_what_the_os_never_writes(self):
        cases = {
            "step count 0": lambda pr: pr.bank.__setitem__(4, 0),
            "step length 11": lambda pr: pr.bank.__setitem__(6, 11),
            "note 200": lambda pr: pr.bank.__setitem__(op._note_at(0, 0, 0, 0) + 4, 200),
            "micro 3": lambda pr: (pr.set_notes(0, 0, 0, [{"note": 1}]), pr.bank.__setitem__(op._note_at(0, 0, 0, 0) + 6, 3)),
            "reserved component bits": lambda pr: struct.pack_into("<H", pr.bank, op._step_at(0, 0, 0), 0x4000),
            "lock enable not 00/FF": lambda pr: pr.bank.__setitem__(op._step_at(0, 0, 0) + 40, 1),
            "mute group 10": lambda pr: pr.bank.__setitem__(op.MUTE_GROUP_AT, 10),
            "chain 3": lambda pr: pr.glob.__setitem__(2 * 32, 16),
            "tempo 30": lambda pr: struct.pack_into("<H", pr.glob, op.TEMPO_AT, 30),
            "selected chain 15": lambda pr: pr.glob.__setitem__(op.SELECTED_CHAIN_AT, 15),
        }
        for want, spoil in cases.items():
            pr = op.Project()
            spoil(pr)
            got = pr.problems()
            self.assertTrue(any(want in p for p in got), "%s not in %s" % (want, got))

    def test_negative_durations_are_accepted(self):
        """The OS itself stores some note durations as -256 (seen in the user's version 73.7 files)."""
        pr = op.Project()
        pr.set_notes(0, 5, 0, [{"note": 60}])
        struct.pack_into("<i", pr.bank, op._note_at(0, 5, 0, 0), -256)
        self.assertEqual(pr.problems(), [])


class Generators(unittest.TestCase):
    def test_euclid(self):
        x = lambda s: [c == "x" for c in s]
        self.assertEqual(op.euclid(3, 8), x("x..x..x."))
        self.assertEqual(op.euclid(4, 16), x("x...x...x...x..."))
        self.assertEqual(op.euclid(3, 8, 1), x(".x..x..x"))
        self.assertEqual(op.euclid(0, 4), x("...."))
        self.assertEqual(op.euclid(7, 7), [True] * 7)
        for k in range(17):
            self.assertEqual(sum(op.euclid(k, 16)), k)
        with self.assertRaises(op.FormatError):
            op.euclid(5, 4)

    def test_write_rhythm(self):
        pr = op.Project()
        op.write_rhythm(pr, 2, 0, op.euclid(3, 8), note=53, velocity=120, length=0.5)
        self.assertEqual(pr.track(2, 0)["step_count"], 8)
        self.assertEqual([bool(pr.notes(2, 0, s)) for s in range(8)], op.euclid(3, 8))
        self.assertEqual(pr.notes(2, 0, 3), [{"note": 53, "velocity": 120, "duration": 3100, "micro": 0, "age": 0}])
        self.assertEqual(pr.problems(), [])

    def test_write_pages(self):
        pr = op.Project()
        op.write_rhythm(pr, 4, 0, op.euclid(4, 16), note=53)       # a kick in the first page's pattern
        steps = [[{"note": 40 + s}] for s in range(40)]
        used = op.write_pages(pr, 4, steps, first=4, chain_slot=2)
        self.assertEqual(used, [4, 5, 6])
        self.assertEqual(pr.chain(2), [4, 5, 6])
        self.assertEqual([pr.track(p, 4)["step_count"] for p in used], [16, 16, 8])
        self.assertEqual(pr.notes(6, 4, 7)[0]["note"], 40 + 39)
        self.assertEqual(pr.notes(6, 4, 8), [])
        for p in used:                                              # the other tracks repeat on every page
            self.assertEqual([bool(pr.notes(p, 0, s)) for s in range(16)], op.euclid(4, 16))
        with self.assertRaises(op.FormatError):
            op.write_pages(pr, 4, steps, first=14, chain_slot=0)    # three pages do not fit after pattern 15


class Transport(unittest.TestCase):
    def test_project_names_and_ids(self):
        self.assertEqual(op.project_path(1), "projects/project01.opz")
        self.assertEqual(op.project_path(10), "projects/project10.opz")
        self.assertEqual([op.project_file_id(n) for n in (1, 10)], [3, 12])
        for n in (0, 11, -1):
            with self.assertRaises(op.FormatError):
                op.project_path(n)

    def test_bank_upload_frames(self):
        pr = op.Project()
        op.write_rhythm(pr, 0, 0, op.euclid(5, 16))
        prefix = pr.pattern(0)
        frames = op.bank_upload_frames(prefix, 9, 0x1234)
        packets = [decode_frame(f) for f in frames]
        self.assertEqual([p.message_id for p in packets], [0x09] * (len(packets) - 1) + [0x0A])
        self.assertTrue(all(p.payload[0] == 0x90 and p.payload[2:4] == b"\x34\x12" for p in packets))
        self.assertEqual(zlib.decompress(b"".join(p.payload[6:] for p in packets)), prefix)
        for project in (10, 15, -1):
            with self.assertRaises(op.FormatError):
                op.bank_upload_frames(prefix, project, 1)
        with self.assertRaises(op.FormatError):
            op.bank_upload_frames(prefix[:-1], 0, 1)
        noise = random.Random(7).randbytes(op.PATTERN_SIZE * 4)
        with self.assertRaises(op.FormatError):                     # would wrap the receiver's 16-bit chunk offset
            op.bank_upload_frames(noise, 0, 1)
        whole = bytes(op.Project().bank)
        self.assertLess(len(zlib.compress(whole)), op.MAX_BANK_COMPRESSED)
        op.bank_upload_frames(whole, 0, 1)


class Device(unittest.TestCase):
    """`opz.py projects` against the fake: backup, put, refusals, rollback."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.lines = []
        self.dev = FakeOpz()
        self.opz = opz.Opz(self.dev, log=self.lines.append, backups=self.tmp.name, confirm=lambda q: True,
                           clock=lambda: self.dev.now)

    def tearDown(self):
        self.tmp.cleanup()

    def song(self) -> bytes:
        pr = op.Project()
        op.write_rhythm(pr, 0, 0, op.euclid(4, 16), note=53)
        pr.set_tempo(128)
        return pr.to_bytes()

    def test_backup_reads_all_ten(self):
        self.dev.files[op.project_path(3)] = self.song()
        d, files = self.opz.backup_projects()
        self.assertEqual(sorted(files), list(range(1, 11)))
        with open(os.path.join(d, "projects", "project03.opz"), "rb") as f:
            self.assertEqual(f.read(), self.song())
        with open(os.path.join(d, "backup.json")) as f:
            meta = json.load(f)["sha256"]
        self.assertEqual(meta["projects/project03.opz"], hashlib.sha256(self.song()).hexdigest())
        self.assertEqual(self.dev.written, set())

    def test_put_on_a_spare_project(self):
        self.assertTrue(self.opz.put_project(7, self.song(), yes=True))
        self.assertEqual(self.dev.files["projects/project07.opz"], self.song())
        self.assertEqual(self.dev.reloaded, [6])
        self.assertTrue(any("written and verified" in l for l in self.lines))

    def test_put_refuses(self):
        self.dev.files[op.project_path(2)] = self.song()
        other = op.Project.parse(self.song())
        other.set_tempo(90)
        self.assertFalse(self.opz.put_project(2, other.to_bytes(), yes=True))      # project 2 has notes
        self.assertEqual(self.dev.written, set())
        spoiled = bytearray(self.song())
        spoiled[4 + op.GLOBAL_SIZE + 4] = 0                                         # step count 0
        self.assertFalse(self.opz.put_project(5, bytes(spoiled), yes=True))
        self.assertFalse(self.opz.put_project(5, old_file(72), yes=True))
        self.assertTrue(self.opz.put_project(5, self.song(), dry_run=True))
        self.assertEqual(self.dev.written, set())
        self.assertTrue(self.opz.put_project(2, other.to_bytes(), overwrite=True, yes=True))
        self.assertEqual(self.dev.reloaded, [1])
        with self.assertRaises(op.FormatError):
            self.opz.put_project(11, self.song(), yes=True)

    def test_rollback_when_read_back_differs(self):
        original = self.dev.files.get(op.project_path(4))
        finish = self.dev._finish_sync

        def lossy():                                    # a device that stores a different file than it received
            finish()
            path = op.project_path(4)
            if path in self.dev.written and self.dev.files[path] == self.song():
                self.dev.files[path] = self.dev.files[path][:-1] + b"\x08"
        self.dev._finish_sync = lossy
        self.assertFalse(self.opz.put_project(4, self.song(), yes=True))
        self.assertEqual(self.dev.files[op.project_path(4)], op.Project().to_bytes() if original is None else original)
        self.assertTrue(any("rollback verified" in l for l in self.lines))

    def test_fake_models_the_index_bug(self):
        self.opz.push([("projects/project11.opz", self.song())])
        self.assertEqual(self.dev.reloaded, [10])
        self.assertTrue(any("RAM CORRUPTED" in l for l in self.dev.log))


class Cli(unittest.TestCase):
    def test_empty_rhythm_check_diff(self):
        import io
        from contextlib import redirect_stdout
        with tempfile.TemporaryDirectory() as d:
            a, b = os.path.join(d, "a.opz"), os.path.join(d, "b.opz")
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(op.main(["empty", a]), 0)
                self.assertEqual(op.main(["rhythm", a, b, "--pattern", "2", "--track", "kick", "--hits", "4"]), 0)
                self.assertEqual(op.main(["check", a, b]), 0)
                self.assertEqual(op.main(["diff", a, b]), 0)
            text = out.getvalue()
            self.assertIn("x...x...x...x...; 0 problem(s)", text)
            self.assertIn("pattern 2 differs: kick", text)
            with open(b, "rb") as f:
                self.assertEqual(op.Project.parse(f.read()).notes(1, 0, 4)[0]["note"], 53)
            with open(a, "wb") as f:
                f.write(old_file(72))
            with redirect_stdout(io.StringIO()):
                self.assertEqual(op.main(["check", a]), 1)


class Vectors(unittest.TestCase):
    def test_committed_vectors_are_current(self):
        with open(op.VECTORS) as f:
            self.assertEqual(f.read(), op._dump(op.vectors()), "run: python3 tools/opz_project.py vectors")


def user_files() -> list[str]:
    roots = [os.environ["OPZ_PROJECTS"]] if os.environ.get("OPZ_PROJECTS") else \
        glob.glob(os.path.join(ROOT, "backups", "*projects*", "projects"))
    return sorted(f for r in roots for f in glob.glob(os.path.join(r, "*.opz")))


@unittest.skipUnless(user_files(), "no project files of the user's (set OPZ_PROJECTS or run opz.py projects backup)")
class UserFiles(unittest.TestCase):
    def test_round_trip_and_checks(self):
        for path in user_files():
            with open(path, "rb") as f:
                data = f.read()
            pr = op.Project.parse(data)
            self.assertEqual(pr.to_bytes(), data, path)
            if (pr.major, pr.minor) == (op.MAJOR, op.MINOR):
                self.assertEqual(pr.problems(), [], path)


if __name__ == "__main__":
    unittest.main()
