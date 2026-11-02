"""Offline tests for presets (patch.json plugs[]) and live sound ($0E) against the in-memory FakeOpz. Run:
    python3 -m unittest discover -s tools/tests
"""

import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_opz import DEFAULT_SOUND, FakeOpz  # noqa: E402
import opz  # noqa: E402
from opz import (BOOT_PATCH, Opz, boot_entries, build_presets_patch, catalogue, find_preset, preset_keys,  # noqa: E402
                 preset_keys_text, preset_problems, preset_slot)
from opz_session import SessionError, StateSession, decode_sound, sound_frame  # noqa: E402
from test_opz_cli import engine  # noqa: E402


def preset(slot=0, index=0, name="p", values=None, octave=0):
    return {"slot": slot, "index": index, "name": name, "default_octave": octave,
            "values": list(values or [7] * 18)}


class Virtual:
    """The vector recorder's clock model: a received frame takes 1 ms, an empty receive its timeout."""

    def __init__(self, device):
        self.device, self.now, self.sent = device, 0.0, []

    def clock(self):
        return self.now

    def send(self, raw):
        self.sent.append(raw)
        self.device.send(raw)

    def receive(self, timeout):
        raw = self.device.receive(timeout)
        self.now += timeout if raw is None else 0.001
        return raw


class Base(unittest.TestCase):
    def setUp(self):
        self.dev = FakeOpz()
        self.t = Virtual(self.dev)
        self.logs = []
        self.bdir = tempfile.mkdtemp()
        self.opz = Opz(self.t, log=self.logs.append, backups=self.bdir, confirm=lambda q: True, clock=self.t.clock)

    def record(self, pid=60950):
        return next(p for p in json.loads(self.dev.files["settings/plugs.json"])["plugs"] if p["id"] == pid)


class ProblemTests(unittest.TestCase):
    def test_clean_synth_presets(self):
        presets = [preset(s, 0, "s%d" % s) for s in range(4)]
        self.assertEqual(preset_problems(presets, 0xF0), ([], []))

    def test_errors(self):
        errors, _ = preset_problems([preset(values=[1] * 17), preset(slot=4, name="x"), preset(index=14, name="y"),
                                     preset(name="a"), preset(name="b")], 0)
        self.assertTrue(any("18 values" in e for e in errors))
        self.assertTrue(any("slot must be 0..3" in e for e in errors))
        self.assertTrue(any("index must be 0..13" in e for e in errors))
        self.assertTrue(any("share slot 0 index 0" in e for e in errors))

    def test_effect_slot_one_is_unreachable(self):
        _, warnings = preset_problems([preset(0, 0), preset(1, 0, "lead")], 0x300)
        self.assertEqual(len(warnings), 1)
        self.assertIn("slot 1 (lead) are never used", warnings[0])

    def test_effect_uses_preset_zero_only(self):
        _, warnings = preset_problems([preset(0, 0), preset(0, 1, "b"), preset(0, 2, "c")], 0x300)
        self.assertEqual(warnings, ["2 presets are past slot 0 index 0 never used: effect tracks (fx1, fx2) have no "
                                    "preset keys, only the preset applied when the plug is chosen"])
        self.assertEqual(preset_problems([preset(0, 0), preset(0, 1, "b")], 0x301)[1], [], "a drum track has keys")

    def test_missing_index_zero_and_long_name(self):
        _, warnings = preset_problems([preset(0, 1, "a twelve chr")], 0x10)
        self.assertTrue(any("keeps 11 characters" in w for w in warnings))
        self.assertTrue(any("no preset at slot 0 index 0: bass starts" in w for w in warnings))

    def test_lfo_depth_zero_is_flagged(self):
        v = [7] * 18
        v[13] = 0
        _, warnings = preset_problems([preset(values=v)], 0x10)
        self.assertTrue(any("LFO depth 0 is full negative depth" in w for w in warnings))
        v[13] = 128
        self.assertEqual(preset_problems([preset(values=v)], 0x10), ([], []))

    def test_preset_slot_per_track(self):
        self.assertEqual([preset_slot(t) for t in range(12)], [0, 0, 0, 0, 0, 1, 2, 3, 0, 0, 0, 0])

    def test_find_preset(self):
        presets = [preset(0, 0, "Deep"), preset(1, 2, "bright")]
        self.assertEqual(find_preset(presets, "deep")["name"], "Deep")
        self.assertEqual(find_preset(presets, slot=1, index=2)["name"], "bright")
        with self.assertRaises(opz.PlanError):
            find_preset(presets, "nope")

    def test_presets_patch_is_plugs_only(self):
        rec = {"engine": "engines/x.engine", "flags": 0, "id": 60950, "samples": "", "version": 3}
        doc = json.loads(build_presets_patch(rec, [preset(octave=-2)]))
        self.assertEqual(list(doc), ["plugs"])
        self.assertEqual(doc["plugs"][0]["version"], 3)
        self.assertEqual(doc["plugs"][0]["presets"][0]["defaultOctave"], -2)
        self.assertEqual(json.loads(build_presets_patch(rec, [preset()], nonce=5))["nonce"], 5)

    def test_install_reports_preset_problems(self):
        e = engine(mask=0x300, presets=[preset(1, 0)])
        errors, warnings, _ = opz.plan_install({"engines": [], "plugs": []}, {"tracks": []}, e, [8], None)
        self.assertEqual(errors, [])
        self.assertTrue(any("never used" in w for w in warnings))


