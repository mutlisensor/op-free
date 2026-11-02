"""Offline tests for the opz installer against the in-memory FakeOpz. Run:
    python3 -m unittest discover -s tools/tests
"""

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from fake_opz import FakeOpz  # noqa: E402
from opz import MASTER, Opz, PlanError, assigned_tracks, build_patch, device_presets, plan_install  # noqa: E402
from opz_session import FileSession, SessionError  # noqa: E402


def engine(name="testeng", pid=60950, pver=1, slot=86, ever=1, mask=0xF0, presets=None, data=b"\x00\x50\x00\xad" * 64,
           master=None):
    e = {"name": name, "data": data, "file": "<memory>", "device_path": "engines/%s.engine" % name,
         "presets": presets or [], "manifest": {
             "engine": name, "memoryslot": slot, "engine_version": ever,
             "plug": {"id": pid, "version": pver, "name": name, "trackmask": mask, "flags": 0,
                      "parameter_names": ["a", "b"]}}}
    if master is not None:
        e["manifest"]["master_in_place"] = master
    return e


class Base(unittest.TestCase):
    def setUp(self):
        self.dev = FakeOpz()
        self.logs = []
        self.opz = Opz(self.dev, log=self.logs.append, backups=tempfile.mkdtemp(), confirm=lambda q: True)

    def settings(self):
        return (json.loads(self.dev.files["settings/plugs.json"]),
                json.loads(self.dev.files["settings/slotConfiguration.json"]))


class InstallTests(Base):
    def test_install_first_free_slot_and_verify(self):
        e = engine()
        self.assertTrue(self.opz.install(e, [4], yes=True))
        plugs, slots = self.settings()
        self.assertEqual(self.dev.files["engines/testeng.engine"], e["data"])
        self.assertIn({"memoryslot": 86, "path": "engines/testeng.engine", "version": 1}, plugs["engines"])
        self.assertEqual(assigned_tracks(slots, 60950), [(4, 2)])

    def test_effect_on_two_fx_tracks(self):
        self.assertTrue(self.opz.install(engine(mask=0x300), [8, 9], yes=True))
        self.assertEqual(sorted(assigned_tracks(self.settings()[1], 60950)), [(8, 2), (9, 2)])

    def test_full_track_refused_nothing_written(self):
        self.assertFalse(self.opz.install(engine(), [5], yes=True))
        self.assertEqual(self.dev.sync_count, 0)
        self.assertTrue(any("no free slot" in l for l in self.logs))

    def test_specific_slot_with_replace_writes_slot_map(self):
        self.assertTrue(self.opz.install(engine(), [5], slot=3, replace=True, yes=True))
        self.assertIn((5, 3), assigned_tracks(self.settings()[1], 60950))

    def test_specific_slot_occupied_without_replace(self):
        self.assertFalse(self.opz.install(engine(), [5], slot=3, yes=True))
        self.assertEqual(self.dev.sync_count, 0)

    def test_dry_run_writes_nothing(self):
        self.assertTrue(self.opz.install(engine(), [4], dry_run=True))
        self.assertEqual(self.dev.sync_count, 0)

    def test_declined_confirmation_writes_nothing(self):
        self.opz.confirm = lambda q: False
        self.assertFalse(self.opz.install(engine(), [4]))
        self.assertEqual(self.dev.sync_count, 0)

    def test_reinstall_needs_higher_version(self):
        self.assertTrue(self.opz.install(engine(), [4], yes=True))
        self.assertFalse(any("RESTART" in l for l in self.logs))       # a new engine loads on import
        self.assertFalse(self.opz.install(engine(), [], yes=True))
        self.assertTrue(self.opz.install(engine(pver=2, ever=2, data=b"\x01" * 300), [], yes=True))
        self.assertTrue(any("RESTART THE OP-Z" in l for l in self.logs))  # a replaced engine needs a restart
        self.assertEqual(self.dev.files["engines/testeng.engine"], b"\x01" * 300)

    def test_verification_failure_restores_slot_map(self):
        before = self.dev.files["settings/slotConfiguration.json"]
        self.dev._import_patch = lambda doc: None          # device "ignores" the patch
        self.assertFalse(self.opz.install(engine(), [4], yes=True))
        self.assertEqual(json.loads(self.dev.files["settings/slotConfiguration.json"]), json.loads(before))
        self.assertTrue(any("restore verified" in l for l in self.logs))


