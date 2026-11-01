"""Offline tests for config/userPresets.dat (tools/opz_userpresets.py) and `opz.py presets bake`. Run:
    python3 -m unittest discover -s tools/tests
"""

import argparse
import io
import json
import os
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import opz  # noqa: E402
import opz_userpresets as u  # noqa: E402

TABLE = 61016


def preset(slot=0, index=0, name="p", values=None, octave=0):
    return {"slot": slot, "index": index, "name": name, "defaultOctave": octave, "values": list(values or [7] * 18)}


def os_file(user=((TABLE, 0, 1, "my bass", [9] * 18),), garbage_free=True) -> bytes:
    """A userPresets.dat the way savePresets writes it: installed plugs in id order, each record a copy of the plug's
    block (factory presets without user bits, stored ones with), the rest id 0 (here with stale bytes left)."""
    records = [bytearray(u.RECORD) for _ in range(u.RECORDS)]
    # TE plug 10 with a factory preset (preset mask only)
    struct.pack_into("<I", records[0], u.ID_AT, 10)
    records[0][0:u.PRESET] = u.preset_bytes("factory", [1] * 18, -1)
    struct.pack_into("<H", records[0], u.MASK_AT, 1)
    struct.pack_into("<I", records[1], u.ID_AT, TABLE)
    for pid, slot, index, name, values in user:
        r = next(x for x in records if u.plug_id(x) == pid)
        at = slot * u.SLOT_STRIDE + index * u.PRESET
        r[at:at + u.PRESET] = u.preset_bytes(name, values, 0)
        for m in (u.MASK_AT, u.USER_AT):
            struct.pack_into("<H", r, m + 2 * slot, u.mask(r, m, slot) | 1 << index)
    if garbage_free:
        records[5][0:40] = b"\xaa" * 40          # a free record with stale bytes (id 0)
    return u.serialise(records)


def os_load(data: bytes, installed: set) -> dict:
    """loadPresets' user loop (0x800fcf8c-0x800fd058): for installed ids, presets whose user bit is set."""
    out = {}
    for r in u.parse(data):
        pid = u.plug_id(r)
        if pid and pid in installed:
            out[pid] = {(p["slot"], p["index"]): p for p in u.user_presets(r)}
    return out


class FormatTests(unittest.TestCase):
    def test_round_trip_and_summary(self):
        data = os_file()
        records = u.parse(data)
        self.assertEqual(u.serialise(records), data)
        self.assertEqual(len(data), 351202)
        self.assertEqual(u.summary(records)[0], "2 plug records, 198 free; 1 with user presets")
        self.assertIn("plug 61016: slot 0 index 1 'my bass'", u.summary(records)[1])

    def test_strict_checks(self):
        data = os_file()
        with self.assertRaisesRegex(u.FormatError, "must be 351202 bytes"):
            u.parse(data[:-1])
        with self.assertRaisesRegex(u.FormatError, "version is 2"):
            u.parse(b"\x02\x00" + data[2:])
        records = u.parse(data)
        struct.pack_into("<I", records[3], u.ID_AT, TABLE)
        with self.assertRaisesRegex(u.FormatError, "plug 61016 has two records"):
            u.parse(u.serialise(records))
        records = u.parse(data)
        struct.pack_into("<H", records[1], u.USER_AT, 1 << 14)
        with self.assertRaisesRegex(u.FormatError, "past index 13"):
            u.parse(u.serialise(records))

    def test_preset_bytes(self):
        b = u.preset_bytes("a name longer than eleven", list(range(18)), 9)
        self.assertEqual(len(b), 31)
        self.assertEqual(b[:12], b"a name long\0")
        self.assertEqual(b[12:30], bytes(range(18)))
        self.assertEqual(struct.unpack("<b", b[30:])[0], 4, "octave clamped like readPresetSettings")
        with self.assertRaises(u.FormatError):
            u.preset_bytes("café", [0] * 18, 0)
        with self.assertRaises(u.FormatError):
            u.preset_bytes("x", [0] * 17, 0)