class KeyTests(unittest.TestCase):
    """What the OP-Z lights: preset index n = white key n+1, per track's slot; FX tracks have no preset keys."""

    def test_synth_tracks_group_by_equal_sets(self):
        presets = [preset(s, i, "%d%d" % (s, i)) for s in range(4) for i in range(5 if s else 4)]
        self.assertEqual(preset_keys(presets, 0xF0), [([4], [0, 1, 2, 3]), ([5, 6, 7], [0, 1, 2, 3, 4])])
        self.assertEqual(preset_keys_text("table", presets, 0xF0),
                         "check on the OP-Z: hold the track with table chosen; lit white keys = presets (bass: white "
                         "keys 1-4; lead, arp, chord: white keys 1-5). Only white key 1 lit (it blinks after every "
                         "sound-slot change) = no presets there: opz.py presets push engines/table")

    def test_drums_read_slot_zero_and_fx_have_none(self):
        presets = [preset(0, 0), preset(0, 1, "b"), preset(0, 3, "d"), preset(1, 0, "lead")]
        self.assertEqual(preset_keys(presets, 0x30F), [([0, 1, 2, 3], [0, 1, 3])])
        self.assertIn("kick, snare, perc, sample: white keys 1-2, 4)", preset_keys_text("kit", presets, 0x30F))
        self.assertEqual(preset_keys_text("swirl", presets, 0x300), "")

    def test_no_trackmask_uses_the_filled_slots_and_lists_empty_tracks(self):
        self.assertEqual(preset_keys([preset(2, 13)], 0), [([6], [13])])
        self.assertIn("arp: white key 14)", preset_keys_text("x", [preset(2, 13)], 0))
        self.assertIn("(bass: white key 1; lead: none)", preset_keys_text("x", [preset(0, 0)], 0x30))