class PlanTests(unittest.TestCase):
    def setUp(self):
        d = FakeOpz()
        self.plugs = json.loads(d.files["settings/plugs.json"])
        self.slots = json.loads(d.files["settings/slotConfiguration.json"])

    def errors(self, e, tracks=(4,), slot=None):
        return plan_install(self.plugs, self.slots, e, list(tracks), slot)[0]

    def test_memory_slot_collision(self):
        self.assertTrue(any("memory slot 79" in x for x in self.errors(engine(slot=79))))

    def test_te_engine_name_and_low_plug_id(self):
        errs = self.errors(engine(name="crush", pid=200))
        self.assertTrue(any("TE engine" in x for x in errs) and any("below 60000" in x for x in errs))

    def test_sim_only_engine_refused(self):
        e = engine()
        e["manifest"]["sim_only"] = True
        self.assertTrue(any("simulator-only" in x for x in self.errors(e)))
        self.assertEqual(self.errors(engine()), [])

    def test_slot_above_89_refused(self):
        self.assertTrue(any("outside" in x for x in self.errors(engine(slot=95))))

    def test_trackmask_is_only_a_warning(self):
        errors, warnings, _ = plan_install(self.plugs, self.slots, engine(mask=0xF0), [0], None)
        self.assertEqual(errors, [])
        self.assertTrue(any("trackmask" in w for w in warnings))

    def test_master_refused_without_master_in_place(self):
        for e in (engine(mask=0xF00), engine(mask=0xF00, master=False), engine(mask=0xF00, master="yes")):
            errors, _, plan = plan_install(self.plugs, self.slots, e, [MASTER], None)
            self.assertEqual(len(errors), 1)
            self.assertIn("cannot go on track master", errors[0])
            self.assertIn("out = NULL", errors[0])
            self.assertIn("master_in_place", errors[0])
            self.assertEqual(plan["assign"], [])
        self.assertTrue(self.errors(engine(mask=0xF00), (10, MASTER), slot=3))

    def test_master_allowed_with_master_in_place(self):
        errors, _, plan = plan_install(self.plugs, self.slots, engine(mask=0x800, master=True), [MASTER], None)
        self.assertEqual(errors, [])
        self.assertEqual(plan["assign"], [(MASTER, 1)])

    def test_synth_and_drum_plugs_only_on_tracks_1_to_8(self):
        for mask in (0xF0, 0xF, 0):
            for t in (8, 9, 10, MASTER):
                errors, _, plan = plan_install(self.plugs, self.slots, engine(mask=mask, master=True), [t], None)
                self.assertEqual(len(errors), 1, (mask, t))
                self.assertIn("is a synth or drum engine", errors[0])
                self.assertIn("Use tracks kick..chord (1-8)", errors[0])
                self.assertEqual(plan["assign"], [])
        self.assertEqual(self.errors(engine(mask=0xF0), (0, 7)), [])
        self.assertEqual(self.errors(engine(mask=0x300), (10,)), [])

    def test_tape_still_allowed(self):
        errors, _, plan = plan_install(self.plugs, self.slots, engine(mask=0x400), [10], None)
        self.assertEqual(errors, [])
        self.assertEqual(plan["assign"], [(10, 1)])

    def test_patch_shape(self):
        doc = json.loads(build_patch(engine(presets=[{"slot": 0, "index": 1, "name": "x", "default_octave": -1,
                                                       "values": [1] * 18}]), [4]))
        entry = doc["auto_import"][0]
        self.assertEqual(entry["tracks"], [4])
        self.assertEqual(entry["engine"], {"path": "engines/testeng.engine", "memoryslot": 86, "version": 1})
        self.assertEqual(entry["plug"]["presets"][0]["defaultOctave"], -1)
        for key in ("id", "version", "engine", "samples", "flags"):
            self.assertIn(key, entry["plug"])

    def test_plug_naming_a_pack_needs_that_pack_installed(self):
        e = engine(mask=0x0F)
        e["manifest"]["plug"]["samples"] = "samplepacks/opzk_testkit.aif"
        self.assertTrue(any("install the pack first" in x for x in self.errors(e, tracks=(0,))))
        self.plugs["plugs"].append({"id": 61400, "version": 1, "engine": "engines/sampleplayergain.engine",
                                    "samples": "samplepacks/opzk_testkit.aif", "flags": 0})
        self.assertEqual(self.errors(e, tracks=(0,)), [])
        e["manifest"]["plug"]["samples"] = "samplepacks/user/kick.aif"
        self.assertTrue(any("only name a pack of ours" in x for x in self.errors(e, tracks=(0,))))

    def test_preset_validation(self):
        with self.assertRaises(PlanError):
            device_presets([{"values": [0] * 17}])