class BakeTests(unittest.TestCase):
    def test_bake_loads_as_user_presets_and_keeps_the_users(self):
        records = u.parse(os_file())
        ours = [preset(0, 0, "wave bass", [1] * 18, -2), preset(0, 1, "reso bass", [2] * 18), preset(3, 13, "chord")]
        report = u.bake(records, [(TABLE, "table", ours)])
        self.assertEqual(report["table"], {"new": 2, "same": 0, "updated": 0, "replaced": 0, "kept": [(0, 1, "my bass")]})
        loaded = os_load(u.serialise(records), {10, TABLE})
        self.assertEqual(sorted(loaded[TABLE]), [(0, 0), (0, 1), (3, 13)])
        self.assertEqual(loaded[TABLE][(0, 0)]["name"], "wave bass")
        self.assertEqual(loaded[TABLE][(0, 0)]["defaultOctave"], -2)
        self.assertEqual(loaded[TABLE][(0, 1)]["name"], "my bass", "the hand-stored preset wins by default")
        self.assertEqual(loaded[10], {}, "TE's record untouched")

    def test_overwrite_user_and_same(self):
        records = u.parse(os_file())
        ours = [preset(0, 1, "reso bass", [2] * 18)]
        self.assertEqual(u.bake(records, [(TABLE, "table", ours)], overwrite_user=True)["table"]["replaced"], 1)
        self.assertEqual(u.bake(records, [(TABLE, "table", ours)])["table"],
                         {"new": 0, "same": 1, "updated": 0, "replaced": 0, "kept": []})

    def test_an_earlier_bake_is_updated_but_hand_stored_presets_stay(self):
        records = u.parse(os_file(user=((TABLE, 0, 3, "", [5] * 18),)))     # stored on the device: no name
        u.bake(records, [(TABLE, "table", [preset(0, 0, "wave bass", [0] * 18), preset(0, 3, "sub organ")])])
        fixed = [preset(0, 0, "wave bass", [0] * 13 + [128, 100, 255, 0, 0]), preset(0, 3, "sub organ")]
        report = u.bake(records, [(TABLE, "table", fixed)])["table"]
        self.assertEqual(report, {"new": 0, "same": 0, "updated": 1, "replaced": 0, "kept": [(0, 3, "")]})
        loaded = os_load(u.serialise(records), {TABLE})[TABLE]
        self.assertEqual(loaded[(0, 0)]["values"][13], 128)
        self.assertEqual(loaded[(0, 3)]["values"], [5] * 18)

    def test_a_plug_without_record_gets_a_zeroed_free_one(self):
        records = u.parse(os_file())
        before = [bytearray(r) for r in records]
        u.bake(records, [(61017, "modal", [preset(1, 0, "glass")])])
        changed = u.changed_records(before, records)
        self.assertEqual(len(changed), 1)
        r = records[changed[0]]
        self.assertEqual(u.plug_id(r), 61017)
        self.assertEqual([p["name"] for p in u.user_presets(r)], ["glass"])
        self.assertEqual(r[u.PRESET:u.PRESET + 9], bytes(9), "stale bytes cleared")
        self.assertEqual(u.mask(r, u.MASK_AT, 1), 1)

    def test_only_our_range_and_bad_keys(self):
        records = u.parse(os_file())
        with self.assertRaisesRegex(u.FormatError, "TE's range"):
            u.bake(records, [(10, "analoga", [preset()])])
        with self.assertRaisesRegex(u.FormatError, "slot 0..3 and index 0..13"):
            u.bake(records, [(TABLE, "table", [preset(0, 14)])])

    def test_every_repo_engine_bakes(self):
        engines = [e for e in opz.catalogue() if e["presets"]]
        records = u.parse(os_file(user=()))
        u.bake(records, [(int(e["manifest"]["plug"]["id"]), e["name"], e["presets"]) for e in engines])
        loaded = os_load(u.serialise(records), {int(e["manifest"]["plug"]["id"]) for e in engines})
        for e in engines:
            with self.subTest(engine=e["name"]):
                self.assertEqual(len(loaded[int(e["manifest"]["plug"]["id"])]), len(e["presets"]))


def args(**kw):
    base = {"engine_dir": [], "disk": None, "file": None, "show": False, "restore": None, "overwrite_user": False,
            "dry_run": False, "yes": False}
    base.update(kw)
    return argparse.Namespace(**base)