class PushTests(Base):
    def install(self, presets):
        self.assertTrue(self.opz.install(engine(presets=presets), [4], yes=True))

    def test_push_replaces_presets_without_version_bump(self):
        self.install([preset(0, 0, "old"), preset(0, 1, "keep")])
        before = self.record()
        self.assertTrue(self.opz.presets_push(engine(presets=[preset(0, 0, "new", [9] * 18)]), yes=True))
        self.assertEqual(self.record(), before)
        got = self.dev.presets[60950]
        self.assertEqual(got[(0, 0)]["name"], "new")
        self.assertEqual(got[(0, 1)]["name"], "keep", "re-import merges")
        backup = sorted(os.listdir(self.bdir))[-1]
        self.assertTrue(os.path.exists(os.path.join(self.bdir, backup, "presets.json")))

    def test_not_installed_writes_nothing(self):
        n = self.dev.sync_count
        self.assertFalse(self.opz.presets_push(engine(presets=[preset()]), yes=True))
        self.assertEqual(self.dev.sync_count, n)
        self.assertTrue(any("not installed" in line for line in self.logs))

    def test_dry_run_and_empty(self):
        self.install([preset()])
        n = self.dev.sync_count
        self.assertTrue(self.opz.presets_push(engine(presets=[preset(name="dry")]), dry_run=True))
        self.assertFalse(self.opz.presets_push(engine(presets=[]), yes=True))
        self.assertEqual(self.dev.sync_count, n)

    def test_kept_patch_is_resent_with_a_nonce(self):
        self.dev.keep_patch = True
        self.install([preset()])
        e = engine(presets=[preset(name="again")])
        self.assertTrue(self.opz.presets_push(e, yes=True))
        self.assertTrue(self.opz.presets_push(e, yes=True))
        self.assertTrue(any("with a nonce" in line for line in self.logs))
        self.assertIn("nonce", json.loads(self.dev.files["settings/patch.json"]))

    def test_restart_loses_presets_and_preset_zero_applies(self):
        self.install([preset(0, 0, "bass", list(range(18)), octave=-2)])
        self.assertTrue(self.dev.apply_preset(4, 60950))
        self.assertEqual(self.dev.sounds[4][:10], list(range(10)))
        self.assertEqual(self.dev.sounds[4][10], DEFAULT_SOUND[10], "pan is kept")
        self.assertFalse(self.dev.apply_preset(5, 60950), "lead reads slot 1, which is empty")
        self.dev.restart()
        self.assertFalse(self.dev.apply_preset(4, 60950))

    def test_install_and_push_say_presets_are_ram_only_and_which_keys_light(self):
        self.install([preset(0, 0), preset(0, 1, "b")])
        self.assertIn("The OP-Z keeps the presets this install sent in RAM only: a restart or crash drops them "
                      "(research/presets.md); then run: opz.py presets push engines/testeng", self.logs)
        self.assertTrue(any(line.startswith("check on the OP-Z") and "bass: white keys 1-2;" in line
                            for line in self.logs))
        self.logs.clear()
        self.assertTrue(self.opz.presets_push(engine(presets=[preset(0, 0)]), yes=True))
        self.assertIn("bass: white key 1;", self.logs[-1])

    def test_engine_update_says_the_restart_drops_the_presets(self):
        self.install([preset()])
        self.assertTrue(self.opz.install(engine(pver=2, ever=2, presets=[preset()]), [], yes=True))
        self.assertTrue(any(line.startswith("The restart also drops the presets this install sent")
                            and "after it, run: opz.py presets push engines/testeng" in line for line in self.logs))

    def test_install_without_presets_says_nothing_about_keys(self):
        self.install([])
        self.assertFalse(any("presets push" in line for line in self.logs))

    def test_auto_import_with_same_version_keeps_presets(self):
        self.install([preset(name="first")])
        self.assertFalse(self.opz.install(engine(presets=[preset(name="second")]), [], yes=True))
        self.assertEqual(self.dev.presets[60950][(0, 0)]["name"], "first")

    def test_device_rules_for_names_and_octaves(self):
        self.dev._import_patch({"plugs": [{"id": 60950, "version": 1, "engine": "engines/x.engine", "presets": [
            {"slot": 9, "index": 2, "name": "a very long name", "defaultOctave": 7, "values": [300] * 18},
            {"slot": 0, "index": 14, "name": "dropped", "values": [0] * 18}]}]})
        got = self.dev.presets[60950]
        self.assertEqual(list(got), [(3, 2)])
        self.assertEqual(got[(3, 2)]["name"], "a very long")
        self.assertEqual(got[(3, 2)]["defaultOctave"], 4)
        self.assertEqual(got[(3, 2)]["values"][0], 300 & 0xFF)