class AssignTests(Base):
    def test_assign_installed_plug_to_drum_track(self):
        self.assertTrue(self.opz.install(engine(), [4], yes=True))
        self.assertTrue(self.opz.assign(60950, [0], yes=True))
        self.assertIn((0, 2), assigned_tracks(self.settings()[1], 60950))
        self.assertIn((4, 2), assigned_tracks(self.settings()[1], 60950))

    def test_assign_unknown_plug_refused(self):
        self.assertFalse(self.opz.assign(60999, [0], yes=True))

    def test_assign_to_master_needs_master_in_place(self):
        e = engine(mask=0xF00)
        self.assertTrue(self.opz.install(e, [8], yes=True))
        before = self.dev.sync_count
        self.assertFalse(self.opz.assign(60950, [MASTER], yes=True))
        self.assertFalse(self.opz.assign(60950, [MASTER], yes=True, engines=[e]))
        self.assertEqual(self.dev.sync_count, before)
        self.assertTrue(any("cannot go on track master" in x for x in self.logs))
        self.assertTrue(self.opz.assign(60950, [10], yes=True, engines=[e]))
        self.assertTrue(self.opz.assign(60950, [MASTER], yes=True, engines=[engine(mask=0xF00, master=True)]))
        self.assertIn((MASTER, 1), assigned_tracks(self.settings()[1], 60950))
        self.assertIn((10, 1), assigned_tracks(self.settings()[1], 60950))

    def test_assign_synth_to_effect_track_refused(self):
        e = engine()
        self.assertTrue(self.opz.install(e, [4], yes=True))
        before = self.dev.sync_count
        for t in (8, 10, MASTER):
            self.assertFalse(self.opz.assign(60950, [t], yes=True, engines=[e]))
            self.assertFalse(self.opz.assign(60950, [t], yes=True))
        self.assertEqual(self.dev.sync_count, before)
        self.assertIn("error:   plug 60950 cannot go on track tape: its trackmask 0xf0 names no effect track, so it is "
                      "a synth or drum engine, and what one does on fx1, fx2, tape or master is untested (it may crash "
                      "the OP-Z). Use tracks kick..chord (1-8)", self.logs)
        self.assertTrue(any("no local plug.json with that id names an effect track" in x for x in self.logs))

    def test_assign_full_track_refused(self):
        self.opz.install(engine(), [4], yes=True)
        before = self.dev.sync_count
        self.assertFalse(self.opz.assign(60950, [5], yes=True))
        self.assertEqual(self.dev.sync_count, before)


class RemoveRestoreTests(Base):
    def test_remove_assigned_needs_unassign(self):
        self.opz.install(engine(), [4], yes=True)
        self.assertFalse(self.opz.remove(60950, yes=True))
        self.assertTrue(self.opz.remove(60950, unassign_first=True, yes=True))
        plugs, slots = self.settings()
        self.assertFalse(any(p["id"] == 60950 for p in plugs["plugs"]))
        self.assertEqual(assigned_tracks(slots, 60950), [])
        self.assertTrue(any("engines/testeng.engine" in l for l in self.logs if l.startswith("removed")))

    def test_refuses_te_plug(self):
        self.assertFalse(self.opz.remove(10, unassign_first=True, yes=True))

    def test_restore_touch_forces_a_real_upload(self):
        bdir, _, slots = self.opz.backup()
        before = self.dev.sync_count
        self.assertTrue(self.opz.restore(bdir, yes=True, touch=True))
        self.assertTrue(any("wrote settings/slotConfiguration.json" in l for l in self.dev.log))
        self.assertEqual(self.settings()[1], slots)
        self.assertEqual(self.dev.sync_count, before + 1)

    def test_restore(self):
        bdir, _, slots = self.opz.backup()
        self.opz.install(engine(), [4], yes=True)
        self.assertTrue(self.opz.restore(bdir, yes=True))
        self.assertEqual(self.settings()[1], slots)


class SessionTests(unittest.TestCase):
    def test_unserved_path(self):
        s = FileSession(FakeOpz())
        s.open(timeout=2)
        with self.assertRaises(SessionError):
            s.pull("engines/crush.engine", part_timeout=0.2)

    def test_no_reply_without_hello(self):
        class Mute(FakeOpz):
            def _on_00(self, p):
                pass
        with self.assertRaises(SessionError):
            FileSession(Mute()).open(timeout=0.5)

    def test_unchanged_file_is_skipped(self):
        dev = FakeOpz()
        s = FileSession(dev)
        s.open(timeout=2)
        same = dev.files["settings/slotConfiguration.json"]
        result = s.push([("settings/slotConfiguration.json", same)], grace=0.2)
        self.assertEqual(result["skipped"], ["settings/slotConfiguration.json"])


if __name__ == "__main__":
    unittest.main()