class CliTests(unittest.TestCase):
    def setUp(self):
        self.disk = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.disk, "config"))
        self.path = os.path.join(self.disk, "config", "userPresets.dat")
        self.original = os_file()
        with open(self.path, "wb") as f:
            f.write(self.original)
        self.backups = tempfile.mkdtemp()
        self.table = os.path.join(opz.ENGINES_DIR, "table")
        self.logs = []

    def run_bake(self, **kw):
        return opz.bake_presets(args(disk=self.disk, **kw), log=self.logs.append, confirm=lambda q: True,
                                backups=self.backups)

    def current(self):
        with open(self.path, "rb") as f:
            return f.read()

    def test_show_and_dry_run_write_nothing(self):
        self.assertEqual(self.run_bake(show=True), 0)
        self.assertIn("2 plug records, 198 free; 1 with user presets", self.logs)
        self.assertEqual(os.listdir(self.backups), [])
        self.assertEqual(self.run_bake(engine_dir=[self.table], dry_run=True), 0)
        self.assertEqual(self.current(), self.original)
        self.assertTrue(any(l.startswith("plan: table: 19 new, 0 updated (from an earlier bake), 0 replaced, 0 "
                                         "already there; kept your stored slot 0 index 1 'my bass'") for l in self.logs), self.logs)
        self.assertIn("dry run: nothing written.", self.logs)

    def test_bake_backup_restore(self):
        self.assertEqual(self.run_bake(engine_dir=[self.table], yes=True), 0)
        baked = self.current()
        self.assertNotEqual(baked, self.original)
        loaded = os_load(baked, {TABLE})[TABLE]
        self.assertEqual(len(loaded), 20)
        self.assertTrue(any(l.startswith("Now eject the OP-Z disk") for l in self.logs))
        backup = os.path.join(self.backups, os.listdir(self.backups)[0])
        with open(os.path.join(backup, "userPresets.dat"), "rb") as f:
            self.assertEqual(f.read(), self.original)
        with open(os.path.join(backup, "backup.json")) as f:
            self.assertEqual(json.load(f)["size"], 351202)
        self.logs.clear()
        self.assertEqual(self.run_bake(engine_dir=[self.table], yes=True), 0)
        self.assertIn("nothing to change: these presets are already in the file as user presets.", self.logs)
        self.assertEqual(self.run_bake(restore=backup, yes=True), 0)
        self.assertEqual(self.current(), self.original)

    def test_refusals_leave_the_file_alone(self):
        with open(self.path, "wb") as f:
            f.write(self.original[:-10])
        self.assertEqual(self.run_bake(yes=True), 1)
        self.assertTrue(any("must be 351202 bytes" in l for l in self.logs))
        self.assertEqual(len(self.current()), len(self.original) - 10)
        os.remove(self.path)
        self.assertEqual(self.run_bake(yes=True), 1)
        self.assertTrue(any("store one (hold track" in l for l in self.logs))
        with open(self.path, "wb") as f:
            f.write(self.original)
        self.assertEqual(opz.bake_presets(args(disk=self.disk, engine_dir=[self.table]), log=self.logs.append,
                                          confirm=lambda q: False, backups=self.backups), 1)
        self.assertEqual(self.current(), self.original)

    def test_disk_is_found_when_the_name_differs(self):
        volumes = tempfile.mkdtemp()
        os.rename(self.disk, os.path.join(volumes, "OP-Z"))
        self.disk = os.path.join(volumes, "OP-Z")
        self.path = os.path.join(self.disk, "config", "userPresets.dat")
        real = opz.find_content_disks
        opz.find_content_disks = lambda: real(volumes)
        try:
            self.assertEqual(opz.bake_presets(args(disk=os.path.join(volumes, "OP-Z Disk"), show=True),
                                              log=self.logs.append, backups=self.backups), 0)
            self.assertTrue(any(l.startswith("no disk at ") and "using the mounted content-mode disk" in l for l in self.logs))
            os.makedirs(os.path.join(volumes, "OP-Z 1", "config"))
            self.logs.clear()
            self.assertEqual(opz.bake_presets(args(disk="/nowhere", show=True), log=self.logs.append), 1)
            self.assertTrue(any("Mounted OP-Z disks:" in l for l in self.logs))
        finally:
            opz.find_content_disks = real

    def test_the_macos_attribute_file_from_our_write_is_removed(self):
        real = opz._write_checked

        def write_like_macos(path, data):
            ok = real(path, data)
            with open(os.path.join(os.path.dirname(path), "._userPresets.dat"), "wb") as f:
                f.write(b"\x00\x05\x16\x07" + bytes(60))
            return ok
        opz._write_checked = write_like_macos
        try:
            self.assertEqual(self.run_bake(engine_dir=[self.table], yes=True), 0)
        finally:
            opz._write_checked = real
        self.assertFalse(os.path.exists(os.path.join(self.disk, "config", "._userPresets.dat")))
        self.assertIn("removed config/._userPresets.dat (macOS file attributes, written with this write)", self.logs)

    def test_main_entry_needs_no_device(self):
        with redirect_stdout(io.StringIO()) as out:
            self.assertEqual(opz.main(["presets", "bake", "--file", self.path, "--show"]), 0)
        self.assertIn("plug 61016: slot 0 index 1 'my bass'", out.getvalue())


if __name__ == "__main__":
    unittest.main()