class SoundTests(Base):
    def test_frame_round_trip(self):
        raw = sound_frame(5, list(range(18)))
        self.assertEqual(raw[5], 0x0E)
        from opzsysex.sysex import decode_frame
        self.assertEqual(decode_sound(decode_frame(raw).payload), (5, list(range(18))))
        with self.assertRaises(ValueError):
            sound_frame(16, [0] * 18)

    def test_audition_restores(self):
        seen = []
        self.assertTrue(self.opz.audition(5, [1] * 18, lambda s: seen.append(list(self.dev.sounds[5])), yes=True))
        self.assertEqual(seen, [[1] * 18])
        self.assertEqual(self.dev.sounds[5], DEFAULT_SOUND)

    def test_audition_restores_after_an_error(self):
        def boom(s):
            raise KeyboardInterrupt
        with self.assertRaises(KeyboardInterrupt):
            self.opz.audition(5, [1] * 18, boom, yes=True)
        self.assertEqual(self.dev.sounds[5], DEFAULT_SOUND)

    def test_declined_audition_writes_nothing(self):
        self.opz.confirm = lambda q: False
        self.assertFalse(self.opz.audition(5, [1] * 18, lambda s: None))
        self.assertEqual(self.dev.sounds[5], DEFAULT_SOUND)
        self.assertNotIn(0x0E, [raw[5] for raw in self.t.sent])

    def test_set_sound_stays_and_logs_undo(self):
        self.assertTrue(self.opz.set_sound(4, [3] * 18, yes=True))
        self.assertEqual(self.dev.sounds[4], [3] * 18)
        self.assertTrue(any("to undo: opz.py presets set --track bass --values 128," in line for line in self.logs))

    def test_live_session_waits_for_expiry(self):
        self.opz.read_settings()                       # its hellos keep a state session live without a new burst
        self.assertTrue(self.opz.set_sound(4, [3] * 18, yes=True))
        self.assertTrue(any("waiting 6.5 s" in line for line in self.logs))

    def test_no_ack_is_an_error(self):
        class Mute(FakeOpz):
            def _on_00(self, p):
                pass
        t = Virtual(Mute())
        with self.assertRaises(SessionError):
            StateSession(t, clock=t.clock).open([4])

    def test_write_outside_a_session_is_ignored(self):
        self.dev.send(sound_frame(4, [9] * 18))
        self.assertEqual(self.dev.sounds[4], DEFAULT_SOUND)

    def test_watch_reports_preset_changes(self):
        self.dev.sound_burst = True                    # the notes' description: $0E on a change (not 1.2.45)
        self.opz.install(engine(presets=[preset(0, 0, "w", [5] * 18)]), [4], yes=True)
        s = StateSession(self.t, clock=self.t.clock)
        s.open([4])
        self.dev.apply_preset(4, 60950)
        self.assertIn((4, [5] * 10 + [DEFAULT_SOUND[10]] + [5] * 7), s.pump(1.0))


