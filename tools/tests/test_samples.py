"""Sample packs: the OP-1 AIFF format, the kit builder, `opz.py samples` on the FakeOpz, and the guards that keep the
user's own samples safe. Run:
    python3 -m unittest discover -s tools/tests

The TE regression test reads TE's UserSampler.aif from the local, git-ignored firmware folder and is skipped when it
is absent.
"""

import array
import io
import json
import os
import struct
import sys
import tempfile
import unittest
import wave
import zlib
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import opz  # noqa: E402
import opz_samples as smp  # noqa: E402
from fake_opz import FakeOpz, default_files  # noqa: E402

TE_PACK = os.path.join(REPO, "decrypted", "firmware", "blackfin", "system", "content", "samplepacks",
                       "UserSampler.aif")
USER_KICKS = "samplepacks/user/ryantechnokicks-8336.aif"     # a path shape TE's app uses (id = crc32 of the path)
USER_SYNTH = "samplepacks/user/user216445511.aif"


def tone(n, level=8000):
    return array.array("h", ((level if (i // 50) % 2 else -level) for i in range(n)))


def user_files():
    """The default fake plus two user samples (flag 2): one in kick slot 3, one (a synth sample) in no slot."""
    files = default_files()
    plugs = json.loads(files["settings/plugs.json"])
    slots = json.loads(files["settings/slotConfiguration.json"])
    plugs["engines"].append({"memoryslot": 68, "path": "engines/synthsampler.engine", "version": 1})
    for path, eng in ((USER_KICKS, "sampleplayergain"), (USER_SYNTH, "synthsampler")):
        plugs["plugs"].append({"engine": "engines/%s.engine" % eng, "flags": 2, "id": zlib.crc32(path.encode()),
                               "samples": path, "version": 1})
        files[path] = b"FORM-user"
    slots["tracks"][0]["packs"].append({"id": zlib.crc32(USER_KICKS.encode()), "slot": 3})
    files["settings/plugs.json"] = json.dumps(plugs).encode()
    files["settings/slotConfiguration.json"] = json.dumps(slots).encode()
    return files


class Positions(unittest.TestCase):
    def test_every_frame_round_trips(self):
        for f in range(0, smp.DRUM_MAX_FRAMES, 997):
            p = smp.pos_from_frame(f)
            self.assertEqual(smp.frame_from_pos(p), f)
            self.assertEqual((p * smp.DRUM_MAX_FRAMES * 2) >> 32, f)    # a truncating multiply agrees
        self.assertEqual(smp.frame_from_pos(smp.pos_from_frame(smp.DRUM_MAX_FRAMES)), smp.DRUM_MAX_FRAMES)

    def test_scale_is_12_seconds(self):
        self.assertEqual(smp.DRUM_MAX_FRAMES, 12 * 44100)
        self.assertEqual(smp.frame_from_pos(1 << 30), 6 * 44100)


class Format(unittest.TestCase):
    def test_build_and_parse_round_trip(self):
        sounds = [tone(1000), tone(500, 3000), tone(2000)]
        data = smp.build_drum_pack(sounds, "rt")
        pack = smp.parse(data)
        self.assertEqual(pack["errors"], [])
        self.assertEqual((pack["kind"], pack["frames"], pack["channels"], pack["bits"], pack["rate"]),
                         ("drum", 3500, 1, 16, 44100.0))
        self.assertEqual(len(pack["keys"]), 24)
        for k, (s, e) in enumerate(pack["keys"]):
            self.assertEqual(pack["samples"][s:e], sounds[k % 3])         # keys repeat the sounds in order
        self.assertEqual(pack["meta"]["type"], "drum")
        self.assertEqual(len(pack["meta"]["volume"]), 24)

    def test_layout_matches_te(self):
        data = smp.build_drum_pack([tone(10)], "x")
        self.assertEqual(data[:4] + data[8:12], b"FORMAIFC")
        self.assertEqual(struct.unpack(">I", data[4:8])[0], len(data) - 8)
        self.assertIn(b"sowt", data)
        appl = data.index(b"APPL")
        self.assertEqual(data[appl + 8:appl + 12], b"op-1")
        self.assertEqual(struct.unpack(">I", data[appl + 4:appl + 8])[0] % 2, 0)

    def test_limits(self):
        with self.assertRaises(smp.PackError):
            smp.build_drum_pack([tone(10)] * 25, "too many")
        with self.assertRaises(smp.PackError):
            smp.build_drum_pack([tone(smp.DRUM_MAX_FRAMES), tone(1)], "too long")
        smp.build_drum_pack([tone(smp.DRUM_MAX_FRAMES)], "exactly 12 s")

    def test_parse_rejects_what_the_os_rejects(self):
        good = smp.build_drum_pack([tone(100)], "g")
        rate = bytearray(good)
        i = rate.index(b"COMM") + 16
        rate[i:i + 10] = smp._ext80(48000)
        self.assertTrue(any("sample rate" in e for e in smp.parse(bytes(rate))["errors"]))
        stereo = bytearray(good)
        i = stereo.index(b"COMM") + 8
        stereo[i:i + 2] = struct.pack(">h", 2)
        self.assertTrue(any("channels" in e for e in smp.parse(bytes(stereo))["errors"]))
        typ = good.replace(b'"type":"drum"', b'"type":"synth"')
        self.assertTrue(any("neither drum nor sampler" in e for e in smp.parse(typ)["errors"]))
        self.assertEqual(smp.parse(b"RIFF....WAVE")["errors"], ["not an AIFF file"])
        plain = smp.write_aiff(tone(100), None)
        self.assertEqual((smp.parse(plain)["kind"], smp.parse(plain)["errors"]), ("plain", []))

    def test_synth_sample_needs_eight_knobs(self):
        meta = {"type": "sampler", "knobs": [0] * 7}
        self.assertTrue(any("8 knobs" in e for e in smp.parse(smp.write_aiff(tone(100), meta))["errors"]))
        meta["knobs"].append(0)
        self.assertEqual(smp.parse(smp.write_aiff(tone(100), meta))["errors"], [])

    def test_test_kit(self):
        sounds = smp.test_kit_sounds()
        self.assertEqual(len(sounds), 24)
        total = sum(len(s) for s in sounds)
        self.assertLess(total, 4 * 44100)
        self.assertEqual(sounds, smp.test_kit_sounds())                    # deterministic
        self.assertTrue(all(max(abs(v) for v in s) > 1000 for s in sounds))   # every key audible
        self.assertEqual(smp.parse(smp.build_drum_pack(sounds, "kit"))["errors"], [])

    def test_read_wav_mixes_and_resamples(self):
        path = os.path.join(tempfile.mkdtemp(), "s.wav")
        with wave.open(path, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(22050)
            w.writeframes(struct.pack("<4h", 1000, 3000, 1000, 3000))
        pcm, notes = smp.read_wav(path)
        self.assertEqual(list(pcm), [2000] * 4)
        self.assertEqual(len(notes), 2)

    @unittest.skipUnless(os.path.exists(TE_PACK), "TE content package not present")
    def test_te_user_sampler_parses_like_the_os(self):
        with open(TE_PACK, "rb") as f:
            pack = smp.parse(f.read())
        self.assertEqual((pack["kind"], pack["errors"], pack["frames"]), ("drum", [], 8 * 44100))
        self.assertEqual(len(pack["keys"]), 24)
        self.assertEqual(pack["keys"][-1][1], 8 * 44100)


class Rules(unittest.TestCase):
    def test_user_plug_id_is_crc_of_path(self):
        self.assertEqual(smp.pack_id(USER_KICKS), 717146855)    # a plug id on the user's OP-Z
        self.assertEqual(smp.pack_id(""), 0)

    def test_pack_path(self):
        self.assertEqual(smp.pack_path("Test Kit!"), "samplepacks/opzk_test-kit.aif")
        with self.assertRaises(smp.PackError):
            smp.pack_path("!!!")
        with self.assertRaises(smp.PackError):
            smp.pack_path("x" * 80)

    def test_removal_block(self):
        plugs = json.loads(user_files()["settings/plugs.json"])
        by_id = {p["id"]: p for p in plugs["plugs"]}
        self.assertIn("user sample", smp.removal_block(by_id[717146855], plugs))
        self.assertIn("did not install", smp.removal_block(by_id[121], plugs))
        self.assertIsNone(smp.removal_block(by_id[10], plugs))                 # an engine plug without samples
        ours = {"id": 61400, "samples": smp.OUR_PREFIX + "a.aif", "flags": 0}
        self.assertIsNone(smp.removal_block(ours, {"plugs": [ours]}))
        twin = dict(ours, id=61401)
        self.assertIn("also use", smp.removal_block(ours, {"plugs": [ours, twin]}))


class Base(unittest.TestCase):
    files = staticmethod(default_files)

    def setUp(self):
        self.dev = FakeOpz(self.files())
        self.logs = []
        self.backups = tempfile.mkdtemp()
        self.opz = opz.Opz(self.dev, log=self.logs.append, backups=self.backups, confirm=lambda q: True,
                           clock=lambda: self.dev.now)        # the fake's own time: no real waiting
        # small, so the paced file transfer stays quick (the full test kit is checked in Format)
        self.kit = smp.build_drum_pack([tone(60 + 10 * i) for i in range(24)], "opzk testkit")

    def settings(self):
        return (json.loads(self.dev.files["settings/plugs.json"]),
                json.loads(self.dev.files["settings/slotConfiguration.json"]))

    def logged(self, text):
        return any(text in line for line in self.logs)


class InstallTests(Base):
    def test_install_first_free_drum_slot(self):
        self.assertTrue(self.opz.samples_install(self.kit, "testkit", [0], yes=True))
        plugs, slots = self.settings()
        rec = next(p for p in plugs["plugs"] if p["id"] == 61400)
        self.assertEqual(rec, {"engine": smp.DRUM_ENGINE, "flags": 0, "id": 61400,
                               "samples": "samplepacks/opzk_testkit.aif", "version": 1})
        self.assertEqual(self.dev.files["samplepacks/opzk_testkit.aif"], self.kit)
        self.assertEqual(opz.assigned_tracks(slots, 61400), [(0, 2)])
        self.assertEqual(self.dev.sync_count, 3)              # pack, probe, patch.json
        self.assertTrue(self.logged("sync 2 (probe): the OP-Z holds the pack"))
        bdir = next(os.path.join(self.backups, d) for d in os.listdir(self.backups) if d.endswith("samples-testkit"))
        with open(os.path.join(bdir, "opzk_testkit.aif"), "rb") as f:
            self.assertEqual(f.read(), self.kit)

    def test_patch_repeats_te_engine_unchanged(self):
        before = self.settings()[0]["engines"]
        self.assertTrue(self.opz.samples_install(self.kit, "testkit", [1], yes=True))
        key = lambda e: e["path"]
        self.assertEqual(sorted(self.settings()[0]["engines"], key=key), sorted(before, key=key))

    def test_pack_lands_before_the_plug_names_it(self):
        order = []
        real = self.dev._import_patch

        def spy(doc):
            order.append("samplepacks/opzk_testkit.aif" in self.dev.files)
            real(doc)
        self.dev._import_patch = spy
        self.assertTrue(self.opz.samples_install(self.kit, "testkit", [0], yes=True))
        self.assertEqual(order, [True])

    def test_lost_pack_stops_before_patch(self):
        real = self.dev._differs
        self.dev._differs = lambda e: True if e["path"].startswith("samplepacks/") else real(e)
        self.assertFalse(self.opz.samples_install(self.kit, "testkit", [0], yes=True))
        self.assertTrue(self.logged("sync 1 did not land"))
        self.assertFalse(any(p["id"] == 61400 for p in self.settings()[0]["plugs"]))

    def test_dry_run_and_cancel_write_nothing(self):
        self.assertTrue(self.opz.samples_install(self.kit, "testkit", [0], dry_run=True))
        self.opz.confirm = lambda q: False
        self.assertFalse(self.opz.samples_install(self.kit, "testkit", [0]))
        self.assertEqual(self.dev.sync_count, 0)

    def test_update_needs_and_gets_a_version_bump(self):
        self.assertTrue(self.opz.samples_install(self.kit, "testkit", [0], yes=True))
        kit2 = smp.build_drum_pack([tone(100)], "v2")
        self.assertFalse(self.opz.samples_install(kit2, "testkit", [0], version=1, yes=True))
        self.assertTrue(self.opz.samples_install(kit2, "testkit", [0], yes=True))
        rec = next(p for p in self.settings()[0]["plugs"] if p["id"] == 61400)
        self.assertEqual(rec["version"], 2)
        self.assertEqual(self.dev.files["samplepacks/opzk_testkit.aif"], kit2)

    def test_refusals(self):
        cases = [
            (dict(tracks=[4]), "plays on kick"),                                  # drum pack on a synth track
            (dict(tracks=[0], plug_id=61000), "outside our sample range"),
            (dict(tracks=[0], slot=1), "holds plug 121"),
            (dict(tracks=[0], version=200), "version must be"),
        ]
        for kw, text in cases:
            self.logs.clear()
            tracks = kw.pop("tracks")
            self.assertFalse(self.opz.samples_install(self.kit, "testkit", tracks, yes=True, **kw), text)
            self.assertTrue(self.logged(text), (text, self.logs))
        bad = smp.build_drum_pack([tone(100)], "x").replace(b'"type":"drum"', b'"type":"synth"')
        self.assertFalse(self.opz.samples_install(bad, "testkit", [0], yes=True))
        self.assertEqual(self.dev.sync_count, 0)

    def test_replace_a_factory_slot_is_allowed(self):
        self.assertTrue(self.opz.samples_install(self.kit, "testkit", [0], slot=1, replace=True, yes=True))
        plugs, slots = self.settings()
        self.assertIn((0, 1), opz.assigned_tracks(slots, 61400))
        self.assertTrue(any(p["id"] == 121 for p in plugs["plugs"]))     # TE's pack stays installed

    def test_remove_ours_deletes_its_pack(self):
        self.assertTrue(self.opz.samples_install(self.kit, "testkit", [0], yes=True))
        self.assertTrue(self.opz.samples_remove(61400, unassign_first=True, yes=True))
        self.assertNotIn("samplepacks/opzk_testkit.aif", self.dev.files)


class Q25Tests(Base):
    """The pieces of hardware check H-S1 (Q-25): tiny kit, spare slot only, read-back, purge of an orphan pack."""

    def test_tiny_kit(self):
        sounds = smp.tiny_kit_sounds()
        data = smp.build_drum_pack(sounds, "testkit")
        pack = smp.parse(data)
        self.assertEqual(pack["errors"], [])
        self.assertEqual(len(pack["keys"]), 24)
        self.assertLess(len(data), 100_000)
        self.assertEqual(data, smp.build_drum_pack(smp.tiny_kit_sounds(), "testkit"))   # deterministic

    def test_occupied_slot_refused_without_replace(self):
        self.assertFalse(self.opz.samples_install(self.kit, "testkit", [0], slot=1, yes=True))
        self.assertTrue(self.logged("holds plug 121"))
        self.assertEqual(self.dev.sync_count, 0)
        self.assertTrue(self.opz.samples_install(self.kit, "testkit", [0], slot=7, yes=True))
        self.assertEqual(opz.assigned_tracks(self.settings()[1], 61400), [(0, 7)])

    def test_purge_an_orphan_pack(self):
        path = "samplepacks/opzk_testkit.aif"
        self.dev.files[path] = self.kit                         # an install that stopped after sync 1
        before = self.settings()
        self.assertTrue(self.opz.samples_purge("testkit", dry_run=True))
        self.assertEqual(self.dev.sync_count, 0)
        self.assertTrue(self.opz.samples_purge("testkit", yes=True))
        self.assertNotIn(path, self.dev.files)
        plugs, slots = self.settings()
        self.assertFalse(any(p["id"] == 61400 for p in plugs["plugs"]))
        self.assertEqual(slots, before[1])
        self.assertTrue(self.logged("purged " + path))

    def test_purge_refuses_a_named_pack(self):
        self.assertTrue(self.opz.samples_install(self.kit, "testkit", [0], yes=True))
        n = self.dev.sync_count
        self.assertFalse(self.opz.samples_purge("testkit", yes=True))
        self.assertTrue(self.logged("remove the plug instead"))
        self.assertEqual(self.dev.sync_count, n)
        self.assertIn("samplepacks/opzk_testkit.aif", self.dev.files)

    def test_purge_stops_when_the_placeholder_does_not_land(self):
        real = self.dev._differs
        self.dev._differs = lambda e: True if e["path"].startswith("samplepacks/") else real(e)
        self.assertFalse(self.opz.samples_purge("testkit", yes=True))
        self.assertTrue(self.logged("does not hold it"))
        self.assertFalse(any(p["id"] == 61400 for p in self.settings()[0]["plugs"]))

    def test_build_cli_tiny(self):
        out = os.path.join(tempfile.mkdtemp(), "q25.aif")
        with redirect_stdout(io.StringIO()) as buf:
            self.assertEqual(opz.main(["samples", "build", out, "--test-kit", "tiny", "--name", "testkit"]), 0)
        self.assertIn("0.96 s", buf.getvalue())


class SyncJobRule(Base):
    """Q-49: what the OP-Z requests and keeps (FUN_8012bb00, FUN_80129454, FUN_80127a24), and the tools' guards."""

    def unguarded(self):
        """Let a two-level path through the host's guard, to show what the device would do with it."""
        import opz_session
        real = opz_session.check_upload_paths
        opz_session.check_upload_paths = lambda files: None
        self.addCleanup(setattr, opz_session, "check_upload_paths", real)

    def test_identical_one_level_file_is_skipped(self):
        path = smp.OUR_PREFIX + "probe.aif"
        self.assertEqual(self.opz.push([(path, self.kit)])["requested"], [path])
        self.assertEqual(self.dev.files[path], self.kit)
        self.assertEqual(self.opz.push([(path, self.kit)])["skipped"], [path])      # the probe: proof it is there

    def test_the_2026_11_01_run_two_levels_deep(self):
        """The first H-S1 run: requested on both syncs, kept on neither, and the sync still reported complete."""
        self.unguarded()
        path = smp.LEGACY_PREFIX + "testkit.aif"
        for _ in range(2):
            self.assertEqual(self.opz.push([(path, self.kit)])["requested"], [path])
        self.assertNotIn(path, self.dev.files)
        self.assertEqual(self.dev.sync_count, 2)
        self.assertTrue(any("invalid file crc" in line for line in self.dev.log))

    def test_two_levels_land_once_the_inbox_folder_exists(self):
        self.unguarded()
        self.dev.inbox_dirs.add("samplepacks")         # left by an earlier one-level samplepacks/x upload
        path = smp.LEGACY_PREFIX + "testkit.aif"
        self.opz.push([(path, self.kit)])
        self.assertEqual(self.dev.files[path], self.kit)
        self.assertEqual(self.opz.push([(path, self.kit)])["skipped"], [path])

    def test_a_one_level_upload_makes_its_inbox_folder(self):
        self.opz.push([(smp.OUR_PREFIX + "a.aif", self.kit)])
        self.assertIn("samplepacks", self.dev.inbox_dirs)

    def test_crc_zero_is_always_requested(self):
        self.dev.files["x/y"] = b""
        self.assertTrue(self.dev._differs({"path": "x/y", "crc": 0}))
        self.assertFalse(self.dev._differs({"path": "samplepacks/TeKicks.aif",
                                            "crc": zlib.crc32(b"FORM-fake")}))

    def test_host_refuses_two_level_paths_before_sending(self):
        import opz_session
        with self.assertRaises(opz_session.SessionError):
            self.opz.push([(smp.LEGACY_PREFIX + "testkit.aif", self.kit)])
        self.assertEqual(self.dev.sync_count, 0)

    def test_install_refuses_the_old_location(self):
        plugs, slots = self.settings()
        errors, _, _ = opz.plan_samples_install(plugs, slots, smp.parse(self.kit), smp.LEGACY_PREFIX + "x.aif", "x",
                                                [0])
        self.assertTrue(any("outside samplepacks/opzk_" in e for e in errors))
        self.assertTrue(any("two folders deep" in e for e in errors))
        self.assertIsNone(smp.upload_path_error(smp.pack_path("x")))


class UserSampleGuards(Base):
    files = staticmethod(user_files)

    def test_readback_flags_a_user_sample_that_lost_its_slot(self):
        plugs, slots = self.settings()
        _, _, plan = opz.plan_samples_install(plugs, slots, smp.parse(self.kit), smp.OUR_PREFIX + "x.aif", "x", [0])
        after = opz.unassign(slots, 717146855)
        problems = opz.verify_samples_install(plugs, after, plan, before=slots)
        self.assertTrue(any("717146855" in p for p in problems))

    def test_generic_remove_refuses_user_samples(self):
        before = dict(self.dev.files)
        for pid in (717146855, zlib.crc32(USER_SYNTH.encode())):
            self.assertFalse(self.opz.remove(pid, unassign_first=True, yes=True))
        self.assertFalse(self.opz.samples_remove(717146855, unassign_first=True, yes=True))
        self.assertEqual(self.dev.files[USER_KICKS], before[USER_KICKS])
        self.assertEqual(self.dev.sync_count, 0)

    def test_name_clash_with_a_user_path_refused(self):
        plugs, slots = self.settings()
        pack = smp.parse(self.kit)
        errors, _, _ = opz.plan_samples_install(plugs, slots, pack, smp.OUR_PREFIX + "ryantechnokicks-8336.aif",
                                                "x", [0])
        self.assertEqual(errors, [])
        plugs["plugs"].append({"engine": smp.DRUM_ENGINE, "flags": 2, "id": 5, "samples": smp.OUR_PREFIX + "x.aif",
                               "version": 1})
        errors, _, _ = opz.plan_samples_install(plugs, slots, pack, smp.OUR_PREFIX + "x.aif", "x", [0])
        self.assertTrue(any("already used" in e for e in errors))

    def test_never_displace_a_user_sample(self):
        self.assertFalse(self.opz.samples_install(self.kit, "testkit", [0], slot=3, replace=True, yes=True))
        self.assertTrue(self.logged("leave user sample 717146855"))
        self.assertFalse(self.opz.assign(10, [0], slot=3, replace=True, yes=True))
        self.assertEqual(self.dev.sync_count, 0)

    def test_engine_install_cannot_displace_a_user_sample(self):
        e = {"name": "testeng", "data": b"\x00" * 64, "file": "<memory>", "device_path": "engines/testeng.engine",
             "presets": [], "manifest": {"engine": "testeng", "memoryslot": 86, "engine_version": 1,
                                         "plug": {"id": 60950, "version": 1, "trackmask": 1, "flags": 0}}}
        self.assertFalse(self.opz.install(e, [0], slot=3, replace=True, yes=True))
        self.assertTrue(self.logged("leave user sample"))

    def test_status_and_list_warn_about_loose_user_samples(self):
        plugs, slots = self.settings()
        self.assertIn("WARNING: user samples in no slot", opz.describe(plugs, slots))
        text = opz.describe_samples(plugs, slots)
        self.assertIn("NO SLOT", text)
        self.assertIn("the user's own samples (never touched): 2", text)

    def test_content_mode_model_shows_why(self):
        self.assertTrue(self.opz.samples_install(self.kit, "testkit", [0], yes=True))
        self.dev.files["samplepacks/user/stray.aif"] = b"FORM"
        gone = self.dev.content_mode()
        self.assertEqual(gone, sorted([USER_SYNTH, "samplepacks/user/stray.aif"]))   # the loose user sample is lost
        self.assertIn(USER_KICKS, self.dev.files)                            # assigned: kept
        self.assertIn("samplepacks/opzk_testkit.aif", self.dev.files)   # ours (flag 0, named by a plug): kept


class Cli(unittest.TestCase):
    def run_cli(self, *argv):
        out = io.StringIO()
        with redirect_stdout(out):
            code = opz.main(list(argv))
        return code, out.getvalue()

    def test_build_check(self):
        d = tempfile.mkdtemp()
        path = os.path.join(d, "kit.aif")
        code, out = self.run_cli("samples", "build", path, "--test-kit")
        self.assertEqual(code, 0, out)
        code, out = self.run_cli("samples", "check", path)
        self.assertEqual(code, 0, out)
        self.assertIn("key 24", out)
        with open(path, "r+b") as f:
            f.seek(0)
            f.write(b"JUNK")
        self.assertEqual(self.run_cli("samples", "check", path)[0], 1)

    def test_build_from_wav_dir(self):
        d = tempfile.mkdtemp()
        for i in range(3):
            with wave.open(os.path.join(d, "%02d.wav" % i), "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(44100)
                w.writeframes(tone(200 + i).tobytes())
        out = os.path.join(d, "kit.aif")
        code, text = self.run_cli("samples", "build", out, "--dir", d)
        self.assertEqual(code, 0, text)
        with open(out, "rb") as f:
            pack = smp.parse(f.read())
        self.assertEqual([e - s for s, e in pack["keys"][:3]], [200, 201, 202])


if __name__ == "__main__":
    unittest.main()