class BankTests(Base):
    """Firmware 1.2.45: no `$0E` in the burst; the restore points come from the bank, acked part by part (Q-44)."""

    def sent_ids(self):
        from opzsysex.sysex import decode_frame
        return [decode_frame(raw).message_id for raw in self.t.sent]

    def open(self, tracks=(4,)):
        self.logs.clear()
        s = StateSession(self.t, log=self.logs.append, clock=self.t.clock)
        s.open(tracks)
        return s

    def test_sounds_from_the_bank(self):
        self.dev.sounds[4] = list(range(18))
        self.dev.sounds[7] = [9] * 18
        s = self.open([4, 7])
        self.assertEqual(s.sounds, {t: self.dev.sounds[t] for t in range(16)})
        self.assertEqual(s.bank_address, 0x82)
        self.assertEqual(s.play_address, 0x82)
        self.assertNotIn(0x0E, s.seen, "1.2.45 sends no $0E")
        parts = s.seen[0x09] + s.seen[0x0A]
        self.assertGreater(parts, 2)
        self.assertEqual(self.sent_ids().count(0x0B), parts - 1, "every part but the last is acked")
        self.assertNotIn(0x08, self.sent_ids())
        self.assertIn("bank: project 9, pattern 3 (%d parts, " % parts, " ".join(self.logs))

    def test_bank_comes_from_the_active_project_and_pattern(self):
        import opz_project
        pr = opz_project.Project()
        pr.set_tempo(123)
        self.dev.files[opz_project.project_path(2)] = pr.to_bytes()
        self.dev.project, self.dev.pattern = 1, 11
        self.dev.sounds[5] = [77] * 18
        s = self.open([5])
        self.assertEqual(s.bank_address, 0x1B)
        self.assertEqual(s.sounds[5], [77] * 18)
        self.assertEqual(s.bank, bytes(self.dev.active_project().bank))

    def test_lost_part_is_asked_for_again(self):
        class Lossy(FakeOpz):
            dropped = False

            def _send_bank_part(self):
                super()._send_bank_part()
                if self.bank_out and self.bank_out["next"] == 3 and not self.dropped:
                    self.dropped = True
                    self.bank_out["next"] += 1        # part 3 never goes out: the next one is out of order
                    self._send_bank_part()
        self.t = Virtual(Lossy())
        s = self.open([4])
        self.assertEqual(s.sounds[4], DEFAULT_SOUND)
        self.assertEqual(self.sent_ids().count(0x08), 1)
        self.assertEqual(self.t.device.banks_sent, 2)
        self.assertIn("bank part 4 out of order: asking for it again", self.logs)

    def test_stalled_transfer_is_asked_for_again(self):
        class Stall(FakeOpz):
            ignored = False

            def _on_0b(self, p):
                if not self.ignored:
                    self.ignored = True
                    return
                super()._on_0b(p)
        self.t = Virtual(Stall())
        s = self.open([4])
        self.assertEqual(s.sounds[4], DEFAULT_SOUND)
        self.assertIn("bank transfer stalled: asking for it again", self.logs)

    def test_bad_stream_is_refused(self):
        class Garbage(FakeOpz):
            def _send_bank(self):
                self.bank_out = {"chunks": [bytes(178), b"\x01\x02"], "next": 0}
                self.banks_sent += 1
                self._send_bank_part()
        self.t = Virtual(Garbage())
        with self.assertRaises(SessionError):
            self.open([4])
        self.assertEqual(self.sent_ids().count(0x08), 2, "two re-requests per session, then it gives up")
        self.assertNotIn(0x0E, self.sent_ids())

    def test_pattern_changed_during_the_read_is_refused(self):
        class Moved(FakeOpz):
            def _play_state(self):
                state = bytearray(super()._play_state())
                state[17] += 1
                return bytes(state)
        self.t = Virtual(Moved())
        with self.assertRaises(SessionError) as e:
            self.open([4])
        self.assertIn("not the bank's", str(e.exception))

    def test_sound_burst_still_works(self):
        self.t = Virtual(FakeOpz(sound_burst=True))
        s = self.open([4])
        self.assertEqual(s.sounds[4], DEFAULT_SOUND)
        self.assertIsNone(s.bank)
        self.assertNotIn(0x0B, self.sent_ids())

    def test_audition_and_set_restore_from_the_bank(self):
        self.dev.sounds[5] = [42] * 18
        self.assertTrue(self.opz.audition(5, [1] * 18, lambda s: None, yes=True))
        self.assertEqual(self.dev.sounds[5], [42] * 18)

    def test_watch_sends_only_hellos_and_bank_reads(self):
        """The read-only device check for Q-44 runs `presets watch`: nothing but $00, $0B and $08 may go out."""
        self.assertTrue(self.opz.watch([7], 2.0))
        self.assertTrue(set(self.sent_ids()) <= {0x00, 0x0B, 0x08}, self.sent_ids())
        self.assertIn(0x0B, self.sent_ids())
        self.assertTrue(any(line.startswith("chord   P1=128") for line in self.logs))

    def test_runner_guard_allows_bank_reads_only(self):
        import opz_bench
        from opz_session import bank_ack_frame, bank_request_frame
        from opzsysex.sysex import frame
        g = opz_bench.Guard(self.t)
        g.send(bank_ack_frame(0, 0))
        g.send(bank_request_frame())
        for mid in (0x09, 0x0A, 0x0C, 0x07, 0x02):
            with self.assertRaises(opz.PlanError):
                g.send(frame(mid, bytes(8)))


class BootFileTests(Base):
    """settings/plugs_patch.json: imported at once and at the next start, which deletes it."""

    def setUp(self):
        super().setUp()
        self.eng = engine(presets=[preset(0, 0, "bass", list(range(18)), octave=-2), preset(1, 3, "lead")])
        self.assertTrue(self.opz.install(self.eng, [4], yes=True, boot=True))

    def boot_doc(self):
        return json.loads(self.dev.files[BOOT_PATCH])

    def test_persist_survives_one_restart(self):
        self.assertTrue(self.opz.persist_presets([self.eng], yes=True))
        doc = self.boot_doc()
        self.assertEqual(list(doc), ["plugs"], "plugs[] only: no auto_import, no engines[]")
        self.assertEqual([p["id"] for p in doc["plugs"]], [60950])
        entry = doc["plugs"][0]
        self.assertEqual({k: entry[k] for k in ("engine", "flags", "id", "samples", "version")}, self.record())
        self.dev.restart()
        self.assertNotIn(BOOT_PATCH, self.dev.files, "the start deletes it")
        self.assertTrue(self.dev.apply_preset(4, 60950))
        self.assertEqual(self.dev.presets[60950][(1, 3)]["name"], "lead")
        self.dev.restart()
        self.assertFalse(self.dev.apply_preset(4, 60950), "only one restart: the file is gone")

    def test_install_says_the_boot_file_brings_presets_back(self):
        self.assertTrue(any(line.startswith("The OP-Z keeps the presets this install sent in RAM; the boot file "
                                            "written next brings them back after a restart") for line in self.logs))
        self.assertFalse(any("then run: opz.py presets push" in line for line in self.logs))
        self.logs.clear()
        self.assertTrue(self.opz.presets_push(self.eng, yes=True, boot=True))
        self.assertTrue(any("the boot file written next" in line for line in self.logs))

    def test_plan_lists_plugs_and_backup_keeps_a_copy(self):
        self.assertTrue(self.opz.persist_presets([self.eng], yes=True))
        self.assertTrue(any(line.startswith("plan: boot file settings/plugs_patch.json (") and "testeng 60950 (2)" in line
                            for line in self.logs))
        backup = sorted(os.listdir(self.bdir))[-1]
        self.assertTrue(backup.endswith("-boot-presets"))
        with open(os.path.join(self.bdir, backup, "plugs_patch.json"), "rb") as f:
            self.assertEqual(f.read(), self.dev.files[BOOT_PATCH])

    def test_only_our_installed_plugs_with_presets(self):
        other = engine(name="notinstalled", pid=60951, presets=[preset()])
        moved = engine(name="moved", pid=60950, presets=[preset()])           # same id, other engine path
        te = engine(name="analoga", pid=10, presets=[preset()])
        plugs = json.loads(self.dev.files["settings/plugs.json"])
        entries, labels, warnings = boot_entries(plugs, [other, te, self.eng])
        self.assertEqual([e["id"] for e in entries], [60950])
        self.assertEqual(labels, ["testeng 60950 (2)"])
        entries, _, warnings = boot_entries(plugs, [moved, self.eng])
        self.assertEqual(entries, [], "the first engine with an id wins")
        self.assertIn("plug 60950 is engines/testeng.engine on the OP-Z, not engines/moved.engine", warnings[0])
        bad = engine(presets=[preset(values=[1] * 3)])
        self.assertEqual(boot_entries(plugs, [bad])[0], [])

    def test_remove_then_persist_does_not_bring_the_plug_back(self):
        self.assertTrue(self.opz.persist_presets([self.eng], yes=True))
        self.assertTrue(self.opz.remove(60950, unassign_first=True, yes=True))
        self.assertTrue(self.opz.persist_presets([self.eng], yes=True))
        self.assertEqual(self.boot_doc(), {"plugs": []})
        self.assertTrue(any(line.startswith("plan: empty boot file") for line in self.logs))
        self.dev.restart()
        self.assertFalse(any(p["id"] == 60950 for p in json.loads(self.dev.files["settings/plugs.json"])["plugs"]))

    def test_a_stale_boot_file_brings_a_removed_plug_back(self):
        """Why remove writes the boot file again: the OS re-creates a plug a plugs[] entry names."""
        self.assertTrue(self.opz.persist_presets([self.eng], yes=True))
        self.assertTrue(self.opz.remove(60950, unassign_first=True, yes=True))
        self.dev.restart()
        self.assertTrue(any(p["id"] == 60950 for p in json.loads(self.dev.files["settings/plugs.json"])["plugs"]))

    def test_off_dry_run_and_unchanged(self):
        n = self.dev.sync_count
        self.assertTrue(self.opz.persist_presets([self.eng], dry_run=True))
        self.assertEqual(self.dev.sync_count, n)
        self.assertNotIn(BOOT_PATCH, self.dev.files)
        self.assertTrue(self.opz.persist_presets([self.eng], yes=True))
        self.assertTrue(self.opz.persist_presets([self.eng], yes=True))
        self.assertTrue(any("already holds this boot file" in line for line in self.logs))
        self.assertTrue(self.opz.persist_presets([self.eng], yes=True, off=True))
        self.assertEqual(self.boot_doc(), {"plugs": []})
        self.dev.restart()
        self.assertFalse(self.dev.apply_preset(4, 60950))

    def test_declined_writes_nothing(self):
        self.opz.confirm = lambda q: False
        self.assertFalse(self.opz.persist_presets([self.eng]))
        self.assertNotIn(BOOT_PATCH, self.dev.files)

    def test_too_big_is_refused(self):
        many = [engine(name="e%d" % i, pid=60900 + i, presets=[preset(s, x, "p%d" % x) for s in range(4)
                                                                for x in range(14)]) for i in range(40)]
        plugs = json.loads(self.dev.files["settings/plugs.json"])
        plugs["plugs"] += [{"engine": e["device_path"], "flags": 0, "id": e["manifest"]["plug"]["id"], "samples": "",
                            "version": 1} for e in many]
        self.dev.files["settings/plugs.json"] = json.dumps(plugs).encode()
        n = self.dev.sync_count
        self.assertFalse(self.opz.persist_presets(many, yes=True))
        self.assertEqual(self.dev.sync_count, n)
        self.assertTrue(any("limit 131072" in line for line in self.logs))

    def test_record_change_is_reported(self):
        real = self.dev._import_patch

        def bump(doc):
            real(doc)
            plugs = json.loads(self.dev.files["settings/plugs.json"])
            plugs["plugs"][-1]["version"] += 1
            self.dev.files["settings/plugs.json"] = json.dumps(plugs).encode()
        self.dev._import_patch = bump
        self.assertFalse(self.opz.persist_presets([self.eng], yes=True))
        self.assertTrue(any(line.startswith("verify:  plug ") for line in self.logs))
        self.assertIn("to empty the boot file before the next start: opz.py presets persist --off", self.logs)

    def test_unparsable_boot_file_is_deleted_at_start(self):
        self.dev.files[BOOT_PATCH] = b"{not json"
        self.dev.restart()
        self.assertNotIn(BOOT_PATCH, self.dev.files)
        self.assertIn("Failed to import update file", self.dev.log)

    def test_catalogue(self):
        root = tempfile.mkdtemp()
        for name, pid in (("b", 60961), ("a", 60960)):
            os.makedirs(os.path.join(root, name))
            with open(os.path.join(root, name, "plug.json"), "w") as f:
                json.dump({"engine": name, "plug": {"id": pid}, "presets": "presets.json"}, f)
            with open(os.path.join(root, name, "presets.json"), "w") as f:
                json.dump([preset()], f)
        os.makedirs(os.path.join(root, "nomanifest"))
        got = catalogue(root, first=[self.eng])
        self.assertEqual([e["name"] for e in got], ["testeng", "a", "b"])


class CliTests(unittest.TestCase):
    def test_check_every_repo_engine(self):
        root = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "engines")
        for name in sorted(os.listdir(root)):
            if os.path.exists(os.path.join(root, name, "plug.json")):
                with self.subTest(engine=name), redirect_stdout(io.StringIO()) as out:
                    self.assertEqual(opz.main(["presets", "check", os.path.join(root, name)]), 0, out.getvalue())

    def test_check_reports_errors(self):
        d = tempfile.mkdtemp()
        with open(os.path.join(d, "plug.json"), "w") as f:
            json.dump({"engine": "x", "plug": {"id": 60999, "trackmask": 16}, "presets": "presets.json"}, f)
        with open(os.path.join(d, "presets.json"), "w") as f:
            json.dump([preset(values=[1] * 3)], f)
        with redirect_stdout(io.StringIO()) as out:
            self.assertEqual(opz.main(["preset", "check", d]), 1)
        self.assertIn("18 values", out.getvalue())

    def test_push_takes_several_engines(self):
        root = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "engines")
        with redirect_stdout(io.StringIO()) as out:
            self.assertEqual(opz.main(["--fake", "presets", "push", os.path.join(root, "table"),
                                       os.path.join(root, "swirl"), "--dry-run"]), 1)
        self.assertIn("plug 61016 is not installed", out.getvalue())
        self.assertIn("plug 61001 is not installed", out.getvalue())

    def test_fake_persist(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(opz, "ROOT", tmp):    # backups go to tmp
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(opz.main(["--fake", "presets", "persist", "--dry-run"]), 0)
            self.assertIn("plan: empty boot file settings/plugs_patch.json", out.getvalue())
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(opz.main(["--fake", "presets", "persist", "--off", "--yes"]), 0)
            self.assertIn("boot file emptied and verified.", out.getvalue())

    def test_fake_install_writes_the_boot_file_unless_no_boot(self):
        root = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "engines")
        blob = os.path.join(tempfile.mkdtemp(), "table.engine")
        with open(blob, "wb") as f:
            f.write(b"\x00\x50\x00\xad" * 64)
        args = ["--fake", "install", os.path.join(root, "table"), "--engine", blob, "--track", "bass", "--yes"]
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(opz, "ROOT", tmp):    # backups go to tmp
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(opz.main(args), 0)
            text = out.getvalue()
            self.assertIn("the boot file written next brings them back after a restart", text)
            self.assertIn("1 plug (presets): table 61016 (20)", text)
            self.assertIn("boot file written and verified", text)
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(opz.main(args + ["--no-boot"]), 0)
        self.assertNotIn("boot file", out.getvalue())
        self.assertIn("then run: opz.py presets push engines/table", out.getvalue())

    def test_repo_catalogue_fits_the_boot_file(self):
        engines = catalogue()
        records = [{"id": e["manifest"]["plug"]["id"], "engine": e["device_path"], "samples": "", "flags": 0,
                    "version": 1} for e in engines]
        entries, labels, warnings = boot_entries({"plugs": records}, engines)
        self.assertEqual(warnings, [])
        self.assertEqual(len(entries), sum(1 for e in engines if e["presets"]))
        self.assertLess(len(opz.build_boot_patch(entries)), opz.BOOT_PATCH_MAX // 2)

    def test_fake_audition_and_watch(self):
        with redirect_stdout(io.StringIO()):
            kit = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                               "engines", "kit")
            self.assertEqual(opz.main(["--fake", "presets", "audition", kit, "--preset", "analog", "--track", "kick",
                                       "--hold", "0.3", "--yes"]), 0)
            self.assertEqual(opz.main(["--fake", "presets", "set", "--track", "bass", "--yes",
                                       "--values", ",".join(["1"] * 18)]), 0)
            self.assertEqual(opz.main(["--fake", "presets", "watch", "--track", "bass", "--seconds", "1"]), 0)


if __name__ == "__main__":
    unittest.main()
