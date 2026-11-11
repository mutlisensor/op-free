#!/usr/bin/env python3
"""opz — install, remove and back up custom OP-Z engines over USB/BLE MIDI.

Usage:
    opz.py new     NAME --kind synth|drum|fx [--size auto|5|10|50] [--slot N] [--plug-id ID] [--dry-run]
                   start an engine from an SDK template (offline; tools/opz_new.py, sdk/docs/QUICKSTART.md)
    opz.py ports
    opz.py backup  [--out backups/]
    opz.py status  [--from BACKUP_DIR]
    opz.py install ENGINE_DIR [--track lead ...] [--slot N] [--engine FILE] [--no-boot] [--dry-run] [--yes]
    opz.py assign  PLUG_ID --track kick [--slot N] [--replace] [--dry-run] [--yes]
    opz.py remove  PLUG_ID [--unassign] [--no-boot] [--dry-run] [--yes]
    opz.py restore BACKUP_DIR [--yes]
    opz.py presets check ENGINE_DIR
    opz.py presets push  ENGINE_DIR [ENGINE_DIR ...] [--no-boot] [--dry-run] [--yes]
    opz.py presets persist [ENGINE_DIR ...] [--off] [--dry-run] [--yes]
    opz.py presets bake  [ENGINE_DIR ...] (--disk VOLUME | --file PATH) [--show] [--restore FILE] [--overwrite-user]
                         [--dry-run] [--yes]
    opz.py presets audition ENGINE_DIR --track lead (--preset NAME | --slot S --index N) [--hold SECONDS] [--yes]
    opz.py presets set   --track lead (--values V0,...,V17 | ENGINE_DIR --preset NAME) [--dry-run] [--yes]
    opz.py presets watch [--track lead ...] [--seconds N]
    opz.py samples build OUT.aif (--wav FILE ... | --dir DIR | --test-kit [tiny]) [--name NAME]
    opz.py samples check FILE.aif
    opz.py samples list  [--from BACKUP_DIR]
    opz.py samples install FILE.aif --name NAME --track kick [--slot N] [--replace] [--plug-id ID] [--version N]
                           [--dry-run] [--yes]
    opz.py samples remove PLUG_ID [--unassign] [--dry-run] [--yes]
    opz.py projects backup [N ...] [--out backups/]
    opz.py projects put N FILE.opz [--overwrite] [--dry-run] [--yes]
    opz.py samples purge NAME [--dry-run] [--yes]
    opz.py test ENGINE_DIR --track lead [--script auto|default|PATH] [--record DIR] [--dry-run] [--engine-confirmed]
                [--yes] [...]          automated device test (tools/opz_bench.py --help)

Global options: --port NAME (default "OP-Z"), --fake (talk to an in-memory OP-Z instead of hardware).

Every write starts with a backup of settings/plugs.json and settings/slotConfiguration.json (backups/<time>/),
refuses unsafe plans, asks for confirmation (unless --yes), verifies the result by reading both files back through a
fresh session, and restores the slot map if verification fails. `presets` (alias `preset`): presets go to the OP-Z in
a patch.json `plugs[]` entry (no version bump) and live in its RAM; `install`, `presets push` and `remove` then write
the boot file settings/plugs_patch.json (`presets persist`; skip with --no-boot), which the OP-Z imports at once and
again at its next start, then deletes: the presets of every plug of ours in engines/ survive one restart.
`audition`/`set`/`watch` use the StateSync `$0E` message (one track's 18 sound bytes in the active pattern); the
current sounds come from the pattern bank the OP-Z sends at the session's start (`$0B` acks, read-only). See
research/presets.md and patterns.md §5. `presets bake` works offline on the content-mode disk: it writes our presets into
config/userPresets.dat as user presets, which the OP-Z imports on eject and then keeps by itself (no computer needed
afterwards). Close the official OP-Z app first: two clients fighting over the session breaks both. See SAFETY.md.

`samples`: OP-1 format drum/synth packs (research/samples.md). Packs go to samplepacks/opzk_<name>.aif (one folder
deep, like TE's factory packs: the file service can't write into samplepacks/user/, Q-49) with a plug of ours (61400-61499) on TE's sample player. The user's own samples (flag 2, or any pack we did not write) are
never removed, overwritten or left unassigned: the OP-Z deletes a removed plug's sample file, and content mode
deletes unassigned user samples. `samples purge` deletes a pack of ours that no plug names (left by an install that
stopped half way): it names it with a temporary plug of ours and removes that plug.

`projects`: project files projects/project01.opz..project10.opz over the normal-mode file service (no content mode).
`backup` reads them (what the OP-Z last saved); `put` writes one checked file (tools/opz_project.py check), which the
OP-Z reloads at once: it backs up all ten first, refuses a project that has notes unless --overwrite, reads the file
back and puts the backup back if it differs. See research/patterns.md.

Live MIDI needs `pip install mido python-rtmidi`. Python 3 standard library otherwise.
"""

from __future__ import annotations

import argparse
import array
import contextlib
import datetime
import hashlib
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from opz_session import FileSession, MidoTransport, SessionError, StateSession  # noqa: E402
import opz_userpresets as userpresets  # noqa: E402
import opz_samples as smp  # noqa: E402
import opz_project as proj  # noqa: E402
import opz_restorecheck as rcheck  # noqa: E402
import opz_new  # noqa: E402

PLUGS = "settings/plugs.json"
SLOTS = "settings/slotConfiguration.json"
PATCH = "settings/patch.json"
BOOT_PATCH = "settings/plugs_patch.json"   # imported at boot, then deleted (research/presets.md, Persistence)
BOOT_PATCH_MAX = 128 * 1024                # the OS reads at most 0x40000 bytes of an import file; stay well below
ENGINES_DIR = os.path.join(ROOT, "engines")

TRACKS = {"kick": 0, "snare": 1, "perc": 2, "sample": 3, "bass": 4, "lead": 5, "arp": 6, "chord": 7,
          "fx1": 8, "fx2": 9, "tape": 10, "master": 11}
TRACK_NAMES = {v: k for k, v in TRACKS.items()}
TE_ENGINES = {"analoga", "chord", "chorus", "chorus2", "cluster", "crush", "delay", "digital", "dist", "electric", "ep",
              "fdnreverb", "fm003b", "ministringshort", "organ", "reverb", "sampleplayergain", "saw013a",
              "synthsampler", "tapetwo", "volt"}
USER_PLUG_MIN = 60000
MAX_SLOT = 89
PARAM_NAMES = ("P1", "P2", "attack", "decay", "sustain", "release", "fx1", "fx2", "filter", "resonance", "pan",
               "level", "portamento", "lfo depth", "lfo speed", "lfo target", "lfo shape", "note style")
PRESET_SLOTS = ("bass", "lead", "arp", "chord")
PRESET_NAME_MAX = 11
PRESET_INDEX_MAX = 13
FX_TRACK_MIN = 8            # fx1, fx2, tape, master: no preset keys on the device, only preset 0 when chosen
FX_TRACKS_MASK = 0xF00      # trackmask bits of fx1, fx2, tape, master
MASTER = 11
MASTER_TE_PLUGS = {39}      # TE's master chorus: the one plug known to process master in place


class PlanError(RuntimeError):
    pass


# ---------------------------------------------------------------------------------------------------------------
# Pure helpers (no device): manifests, plans, verification
# ---------------------------------------------------------------------------------------------------------------

def track_index(value: str) -> int:
    if value.lower() in TRACKS:
        return TRACKS[value.lower()]
    n = int(value)
    if not 0 <= n <= 11:
        raise argparse.ArgumentTypeError("track must be a name (%s) or 0..11" % ", ".join(TRACKS))
    return n


def tracks_from_mask(mask: int) -> list[int]:
    return [t for t in range(12) if mask >> t & 1]


def load_manifest(engine_dir: str) -> dict:
    """plug.json and its presets file, without the built engine (enough for the presets commands)."""
    edir = os.path.abspath(engine_dir)
    name = os.path.basename(edir.rstrip("/"))
    with open(os.path.join(edir, "plug.json")) as f:
        manifest = json.load(f)
    presets = []
    pfile = manifest.get("presets")
    if isinstance(pfile, str) and os.path.exists(os.path.join(edir, pfile)):
        with open(os.path.join(edir, pfile)) as f:
            presets = json.load(f)
    return {"name": manifest.get("engine", name), "manifest": manifest, "presets": presets, "data": b"",
            "file": None, "dir": name, "device_path": "engines/%s.engine" % manifest.get("engine", name)}


def load_engine(engine_dir: str, engine_file: str | None = None) -> dict:
    engine = load_manifest(engine_dir)
    path = engine_file or os.path.join(ROOT, "build", "bfin", engine["dir"], engine["dir"] + ".engine")
    if not os.path.exists(path):
        raise PlanError("no built engine at %s — run: python3 tools/opz_build.py %s" % (path, engine_dir))
    with open(path, "rb") as f:
        engine["data"] = f.read()
    engine["file"] = path
    return engine


def newest_settings(root: str) -> str | None:
    """The newest backup folder under root that holds both settings files."""
    dirs = sorted((d for d in os.listdir(root) if os.path.isfile(os.path.join(root, d, PLUGS))
                   and os.path.isfile(os.path.join(root, d, SLOTS))), reverse=True) if os.path.isdir(root) else []
    return os.path.join(root, dirs[0]) if dirs else None


def restore_check(a) -> int:
    """opz.py restore-check: offline, reads names and sizes only."""
    source = a.source or newest_settings(os.path.join(ROOT, "backups"))
    device = None
    if source:
        with open(os.path.join(source, PLUGS)) as f1, open(os.path.join(source, SLOTS)) as f2:
            device = {"plugs": json.load(f1), "slots": json.load(f2)}
        print("settings: %s" % source)
    else:
        print("settings: none (no backup in backups/): no prediction about plug definitions or matches")
    r = rcheck.check(rcheck.list_folder(a.backup), device, rcheck.list_folder(a.disk) if a.disk else None)
    for line in rcheck.describe(r):
        print(line)
    if a.log:
        with open(a.log, errors="replace") as f:
            log = rcheck.parse_import_log(f.read())
        print("import.log: last count %s; %d imported, %d refused for memory, %d failed" % (
            log["used"] or "none", log["imports"], len(log["memory_full"]), len(log["failed"])))
        for n in log["memory_full"]:
            print("  sample memory full: " + n)
        for line in log["failed"]:
            print("  " + line)
    return 1 if r["problems"] else 0


def engine_sizes(build: str | None = None) -> dict:
    """Device path -> size in bytes of every engine built here (build/bfin/NAME/NAME.engine), for space_report."""
    build = build or os.path.join(ROOT, "build", "bfin")
    out = {}
    for name in (sorted(os.listdir(build)) if os.path.isdir(build) else []):
        f = os.path.join(build, name, name + ".engine")
        if os.path.isfile(f):
            out["engines/%s.engine" % name] = os.path.getsize(f)
    return out


def device_presets(presets: list) -> list:
    """Our presets.json -> the keys PlugManager's readPresetSettings reads (defaultOctave, index, name, slot, values)."""
    out = []
    for p in presets:
        values = list(p["values"])
        if len(values) != 18 or not all(0 <= v <= 255 for v in values):
            raise PlanError("preset %r must have 18 values in 0..255" % p.get("name"))
        out.append({"slot": int(p.get("slot", 0)), "index": int(p.get("index", 0)), "name": str(p.get("name", "")),
                    "defaultOctave": int(p.get("defaultOctave", p.get("default_octave", 0))), "values": values})
    return out


def preset_slot(track: int) -> int:
    """The preset slot the OP-Z reads on a track: track − 4 on the synth tracks 4–7, else 0."""
    return track - 4 if 4 <= track <= 7 else 0


def _is_int(v) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def preset_problems(presets: list, trackmask: int = 0) -> tuple[list[str], list[str]]:
    """(errors, warnings) for a presets.json against what the OP-Z accepts and which slots the plug's tracks read."""
    errors, warnings, seen = [], [], {}
    for n, p in enumerate(presets):
        name = p.get("name", "#%d" % n)
        values = p.get("values")
        if not isinstance(values, list) or len(values) != 18 or not all(_is_int(v) and 0 <= v <= 255 for v in values):
            errors.append("preset %r must have 18 values in 0..255" % name)
        slot, index = p.get("slot", 0), p.get("index", 0)
        if not _is_int(slot) or not 0 <= slot <= 3:
            errors.append("preset %r: slot must be 0..3 (%s)" % (name, ", ".join(PRESET_SLOTS)))
            continue
        if not _is_int(index) or not 0 <= index <= PRESET_INDEX_MAX:
            errors.append("preset %r: index must be 0..%d" % (name, PRESET_INDEX_MAX))
            continue
        if (slot, index) in seen:
            errors.append("presets %r and %r share slot %d index %d" % (seen[(slot, index)], name, slot, index))
        seen[(slot, index)] = name
        if not isinstance(name, str) or len(name) > PRESET_NAME_MAX:
            warnings.append("preset %r: the OP-Z keeps %d characters of the name" % (name, PRESET_NAME_MAX))
        if isinstance(values, list) and len(values) == 18 and values[13] == 0:
            warnings.append("preset %r: LFO depth 0 is full negative depth (the dial is bipolar, 128 = off); for no "
                            "LFO use depth 128, speed 100, target 255, shape 0 like TE's default sound" % name)
    tracks = tracks_from_mask(trackmask)
    if tracks and seen:
        read = sorted({preset_slot(t) for t in tracks})
        for slot in sorted({s for s, _ in seen} - set(read)):
            warnings.append("presets in slot %d (%s) are never used: the plug's tracks (%s) read slot %s"
                            % (slot, PRESET_SLOTS[slot], ", ".join(TRACK_NAMES[t] for t in tracks),
                               " and ".join(str(r) for r in read)))
        for slot in read:
            if (slot, 0) not in seen:
                users = [TRACK_NAMES[t] for t in tracks if preset_slot(t) == slot]
                warnings.append("no preset at slot %d index 0: %s start%s from TE's default sound when switched to "
                                "this plug" % (slot, ", ".join(users), "" if len(users) > 1 else "s"))
        extra = sorted(k for k in seen if k[0] == 0 and k[1] != 0)
        if all(t >= FX_TRACK_MIN for t in tracks) and extra:
            warnings.append("%d preset%s past slot 0 index 0 never used: effect tracks (%s) have no preset keys, only "
                            "the preset applied when the plug is chosen" % (
                                len(extra), "s are" if len(extra) > 1 else " is", ", ".join(TRACK_NAMES[t] for t in tracks)))
    return errors, warnings


def preset_keys(presets: list, trackmask: int = 0) -> list[tuple[list[int], list[int]]]:
    """[(tracks, indices)]: the preset indices each track can recall, grouped by equal sets. Preset index n is white
    key n+1 (TrackContext::onNotePressed's key table); FX tracks have no preset keys. With no trackmask, the synth
    tracks of the slots the presets fill."""
    slots: dict[int, set[int]] = {}
    for p in presets:
        slots.setdefault(int(p.get("slot", 0)), set()).add(int(p.get("index", 0)))
    if trackmask:
        tracks = [t for t in tracks_from_mask(trackmask) if t < FX_TRACK_MIN]
    else:
        tracks = sorted(4 + s for s in slots if 0 <= s <= 3)
    groups: dict[tuple[int, ...], list[int]] = {}
    for t in tracks:
        groups.setdefault(tuple(sorted(slots.get(preset_slot(t), ()))), []).append(t)
    return [(ts, list(ix)) for ix, ts in groups.items()]


def _key_list(indices: list[int]) -> str:
    """Preset indices as 1-based white keys, runs joined: [0, 1, 2, 4] -> '1-3, 5'."""
    runs: list[list[int]] = []
    for i in sorted(indices):
        if runs and i == runs[-1][1] + 1:
            runs[-1][1] = i
        else:
            runs.append([i, i])
    return ", ".join("%d" % (a + 1) if a == b else "%d-%d" % (a + 1, b + 1) for a, b in runs)


def preset_keys_text(name: str, presets: list, trackmask: int = 0) -> str:
    """What the user should see: the white keys that light (solid) while holding a track with the plug chosen."""
    parts = ["%s: white key%s %s" % (", ".join(TRACK_NAMES[t] for t in ts), "s" if len(ix) > 1 else "", _key_list(ix))
             if ix else "%s: none" % ", ".join(TRACK_NAMES[t] for t in ts) for ts, ix in preset_keys(presets, trackmask)]
    if not parts:
        return ""
    return ("check on the OP-Z: hold the track with %s chosen; lit white keys = presets (%s). Only white key 1 lit "
            "(it blinks after every sound-slot change) = no presets there: opz.py presets push engines/%s"
            % (name, "; ".join(parts), name))


def find_preset(presets: list, name: str | None = None, slot: int | None = None, index: int | None = None) -> dict:
    if name is not None:
        found = [p for p in presets if str(p.get("name", "")).lower() == name.lower()]
        if len(found) != 1:
            raise PlanError("%s preset named %r" % ("no" if not found else "more than one", name))
        return found[0]
    found = [p for p in presets if p.get("slot", 0) == (slot or 0) and p.get("index", 0) == (index or 0)]
    if not found:
        raise PlanError("no preset at slot %d index %d" % (slot or 0, index or 0))
    return found[0]


def format_sound(values, knobs=None) -> str:
    names = list(PARAM_NAMES)
    for i, k in enumerate(list(knobs or [])[:2]):
        names[i] = str(k)
    return " ".join("%s=%d" % (n, v) for n, v in zip(names, values))


def set_command(track: int, values) -> str:
    return "opz.py presets set --track %s --values %s" % (TRACK_NAMES.get(track, track), ",".join(str(v) for v in values))


def check_sound(track: int, values) -> list[int]:
    values = list(values)
    if not 0 <= track <= 11:
        raise PlanError("track must be 0..11")
    if len(values) != 18 or not all(_is_int(v) and 0 <= v <= 255 for v in values):
        raise PlanError("a sound needs 18 values in 0..255")
    return values


def plug_entry(record: dict, presets: list) -> dict:
    """A top-level plugs[] entry (imported without a version check) that only replaces presets: it repeats the
    installed engine, samples, flags and version, which the import overwrites."""
    return {"id": int(record["id"]), "version": int(record["version"]), "engine": record["engine"],
            "samples": record.get("samples", ""), "flags": int(record.get("flags", 0)),
            "presets": device_presets(presets)}


def build_presets_patch(record: dict, presets: list, nonce: int | None = None) -> bytes:
    """patch.json that only replaces one plug's presets (see plug_entry)."""
    doc = {"plugs": [plug_entry(record, presets)]}
    if nonce is not None:
        doc["nonce"] = nonce
    return json.dumps(doc, separators=(",", ":"), sort_keys=True).encode()


def catalogue(root: str = ENGINES_DIR, first: list | None = None) -> list[dict]:
    """`first`, then every engine directory under `root` with a plug.json (manifest and presets only)."""
    out = list(first or [])
    for name in sorted(os.listdir(root)) if os.path.isdir(root) else []:
        if os.path.exists(os.path.join(root, name, "plug.json")):
            out.append(load_manifest(os.path.join(root, name)))
    return out


def boot_entries(plugs: dict, engines: list) -> tuple[list[dict], list[str], list[str]]:
    """The boot file's plugs[]: every installed plug in the user range that one of `engines` describes (same id and
    engine path) and whose presets pass preset_problems. The first engine with an id wins. Returns (entries, one
    label per entry, warnings). Plugs we have no engine directory for (TE's, the app's sample plugs) are never in it."""
    by_id: dict[int, dict] = {}
    for e in engines:
        by_id.setdefault(int(e["manifest"]["plug"]["id"]), e)
    entries, labels, warnings = [], [], []
    for record in sorted(plugs.get("plugs", []), key=lambda p: p["id"]):
        e = by_id.get(record["id"])
        if record["id"] < USER_PLUG_MIN or e is None or not e["presets"]:
            continue
        if record["engine"] != e["device_path"]:
            warnings.append("plug %d is %s on the OP-Z, not %s: left out of the boot file"
                            % (record["id"], record["engine"], e["device_path"]))
            continue
        if preset_problems(e["presets"], int(e["manifest"]["plug"].get("trackmask", 0)))[0]:
            warnings.append("%s: presets.json has errors (opz.py presets check): left out of the boot file" % e["name"])
            continue
        entries.append(plug_entry(record, e["presets"]))
        labels.append("%s %d (%d)" % (e["name"], record["id"], len(e["presets"])))
    return entries, labels, warnings


def build_boot_patch(entries: list[dict]) -> bytes:
    """settings/plugs_patch.json: plugs[] only (no auto_import, no engines[], so no engine or track changes)."""
    return json.dumps({"plugs": entries}, separators=(",", ":"), sort_keys=True).encode()


def build_patch(engine: dict, tracks: list[int]) -> bytes:
    m = engine["manifest"]
    plug = m["plug"]
    doc = {"auto_import": [{
        "engine": {"path": engine["device_path"], "memoryslot": int(m["memoryslot"]),
                   "version": int(m.get("engine_version", 1))},
        "plug": {"id": int(plug["id"]), "version": int(plug["version"]), "engine": engine["device_path"],
                 "samples": plug.get("samples", ""), "flags": int(plug.get("flags", 0)),
                 "name": plug.get("name", engine["name"]), "trackmask": int(plug.get("trackmask", 0)),
                 "parameter_names": plug.get("parameter_names", []),
                 "presets": device_presets(engine["presets"])},
        "tracks": tracks,
    }]}
    return json.dumps(doc, separators=(",", ":"), sort_keys=True).encode()


def slot_entries(slots: dict, track: int) -> list:
    tr = next((t for t in slots.get("tracks", []) if t.get("index") == track), None)
    return tr["packs"] if tr else []


def assign(slots: dict, track: int, slot: int, plug_id: int) -> dict:
    doc = json.loads(json.dumps(slots))
    tr = next((t for t in doc["tracks"] if t.get("index") == track), None)
    if tr is None:
        tr = {"index": track, "packs": []}
        doc["tracks"].append(tr)
        doc["tracks"].sort(key=lambda t: t["index"])
    tr["packs"] = [p for p in tr["packs"] if p["slot"] != slot] + [{"id": plug_id, "slot": slot}]
    tr["packs"].sort(key=lambda p: p["slot"])
    return doc


def unassign(slots: dict, plug_id: int) -> dict:
    doc = json.loads(json.dumps(slots))
    for tr in doc["tracks"]:
        tr["packs"] = [p for p in tr["packs"] if p["id"] != plug_id]
    return doc


def assigned_tracks(slots: dict, plug_id: int) -> list[tuple[int, int]]:
    return [(t["index"], p["slot"]) for t in slots.get("tracks", []) for p in t.get("packs", []) if p["id"] == plug_id]


def effect_plug(manifest: dict) -> bool:
    """The trackmask names an effect track. Only such a plug may go on fx1, fx2, tape or master: what a synth or drum
    engine does there is untested."""
    return bool(int(manifest["plug"].get("trackmask", 0)) & FX_TRACKS_MASK)


def plug_manifest(engines: list, plug_id: int) -> dict | None:
    return next((e["manifest"] for e in engines if int(e["manifest"]["plug"]["id"]) == plug_id), None)


def instrument_error(plug_id: int, track: int, manifest: dict | None) -> str:
    why = ("its trackmask 0x%x names no effect track" % int(manifest["plug"].get("trackmask", 0)) if manifest
           else "no local plug.json with that id names an effect track in its trackmask")
    return ("plug %d cannot go on track %s: %s, so it is a synth or drum engine, and what one does on fx1, fx2, tape "
            "or master is untested (it may crash the OP-Z). Use tracks kick..chord (1-8)" % (plug_id, TRACK_NAMES[track], why))


def master_in_place(manifest: dict) -> bool:
    """plug.json says the engine handles master's call: renderFX with out = NULL, result written back into in."""
    return manifest.get("master_in_place") is True


def master_plugs(engines: list) -> set[int]:
    """Plug ids that may go on master: TE's chorus and every engine in `engines` whose plug.json declares it."""
    return MASTER_TE_PLUGS | {int(e["manifest"]["plug"]["id"]) for e in engines if master_in_place(e["manifest"])}


def master_error(plug_id: int) -> str:
    return ("plug %d cannot go on track master: master runs its plug in place (renderFX gets out = NULL), so an "
            "engine that writes out crashes the OP-Z, probably at every start of that project "
            "(research/routing.md section 3). Only an engine whose plug.json declares \"master_in_place\": true "
            "may go there" % plug_id)


def plan_install(plugs: dict, slots: dict, engine: dict, tracks: list[int], slot: int | None,
                 replace: bool = False) -> tuple[list[str], list[str], dict]:
    """Return (errors, warnings, plan). plan["slot_map"] is set when a specific slot must be written."""
    m, errors, warnings = engine["manifest"], [], []
    plug = m["plug"]
    pid, pver, mslot = int(plug["id"]), int(plug["version"]), int(m["memoryslot"])
    ever = int(m.get("engine_version", 1))
    if engine["name"] in TE_ENGINES:
        errors.append("engine name %r is a TE engine; pick another name" % engine["name"])
    if m.get("sim_only"):
        errors.append("%s is a simulator-only engine (plug.json sim_only): it never goes on an OP-Z" % engine["name"])
    if pid < USER_PLUG_MIN:
        errors.append("plug id %d is below %d (TE's range)" % (pid, USER_PLUG_MIN))
    if not 0 < mslot <= MAX_SLOT:
        errors.append("memory slot %d is outside 1..%d" % (mslot, MAX_SLOT))
    for e in plugs.get("engines", []):
        if e["memoryslot"] == mslot and e["path"] != engine["device_path"]:
            errors.append("memory slot %d is already used by %s" % (mslot, e["path"]))
        if e["path"] == engine["device_path"] and e["version"] == ever:
            warnings.append("engine version %d is already registered: bump engine_version in plug.json" % ever)
    old = next((p for p in plugs.get("plugs", []) if p["id"] == pid), None)
    if old is not None:
        if old["engine"] != engine["device_path"]:
            errors.append("plug id %d already belongs to %s" % (pid, old["engine"]))
        elif old["version"] >= pver:
            errors.append("plug %d version %d is installed; the OP-Z ignores an import unless the version is higher "
                          "(bump plug.version in plug.json)" % (pid, old["version"]))
    samples = str(plug.get("samples", ""))
    if samples:
        # The plug names a pack: it must be one of ours and already on the OP-Z (a plug naming a missing pack breaks
        # content mode; plug-format.md). Our packs are installed with `opz.py samples install` first.
        if not samples.startswith(smp.OUR_PREFIX):
            errors.append("plug.json names %s; an engine plug may only name a pack of ours (%s...)"
                          % (samples, smp.OUR_PREFIX))
        elif not any(p.get("samples") == samples and smp.is_our_sample(p) for p in plugs.get("plugs", [])):
            errors.append("plug.json names %s, which no sample plug of ours has installed: install the pack first "
                          "(opz.py samples install, research/samples.md H-S1)" % samples)
    mask = int(plug.get("trackmask", 0))
    perrors, pwarnings = preset_problems(engine["presets"], mask)
    errors += perrors
    warnings += pwarnings
    plan = {"tracks": [], "slot_map": None, "assign": [],
            "restart": any(e["path"] == engine["device_path"] for e in plugs.get("engines", []))}
    for t in tracks:
        if t >= FX_TRACK_MIN and not effect_plug(m):
            errors.append(instrument_error(pid, t, m))
            continue
        if t == MASTER and not master_in_place(m):
            errors.append(master_error(pid))
            continue
        if mask and not mask >> t & 1:
            warnings.append("track %s is outside the plug's trackmask 0x%x (the device does not enforce it)"
                            % (TRACK_NAMES[t], mask))
        packs = slot_entries(slots, t)
        if any(p["id"] == pid for p in packs):
            warnings.append("plug %d is already on track %s" % (pid, TRACK_NAMES[t]))
            continue
        if slot is not None:
            taken = next((p for p in packs if p["slot"] == slot), None)
            if taken and not replace:
                errors.append("track %s slot %d holds plug %d (use --replace to overwrite)" % (TRACK_NAMES[t], slot, taken["id"]))
            plan["assign"].append((t, slot))
        else:
            used = {p["slot"] for p in packs}
            free = next((s for s in range(1, 11) if s not in used), None)
            if free is None:
                errors.append("track %s has no free slot (use --slot N --replace)" % TRACK_NAMES[t])
            else:
                plan["tracks"].append(t)
                plan["assign"].append((t, free))
    if slot is not None:
        doc = slots
        for t, s in plan["assign"]:
            doc = assign(doc, t, s, pid)
        plan["slot_map"] = doc
        errors += orphan_errors(plugs, slots, doc)
    return errors, warnings, plan


def orphan_errors(plugs: dict, before: dict, after: dict) -> list[str]:
    """Refuse a slot change that leaves one of the user's own samples in no slot: content mode would drop it and
    delete its file (research/samples.md)."""
    return ["slot change would leave user sample %d (%s) in no slot; the OP-Z deletes unassigned user samples in "
            "content mode. Pick another slot" % (p["id"], p.get("samples", "")) for p in
            smp.orphaned_user_samples(plugs, before, after)]


def verify_install(plugs: dict, slots: dict, engine: dict, expected: list[tuple[int, int]]) -> list[str]:
    m, problems = engine["manifest"], []
    pid = int(m["plug"]["id"])
    e = next((x for x in plugs.get("engines", []) if x["path"] == engine["device_path"]), None)
    if e is None:
        problems.append("engine %s missing from plugs.json" % engine["device_path"])
    elif e["memoryslot"] != int(m["memoryslot"]) or e["version"] != int(m.get("engine_version", 1)):
        problems.append("engine definition differs: %s" % e)
    p = next((x for x in plugs.get("plugs", []) if x["id"] == pid), None)
    if p is None:
        problems.append("plug %d missing from plugs.json" % pid)
    elif p["engine"] != engine["device_path"] or p["version"] != int(m["plug"]["version"]):
        problems.append("plug record differs: %s" % p)
    got = set(assigned_tracks(slots, pid))
    for t, s in expected:
        if (t, s) not in got:
            problems.append("plug %d not in track %s slot %d" % (pid, TRACK_NAMES[t], s))
    return problems


def orphan_engines(plugs: dict) -> list[str]:
    used = {p["engine"] for p in plugs.get("plugs", [])}
    return sorted(e["path"] for e in plugs.get("engines", []) if e["path"] not in used)


def describe(plugs: dict, slots: dict) -> str:
    engines = {p["id"]: p["engine"].split("/")[-1].replace(".engine", "") for p in plugs.get("plugs", [])}
    lines = []
    for t in sorted(slots.get("tracks", []), key=lambda t: t["index"]):
        cells = ["%d:%s%s" % (p["slot"], p["id"], "(%s)" % engines.get(p["id"], "?")) for p in t["packs"]]
        lines.append("  %-7s %s" % (TRACK_NAMES.get(t["index"], str(t["index"])), "  ".join(cells)))
    user = [p for p in plugs.get("plugs", []) if p["id"] >= USER_PLUG_MIN]
    lines.append("user plugs: " + (", ".join("%d %s v%d" % (p["id"], p["engine"], p["version"]) for p in user) or "none"))
    lines.append("orphan engines: " + (", ".join(orphan_engines(plugs)) or "none"))
    loose = smp.unassigned_user_samples(plugs, slots)
    if loose:
        lines.append("WARNING: user samples in no slot (content mode deletes them; assign them again first): "
                     + ", ".join("%d %s" % (p["id"], p.get("samples", "")) for p in loose))
    return "\n".join(lines)


# ---------------------------------------------------------------------------------------------------------------
# Room on the OP-Z, and taking every plug of ours off it (pure). docs/roadmap/restore-space.md
# ---------------------------------------------------------------------------------------------------------------

OUR_PLUG_MIN = 60900        # test plugs 60900-60999, released from 61000, our sample plugs 61400-61499
OUR_PLUG_MAX = 61999        # (sdk/docs/memory-slots.md); 60000-60899 are other people's user plugs
PLUG_LIMIT = 200            # plug definitions the OS keeps (FUN_800ff9e8: "plug count full" past 200) [code]
SAMPLE_LIMIT = 24 * 1024 * 1024   # content mode's sample memory, samplepacks/user/ only (FUN_8003377c) [code]


ROOM_LOW = 60               # free plug definitions below which our tools warn before an install [inferred: a
ROOM_TIGHT = 30             # typical 24 MB of user samples is 25-60 packs], and below which they call it full


def room_level(free: int) -> str:
    return "full" if free < ROOM_TIGHT else "low" if free < ROOM_LOW else "ok"


def install_room(plugs: dict, plug_id: int) -> str | None:
    """The warning when an install leaves too few plug definitions for a content-mode restore of the user's own
    samples (each needs one; a full table drops the sample silently, docs/roadmap/restore-space.md), or None."""
    added = 0 if any(p["id"] == plug_id for p in plugs.get("plugs", [])) else 1
    free = PLUG_LIMIT - len(plugs.get("plugs", [])) - added
    level = room_level(free)
    if level == "ok":
        return None
    return ("room: %d of %d plug definitions free after this install; a content-mode restore of your own samples needs "
            "one per sample (%s). opz.py remove --all-ours frees them again"
            % (max(free, 0), PLUG_LIMIT, "too few for a typical backup" if level == "full" else "fewer than %d left" % ROOM_LOW))


def is_our_plug(plug: dict) -> bool:
    """A plug this project made: its id is in our range and it is not one of the user's samples (whose ids are
    crc32 of their path and can land anywhere)."""
    return OUR_PLUG_MIN <= int(plug["id"]) <= OUR_PLUG_MAX and not smp.is_user_sample(plug)


def space_report(plugs: dict, slots: dict, engine_sizes: dict | None = None) -> dict:
    """What the OP-Z holds that a restore needs room for, from the two settings files (what the file service can
    read; it lists no file sizes and no free space). engine_sizes maps an engine path to its size in bytes, for the
    engines whose build we have."""
    every = plugs.get("plugs", [])
    ours = [p for p in every if is_our_plug(p)]
    user = [p for p in every if smp.is_user_sample(p)]
    # engines our plugs use and orphans (left by removals), except TE's own (our sample plugs play on TE's)
    used = {p["engine"] for p in ours} | set(orphan_engines(plugs))
    our_engines = sorted(e["path"] for e in plugs.get("engines", []) if e["path"] in used
                         and e["path"].split("/")[-1].replace(".engine", "") not in TE_ENGINES)
    sizes = engine_sizes or {}
    known = [sizes[p] for p in our_engines if p in sizes]
    in_slots = {p["id"] for t in slots.get("tracks", []) for p in t.get("packs", [])}
    return {
        "plugs": len(every), "limit": PLUG_LIMIT, "free": PLUG_LIMIT - len(every),
        "ours": [p["id"] for p in ours], "user_samples": len(user),
        "te": len(every) - len(ours) - len(user),
        "our_engine_files": our_engines, "our_engine_bytes": sum(known), "unsized": len(our_engines) - len(known),
        "displaced_te": sorted(p["id"] for p in every if p["id"] < USER_PLUG_MIN and p["id"] not in in_slots),
        "loose_user_samples": [p["id"] for p in smp.unassigned_user_samples(plugs, slots)],
    }


def describe_space(r: dict) -> str:
    lines = ["plug definitions: %d of %d (%d free): %d of ours, %d user samples, %d TE"
             % (r["plugs"], r["limit"], r["free"], len(r["ours"]), r["user_samples"], r["te"])]
    size = "%d KB" % -(-r["our_engine_bytes"] // 1024) + (" + %d not built here" % r["unsized"] if r["unsized"] else "")
    lines.append("our engine files: %d (%s), on the OP-Z's 512 MB flash; content mode shows none of them"
                 % (len(r["our_engine_files"]), size))
    if r["displaced_te"]:
        lines.append("TE plugs in no slot (displaced; a restore or remove --all-ours --put-back brings them back): "
                     + ", ".join(str(i) for i in r["displaced_te"]))
    if r["loose_user_samples"]:
        lines.append("WARNING: user samples in no slot (content mode deletes them): "
                     + ", ".join(str(i) for i in r["loose_user_samples"]))
    return "\n".join(lines)


def plan_remove_ours(plugs: dict, slots: dict, put_back: dict | None = None) -> dict:
    """Every plug of ours off the OP-Z: which plugs go, which cells empty, and (with put_back, the slot map of an
    earlier backup) which installed non-ours plug goes back into a cell we empty. Plugs whose removal would delete
    a file we did not write (removal_block) stay, with the reason."""
    ids, kept = [], []
    for p in plugs.get("plugs", []):
        if not is_our_plug(p):
            continue
        why = smp.removal_block(p, plugs)
        if why:
            kept.append((p["id"], why))
        else:
            ids.append(p["id"])
    gone = set(ids)
    doc = json.loads(json.dumps(slots))
    cleared = [(t["index"], q["slot"], q["id"]) for t in doc.get("tracks", []) for q in t.get("packs", [])
               if q["id"] in gone]
    for pid in ids:
        doc = unassign(doc, pid)
    refilled = []
    if put_back is not None:
        installed = {p["id"] for p in plugs.get("plugs", [])}
        old = {(t["index"], q["slot"]): q["id"] for t in put_back.get("tracks", []) for q in t.get("packs", [])}
        for t, s, _ in cleared:
            q = old.get((t, s))
            if q is None or q in gone or q not in installed:
                continue
            doc = assign(doc, t, s, q)
            refilled.append((t, s, q))
    return {"ids": ids, "kept": kept, "cleared": cleared, "refilled": refilled, "slot_map": doc,
            "errors": orphan_errors(plugs, slots, doc)}


# ---------------------------------------------------------------------------------------------------------------
# Sample packs (pure): plan, patch, listing. research/samples.md
# ---------------------------------------------------------------------------------------------------------------

def plan_samples_install(plugs: dict, slots: dict, pack: dict, path: str, name: str, tracks: list[int],
                         slot: int | None = None, replace: bool = False, plug_id: int | None = None,
                         version: int | None = None) -> tuple[list[str], list[str], dict]:
    """(errors, warnings, plan) for putting a checked pack (opz_samples.parse) at `path` as a plug of ours.

    plan = {"plug": the patch.json plug, "engine": the engine definition to repeat, "tracks": auto_import tracks,
    "assign": [(track, slot)], "slot_map": doc or None, "update": bool}."""
    errors, warnings = list(pack["errors"]), list(pack["warnings"])
    plan = {"plug": None, "engine": None, "tracks": [], "assign": [], "slot_map": None, "update": False}
    kind = pack["kind"]
    if kind not in ("drum", "synth"):
        errors.append("only op-1 drum packs and synth samples can be installed (this is %s)" % (kind or "unreadable"))
        return errors, warnings, plan
    if not path.startswith(smp.OUR_PREFIX):
        errors.append("%s is outside %s, where our packs live" % (path, smp.OUR_PREFIX))
    if smp.upload_path_error(path):
        errors.append(smp.upload_path_error(path))
    engine_path = smp.DRUM_ENGINE if kind == "drum" else smp.SYNTH_ENGINE
    allowed = range(0, 4) if kind == "drum" else range(4, 8)
    for t in tracks:
        if t not in allowed:
            errors.append("a %s pack plays on %s, not on %s" % (
                kind, "kick, snare, perc, sample" if kind == "drum" else "bass, lead, arp, chord", TRACK_NAMES[t]))
    engine = next((e for e in plugs.get("engines", []) if e["path"] == engine_path), None)
    if engine is None:
        errors.append("%s is not installed on this OP-Z" % engine_path)
    all_plugs = plugs.get("plugs", [])
    users = [p for p in all_plugs if p.get("samples") == path]
    ours = [p for p in users if smp.is_our_sample(p)]
    if len(users) > len(ours) or len(ours) > 1:
        errors.append("%s is already used by plug(s) %s; pick another name" % (path, [p["id"] for p in users]))
    pid_of_path = smp.pack_id(path)
    clash = [p["id"] for p in all_plugs if p.get("samples") and p.get("samples") != path
             and smp.pack_id(p["samples"]) == pid_of_path]
    if clash:
        errors.append("%s has the same sample-pack id as plug(s) %s; pick another name" % (path, clash))
    old = ours[0] if len(ours) == 1 else None
    if old is not None:
        plan["update"] = True
        if plug_id is not None and plug_id != old["id"]:
            errors.append("%s belongs to plug %d; leave out --plug-id or use %d" % (path, old["id"], old["id"]))
        plug_id = old["id"]
        version = version if version is not None else int(old["version"]) + 1
        if version <= int(old["version"]):
            errors.append("plug %d version %d is installed; the OP-Z ignores an import unless the version is higher"
                          % (plug_id, old["version"]))
        warnings.append("updating plug %d: the OP-Z re-reads the pack when the plug is next loaded; restart it if "
                        "the old sound stays" % plug_id)
    else:
        taken = {p["id"] for p in all_plugs}
        if plug_id is None:
            plug_id = next((i for i in smp.OUR_PLUG_IDS if i not in taken), None)
            if plug_id is None:
                errors.append("all sample plug ids %d-%d are in use" % (smp.OUR_PLUG_IDS[0], smp.OUR_PLUG_IDS[-1]))
                return errors, warnings, plan
        elif plug_id not in smp.OUR_PLUG_IDS:
            errors.append("plug id %d is outside our sample range %d-%d" % (
                plug_id, smp.OUR_PLUG_IDS[0], smp.OUR_PLUG_IDS[-1]))
        elif plug_id in taken:
            errors.append("plug id %d is already installed" % plug_id)
        version = 1 if version is None else version
    if not 0 < version < 128:
        errors.append("version must be 1..127 (stored in one byte)")
    plan["plug"] = {"id": int(plug_id), "version": int(version), "name": name[:32], "engine": engine_path,
                    "samples": path, "flags": 0, "trackmask": 0x000F if kind == "drum" else 0x00F0,
                    "parameter_names": [], "presets": []}
    if engine is not None:
        plan["engine"] = {"path": engine["path"], "memoryslot": engine["memoryslot"], "version": engine["version"]}
    for t in tracks:
        packs = slot_entries(slots, t)
        if any(p["id"] == plug_id for p in packs):
            warnings.append("plug %d is already on track %s" % (plug_id, TRACK_NAMES[t]))
            continue
        if slot is not None:
            taken_slot = next((p for p in packs if p["slot"] == slot), None)
            if taken_slot and not replace:
                errors.append("track %s slot %d holds plug %d (use --replace to overwrite)"
                              % (TRACK_NAMES[t], slot, taken_slot["id"]))
            plan["assign"].append((t, slot))
        else:
            used = {p["slot"] for p in packs}
            free = next((s for s in range(1, 11) if s not in used), None)
            if free is None:
                errors.append("track %s has no free slot (use --slot N --replace)" % TRACK_NAMES[t])
            else:
                plan["tracks"].append(t)
                plan["assign"].append((t, free))
    if slot is not None:
        doc = slots
        for t, s in plan["assign"]:
            doc = assign(doc, t, s, plug_id)
        plan["slot_map"] = doc
        errors += orphan_errors(plugs, slots, doc)
    return errors, warnings, plan


def build_samples_patch(plan: dict) -> bytes:
    """patch.json for a sample plug of ours: TE's sample engine repeated as installed (no change), our plug."""
    doc = {"auto_import": [{"engine": plan["engine"], "plug": plan["plug"], "tracks": plan["tracks"]}]}
    return json.dumps(doc, separators=(",", ":"), sort_keys=True).encode()


def verify_samples_install(plugs: dict, slots: dict, plan: dict, before: dict | None = None) -> list[str]:
    """Problems in the read-back after an install; with `before` (the slot map before it) also every user sample
    that lost its slot."""
    want, problems = plan["plug"], []
    if before is not None:
        problems += ["user sample %d (%s) is no longer in a slot" % (p["id"], p.get("samples", ""))
                     for p in smp.orphaned_user_samples(plugs, before, slots)]
    p = next((x for x in plugs.get("plugs", []) if x["id"] == want["id"]), None)
    if p is None:
        problems.append("plug %d missing from plugs.json" % want["id"])
    elif (p.get("samples"), p.get("engine"), p.get("version")) != (want["samples"], want["engine"], want["version"]):
        problems.append("plug record differs: %s" % p)
    got = set(assigned_tracks(slots, want["id"]))
    for t, s in plan["assign"]:
        if (t, s) not in got:
            problems.append("plug %d not in track %s slot %d" % (want["id"], TRACK_NAMES[t], s))
    return problems


def describe_samples(plugs: dict, slots: dict) -> str:
    where = smp.assignments(slots)
    groups: dict[str, list[str]] = {"ours": [], "user": [], "factory": [], "other": []}
    for p in sorted(plugs.get("plugs", []), key=lambda p: p["id"]):
        kind = smp.classify(p)
        if kind == "engine":
            continue
        at = ", ".join("%s %d" % (TRACK_NAMES.get(t, t), s) for t, s in where.get(p["id"], [])) or "NO SLOT"
        groups[kind].append("  %-10d %-45s %s" % (p["id"], p["samples"], at))
    titles = {"ours": "ours (safe to update and remove)", "user": "the user's own samples (never touched)",
              "factory": "TE's factory packs", "other": "other sample plugs (not ours: never removed)"}
    lines = []
    for k in ("ours", "user", "factory", "other"):
        lines.append("%s: %d" % (titles[k], len(groups[k])))
        lines += groups[k]
    loose = smp.unassigned_user_samples(plugs, slots)
    if loose:
        lines.append("WARNING: %d user sample(s) in NO SLOT. Content mode removes them and deletes their files; "
                     "put them back in a slot (opz.py assign ID --track T) before using content mode." % len(loose))
    return "\n".join(lines)


def plan_samples_purge(plugs: dict, slots: dict, name: str) -> tuple[list[str], list[str], dict, bytes]:
    """(errors, warnings, plan, placeholder) for deleting samplepacks/opzk_NAME.aif when no plug names it: a
    64-frame placeholder pack replaces whatever is there (so the temporary plug never names a missing file), a plug of
    ours with no slot names it, and removing that plug makes the OP-Z delete the file (FUN_800fe700)."""
    path = smp.pack_path(name)      # samplepacks/opzk_NAME.aif; the old samplepacks/user/ path never held a pack (Q-49)
    placeholder = smp.build_drum_pack([array.array("h", [0] * 64)], "purge")
    users = [p["id"] for p in plugs.get("plugs", []) if p.get("samples") == path]
    if users:
        return (["plug(s) %s name %s: remove the plug instead (opz.py samples remove)" % (users, path)], [],
                {"plug": None}, placeholder)
    errors, warnings, plan = plan_samples_install(plugs, slots, smp.parse(placeholder), path, "purge-" + name, [])
    return errors, warnings, plan, placeholder


def samples_from_args(a) -> tuple[list, list[str]]:
    """The sounds for `samples build`: --test-kit [tiny], --wav files, or the WAVs in --dir (sorted by name)."""
    if a.test_kit:
        return (smp.tiny_kit_sounds() if a.test_kit == "tiny" else smp.test_kit_sounds()), []
    files = list(a.wav or [])
    if a.dir:
        files += sorted(os.path.join(a.dir, f) for f in os.listdir(a.dir) if f.lower().endswith(".wav"))
    if not files:
        raise PlanError("give --test-kit, --wav FILE (repeatable) or --dir DIR")
    sounds, notes = [], []
    for f in files:
        s, n = smp.read_wav(f)
        sounds.append(s)
        notes += n
    return sounds, notes


# ---------------------------------------------------------------------------------------------------------------
# Device operations
# ---------------------------------------------------------------------------------------------------------------

class Opz:
    def __init__(self, transport, log=print, backups=os.path.join(ROOT, "backups"), confirm=None, clock=time.monotonic):
        self.transport = transport
        self.clock = clock
        self.log = log
        self.backups = backups
        self.confirm = confirm or (lambda question: input(question + " [y/N] ").strip().lower() == "y")

    @contextlib.contextmanager
    def session(self):
        s = FileSession(self.transport, log=lambda m: self.log("  " + m), clock=self.clock)
        s.open()
        try:
            yield s
        finally:
            s.close()

    def read_settings(self) -> tuple[bytes, bytes]:
        with self.session() as s:
            return s.pull(PLUGS), s.pull(SLOTS)

    def backup(self, label: str = "") -> tuple[str, dict, dict]:
        plugs_raw, slots_raw = self.read_settings()
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        d = os.path.join(self.backups, stamp + (("-" + label) if label else ""))
        os.makedirs(os.path.join(d, "settings"), exist_ok=True)
        meta = {}
        for path, raw in ((PLUGS, plugs_raw), (SLOTS, slots_raw)):
            with open(os.path.join(d, path), "wb") as f:
                f.write(raw)
            meta[path] = hashlib.sha256(raw).hexdigest()
        with open(os.path.join(d, "backup.json"), "w") as f:
            json.dump({"sha256": meta}, f, indent=2)
        self.log("backup: %s" % d)
        return d, json.loads(plugs_raw), json.loads(slots_raw)

    def push(self, files, remove=None) -> dict:
        with self.session() as s:
            return s.push(files, remove)

    def install(self, engine: dict, tracks: list[int], slot: int | None = None, replace: bool = False,
                dry_run: bool = False, yes: bool = False, boot: bool = False) -> bool:
        """boot=True: the caller writes the boot file next (persist_presets), so the advice says so."""
        bdir, plugs, slots = self.backup("install-" + engine["name"])
        errors, warnings, plan = plan_install(plugs, slots, engine, tracks, slot, replace)
        for w in warnings:
            self.log("warning: " + w)
        for e in errors:
            self.log("error:   " + e)
        if errors:
            self.log("nothing written.")
            return False
        files = [(engine["device_path"], engine["data"]), (PATCH, build_patch(engine, plan["tracks"]))]
        if plan["slot_map"] is not None:
            files.append((SLOTS, json.dumps(plan["slot_map"], separators=(",", ":")).encode()))
        self.log("plan: %s (%d bytes) as plug %d -> %s" % (
            engine["device_path"], len(engine["data"]), engine["manifest"]["plug"]["id"],
            ", ".join("%s slot %d" % (TRACK_NAMES[t], s) for t, s in plan["assign"]) or "no track"))
        room = install_room(plugs, engine["manifest"]["plug"]["id"])
        if room:
            self.log("warning: " + room)
        if dry_run:
            self.log("dry run: nothing written. patch.json:\n" + files[1][1].decode())
            return True
        if not yes and not self.confirm("Install now?"):
            self.log("cancelled.")
            return False
        result = self.push(files)
        self.log("sync: requested %s; skipped (unchanged) %s" % (result["requested"], result["skipped"]))
        plugs2, slots2 = (json.loads(x) for x in self.read_settings())
        problems = verify_install(plugs2, slots2, engine, plan["assign"])
        if problems:
            for p in problems:
                self.log("verify:  " + p)
            self.log("restoring the slot map from %s" % bdir)
            self.restore(bdir, yes=True)
            return False
        with open(os.path.join(bdir, "install.json"), "w") as f:
            json.dump({"engine": engine["device_path"], "sha256": hashlib.sha256(engine["data"]).hexdigest(),
                       "plug": engine["manifest"]["plug"]["id"], "assigned": plan["assign"]}, f, indent=2)
        self.log("installed and verified.\n" + describe(plugs2, slots2))
        if plan["restart"]:
            self.log("RESTART THE OP-Z: this engine was already registered, and the OP-Z keeps the code it loaded at "
                     "boot until it restarts (proven on hardware 2026-10-17).")
        if engine["presets"]:
            if boot:
                self.log("The OP-Z keeps the presets this install sent in RAM; the boot file written next brings them "
                         "back after %s (research/presets.md)." % ("the restart" if plan["restart"] else "a restart"))
            else:
                self.log(("The restart also drops the presets this install sent" if plan["restart"] else
                          "The OP-Z keeps the presets this install sent in RAM only: a restart or crash drops them")
                         + " (research/presets.md); %s run: opz.py presets push engines/%s"
                         % ("after it," if plan["restart"] else "then", engine["name"]))
            keys = preset_keys_text(engine["name"], engine["presets"],
                                    int(engine["manifest"]["plug"].get("trackmask", 0)))
            if keys:
                self.log(keys)
        return True

    def assign(self, plug_id: int, tracks: list[int], slot: int | None = None, replace: bool = False,
               dry_run: bool = False, yes: bool = False, engines: list | None = None) -> bool:
        """Put an installed plug into a track's sound slot by writing only the slot map. `engines` (catalogue()):
        the plug.json files that say which of our plugs may go on the effect tracks and on master."""
        bdir, plugs, slots = self.backup("assign-%d" % plug_id)
        if not any(p["id"] == plug_id for p in plugs.get("plugs", [])):
            self.log("error:   plug %d is not installed" % plug_id)
            return False
        doc, done, errors = slots, [], []
        manifest = plug_manifest(engines or [], plug_id)
        for t in tracks:
            if t >= FX_TRACK_MIN and plug_id >= USER_PLUG_MIN and not (manifest and effect_plug(manifest)):
                errors.append(instrument_error(plug_id, t, manifest))
                continue
            if t == MASTER and plug_id not in master_plugs(engines or []):
                errors.append(master_error(plug_id))
                continue
            packs = slot_entries(doc, t)
            if any(p["id"] == plug_id for p in packs):
                self.log("warning: plug %d is already on track %s" % (plug_id, TRACK_NAMES[t]))
                continue
            if slot is not None:
                taken = next((p for p in packs if p["slot"] == slot), None)
                if taken and not replace:
                    errors.append("track %s slot %d holds plug %d (use --replace)" % (TRACK_NAMES[t], slot, taken["id"]))
                    continue
                s = slot
            else:
                used = {p["slot"] for p in packs}
                s = next((n for n in range(1, 11) if n not in used), None)
                if s is None:
                    errors.append("track %s has no free slot (use --slot N --replace)" % TRACK_NAMES[t])
                    continue
            doc = assign(doc, t, s, plug_id)
            done.append((t, s))
        errors += orphan_errors(plugs, slots, doc)
        for e in errors:
            self.log("error:   " + e)
        if errors or not done:
            self.log("nothing written.")
            return not errors and not done
        self.log("plan: plug %d -> %s" % (plug_id, ", ".join("%s slot %d" % (TRACK_NAMES[t], s) for t, s in done)))
        if dry_run:
            self.log("dry run: nothing written.")
            return True
        if not yes and not self.confirm("Assign now?"):
            self.log("cancelled.")
            return False
        self.push([(SLOTS, json.dumps(doc, separators=(",", ":")).encode())])
        got = json.loads(self.read_settings()[1])
        if not all(x in set(assigned_tracks(got, plug_id)) for x in done):
            self.log("verify:  assignment missing; restoring the slot map from %s" % bdir)
            self.restore(bdir, yes=True)
            return False
        self.log("assigned and verified.")
        return True

    def remove(self, plug_id: int, unassign_first: bool = False, dry_run: bool = False, yes: bool = False,
               allow_te: bool = False) -> bool:
        bdir, plugs, slots = self.backup("remove-%d" % plug_id)
        record = next((p for p in plugs.get("plugs", []) if p["id"] == plug_id), None)
        if record is None:
            self.log("error:   plug %d is not installed" % plug_id)
            return False
        if plug_id < USER_PLUG_MIN and not allow_te:
            self.log("error:   plug %d is in TE's range; refusing" % plug_id)
            return False
        blocked = smp.removal_block(record, plugs)
        if blocked:
            self.log("error:   %s; refusing" % blocked)
            return False
        where = assigned_tracks(slots, plug_id)
        if where and not unassign_first:
            self.log("error:   plug %d is assigned to %s; use --unassign" % (
                plug_id, ", ".join("%s slot %d" % (TRACK_NAMES.get(t, t), s) for t, s in where)))
            return False
        if dry_run:
            self.log("dry run: would unassign %s and remove plug %d" % (where or "nothing", plug_id))
            return True
        if not yes and not self.confirm("Remove plug %d?" % plug_id):
            self.log("cancelled.")
            return False
        if where:
            self.push([(SLOTS, json.dumps(unassign(slots, plug_id), separators=(",", ":")).encode())])
            if assigned_tracks(json.loads(self.read_settings()[1]), plug_id):
                self.log("error:   unassigning failed; plug not removed")
                return False
        self.push([], remove=[plug_id])
        plugs2, slots2 = (json.loads(x) for x in self.read_settings())
        if any(p["id"] == plug_id for p in plugs2.get("plugs", [])):
            self.log("verify:  plug %d still present" % plug_id)
            return False
        self.log("removed plug %d. orphan engine files (no known delete): %s" % (plug_id, orphan_engines(plugs2) or "none"))
        return True

    def remove_ours(self, put_back_dir: str | None = None, dry_run: bool = False, yes: bool = False) -> bool:
        """Take every plug of ours off the OP-Z in three verified writes, one confirm: an empty boot file first (a
        stale one would bring removed plugs back at the next start, research/presets.md), the slot map without
        them (with put_back_dir: TE's plugs back in the cells they had in that backup), then one sync job of remove
        operations. The user's samples are never touched. Engine files stay (no normal-mode delete is known); they
        are a few KB each and content mode never shows them."""
        bdir, plugs, slots = self.backup("remove-ours")
        put_back = None
        if put_back_dir:
            with open(os.path.join(put_back_dir, SLOTS)) as f:
                put_back = json.load(f)
        plan = plan_remove_ours(plugs, slots, put_back)
        for pid, why in plan["kept"]:
            self.log("warning: plug %d stays: %s" % (pid, why))
        for e in plan["errors"]:
            self.log("error:   " + e)
        if plan["errors"]:
            self.log("nothing written.")
            return False
        if not plan["ids"]:
            self.log("no plugs of ours on the OP-Z: nothing to remove.")
            return True
        self.log("plan: remove %d plug%s of ours: %s" % (len(plan["ids"]), "s" if len(plan["ids"]) > 1 else "",
                                                       ", ".join(str(i) for i in plan["ids"])))
        refilled = {(t, s) for t, s, _ in plan["refilled"]}
        emptied = ["%s %d" % (TRACK_NAMES.get(t, t), s) for t, s, _ in plan["cleared"] if (t, s) not in refilled]
        if emptied:
            self.log("plan: empty %s" % ", ".join(emptied))
        if plan["refilled"]:
            self.log("plan: put back %s" % ", ".join("%s %d = %d" % (TRACK_NAMES.get(t, t), s, q)
                                                      for t, s, q in plan["refilled"]))
        self.log("plan: empty boot file %s; the engine files stay (no known delete)" % BOOT_PATCH)
        if dry_run:
            self.log("dry run: nothing written.")
            return True
        if not yes and not self.confirm("Remove %s of ours?" % ("all %d plugs" % len(plan["ids"]) if len(plan["ids"]) > 1 else "the 1 plug")):
            self.log("cancelled.")
            return False
        self.push([(BOOT_PATCH, build_boot_patch([]))])
        if plan["slot_map"] != slots:
            self.push([(SLOTS, json.dumps(plan["slot_map"], separators=(",", ":")).encode())])
            if json.loads(self.read_settings()[1]) != plan["slot_map"]:
                self.log("verify:  slot map differs; restoring the slot map from %s; no plug removed" % bdir)
                self.restore(bdir, yes=True)
                return False
        self.push([], remove=plan["ids"])
        plugs2, slots2 = (json.loads(x) for x in self.read_settings())
        left = [p["id"] for p in plugs2.get("plugs", []) if p["id"] in set(plan["ids"])]
        if left:
            self.log("verify:  still present: %s (run it again)" % ", ".join(str(i) for i in left))
            return False
        self.log("removed %d plug%s of ours and verified. The user's samples and TE's plugs are untouched.\n%s"
                 % (len(plan["ids"]), "s" if len(plan["ids"]) > 1 else "", describe_space(space_report(plugs2, slots2))))
        return True

    def restore(self, backup_dir: str, yes: bool = False, touch: bool = False) -> bool:
        """Upload the backed-up slot map. touch=True re-serialises it (same content, different bytes) so the OP-Z
        must really download it: a zero-change test of the upload path."""
        with open(os.path.join(backup_dir, SLOTS), "rb") as f:
            raw = f.read()
        want = json.loads(raw)
        if touch:
            raw = json.dumps(want, indent=1).encode() + b"\n"
        if not yes and not self.confirm("Restore the slot map from %s?" % backup_dir):
            self.log("cancelled.")
            return False
        result = self.push([(SLOTS, raw)])
        if touch and SLOTS in result["skipped"]:
            self.log("warning: the OP-Z did not request the touched slot map (upload path not exercised)")
        got = json.loads(self.read_settings()[1])
        ok = got == want
        self.log("slot map restore %s" % ("verified" if ok else "FAILED: readback differs"))
        return ok

    # -- sample packs ---------------------------------------------------------------------------------------------
    def samples_install(self, data: bytes, name: str, tracks: list[int], slot: int | None = None,
                        replace: bool = False, plug_id: int | None = None, version: int | None = None,
                        dry_run: bool = False, yes: bool = False) -> bool:
        """Put a pack on the OP-Z as a plug of ours, in three syncs: the pack file; the same file again, which the
        OP-Z must skip (proof that it holds those bytes, since samples cannot be read back: it requests a listed file
        unless crc32 of the file at that path equals the listed CRC, FUN_8012bb00); then patch.json (and the slot map
        with --slot). A plug never names a pack that is not on the device (SAFETY.md rule 4)."""
        bdir, plugs, slots = self.backup("samples-" + name)
        try:
            path = smp.pack_path(name)
        except smp.PackError as e:
            self.log("error:   %s" % e)
            return False
        pack = smp.parse(data)
        errors, warnings, plan = plan_samples_install(plugs, slots, pack, path, name, tracks, slot, replace,
                                                      plug_id, version)
        for w in warnings:
            self.log("warning: " + w)
        for e in errors:
            self.log("error:   " + e)
        if errors:
            self.log("nothing written.")
            return False
        patch = build_samples_patch(plan)
        self.log("plan: %s (%s pack, %.2f s, %d bytes) as plug %d v%d on %s -> %s" % (
            path, pack["kind"], pack["frames"] / smp.SAMPLE_RATE, len(data), plan["plug"]["id"],
            plan["plug"]["version"], plan["plug"]["engine"],
            ", ".join("%s slot %d" % (TRACK_NAMES[t], s) for t, s in plan["assign"]) or "no track"))
        self.log("our packs sit in samplepacks/, outside the 24 MB that content mode allows for samplepacks/user/; "
                 "nothing checks the flash for this upload, so keep packs small (`opz.py samples list`)")
        if dry_run:
            self.log("dry run: nothing written. patch.json:\n" + patch.decode())
            return True
        if not yes and not self.confirm("Upload the pack and install plug %d?" % plan["plug"]["id"]):
            self.log("cancelled.")
            return False
        result = self.push([(path, data)])
        self.log("sync 1 (pack): requested %s; skipped (already there) %s" % (result["requested"], result["skipped"]))
        probe = self.push([(path, data)])
        if path not in probe["skipped"]:
            self.log("error:   the OP-Z asked for the pack again, so the file at %s is not these bytes (it skips "
                     "a file only when the copy on its flash has the listed CRC): sync 1 did not land. No plug was "
                     "installed and nothing names the path" % path)
            return False
        self.log("sync 2 (probe): the OP-Z holds the pack (identical file skipped)")
        files = [(PATCH, patch)]
        if plan["slot_map"] is not None:
            files.append((SLOTS, json.dumps(plan["slot_map"], separators=(",", ":")).encode()))
        self.push(files)
        plugs2, slots2 = (json.loads(x) for x in self.read_settings())
        problems = verify_samples_install(plugs2, slots2, plan, before=slots)
        if problems:
            for p in problems:
                self.log("verify:  " + p)
            self.log("restoring the slot map from %s (the pack file stays: remove the plug when it appears, or "
                     "re-run this install)" % bdir)
            self.restore(bdir, yes=True)
            return False
        with open(os.path.join(bdir, "samples.json"), "w") as f:
            json.dump({"path": path, "sha256": hashlib.sha256(data).hexdigest(), "plug": plan["plug"],
                       "assigned": plan["assign"]}, f, indent=2)
        with open(os.path.join(bdir, os.path.basename(path)), "wb") as f:
            f.write(data)
        self.log("installed and verified.\n" + describe_samples(plugs2, slots2))
        return True

    def samples_remove(self, plug_id: int, unassign_first: bool = False, dry_run: bool = False,
                       yes: bool = False) -> bool:
        """Remove a sample plug of ours; the OP-Z deletes its pack file with it."""
        plugs = json.loads(self.read_settings()[0])
        record = next((p for p in plugs.get("plugs", []) if p["id"] == plug_id), None)
        if record is not None and not smp.is_our_sample(record):
            self.log("error:   plug %d is not a sample plug of ours (%s); refusing" % (
                plug_id, record.get("samples") or "no sample pack"))
            return False
        return self.remove(plug_id, unassign_first, dry_run, yes)

    def samples_purge(self, name: str, dry_run: bool = False, yes: bool = False) -> bool:
        """Delete a pack of ours that no plug names (plan_samples_purge): placeholder, skip-probe, a temporary plug of
        ours in no slot, then remove that plug, which deletes the file. Four syncs, each read back."""
        bdir, plugs, slots = self.backup("samples-purge-" + name)
        errors, warnings, plan, data = plan_samples_purge(plugs, slots, name)
        for w in warnings:
            self.log("warning: " + w)
        for e in errors:
            self.log("error:   " + e)
        if errors:
            self.log("nothing written.")
            return False
        path, pid = plan["plug"]["samples"], plan["plug"]["id"]
        self.log("plan: overwrite %s with a %d-byte placeholder, name it with plug %d (no slot), remove plug %d: "
                 "the OP-Z deletes the file with it" % (path, len(data), pid, pid))
        if dry_run:
            self.log("dry run: nothing written.")
            return True
        if not yes and not self.confirm("Purge %s?" % path):
            self.log("cancelled.")
            return False
        self.push([(path, data)])
        if path not in self.push([(path, data)])["skipped"]:
            self.log("error:   the OP-Z asked for the placeholder again, so it does not hold it (it skips a file "
                     "only when the copy on its flash has the listed CRC); nothing names the path, nothing else changed")
            return False
        self.push([(PATCH, build_samples_patch(plan))])
        plugs2, slots2 = (json.loads(x) for x in self.read_settings())
        problems = verify_samples_install(plugs2, slots2, plan, before=slots)
        if problems:
            for p in problems:
                self.log("verify:  " + p)
            self.log("stopped before removing anything (backup %s)" % bdir)
            return False
        if not self.remove(pid, yes=True):
            self.log("error:   plug %d names the placeholder and could not be removed: run `opz.py samples remove "
                     "%d` again" % (pid, pid))
            return False
        self.log("purged %s: the OP-Z deletes a removed plug's file (it cannot be listed to confirm)" % path)
        return True

    # -- presets --------------------------------------------------------------------------------------------------
    def presets_push(self, engine: dict, dry_run: bool = False, yes: bool = False, boot: bool = False) -> bool:
        """Replace an installed plug's presets: patch.json with only a plugs[] entry, so no version bump, no engine
        import and no slot change. Verified by reading back an unchanged plug record and slot map (the presets
        themselves are not readable). boot=True: the caller writes the boot file next."""
        bdir, plugs, slots = self.backup("presets-" + engine["name"])
        plug = engine["manifest"]["plug"]
        pid = int(plug["id"])
        record = next((p for p in plugs.get("plugs", []) if p["id"] == pid), None)
        errors, warnings = preset_problems(engine["presets"], int(plug.get("trackmask", 0)))
        if record is None:
            errors.insert(0, "plug %d is not installed (install it first)" % pid)
        elif record["engine"] != engine["device_path"]:
            errors.insert(0, "plug id %d belongs to %s" % (pid, record["engine"]))
        if not engine["presets"]:
            errors.append("presets.json is empty: nothing to push")
        for w in warnings:
            self.log("warning: " + w)
        for e in errors:
            self.log("error:   " + e)
        if errors:
            self.log("nothing written.")
            return False
        patch = build_presets_patch(record, engine["presets"])
        self.log("plan: %d presets -> plug %d (%s v%d) through patch.json plugs[] (no version bump)"
                 % (len(engine["presets"]), pid, record["engine"], record["version"]))
        if dry_run:
            self.log("dry run: nothing written. patch.json:\n" + patch.decode())
            return True
        if not yes and not self.confirm("Push presets now?"):
            self.log("cancelled.")
            return False
        result = self.push([(PATCH, patch)])
        if PATCH in result["skipped"]:
            self.log("the OP-Z did not fetch patch.json (it still holds an identical one); sending it with a nonce")
            result = self.push([(PATCH, build_presets_patch(record, engine["presets"], int(self.clock() * 1000)))])
            if PATCH in result["skipped"]:
                self.log("error:   the OP-Z did not fetch patch.json; presets not pushed")
                return False
        plugs2, slots2 = (json.loads(x) for x in self.read_settings())
        problems = []
        record2 = next((p for p in plugs2.get("plugs", []) if p["id"] == pid), None)
        if record2 != record:
            problems.append("plug record changed: %s -> %s" % (record, record2))
        if slots2 != slots:
            problems.append("slot map changed")
        if problems:
            for p in problems:
                self.log("verify:  " + p)
            if slots2 != slots:
                self.log("restoring the slot map from %s" % bdir)
                self.restore(bdir, yes=True)
            return False
        with open(os.path.join(bdir, "presets.json"), "w") as f:
            json.dump(device_presets(engine["presets"]), f, indent=2)
        self.log("presets pushed and verified (plug record and slot map unchanged; presets cannot be read back).")
        self.log("The OP-Z keeps them in RAM; the boot file written next brings them back after a restart "
                 "(research/presets.md)." if boot else
                 "The OP-Z keeps them until it restarts: push again after a restart (research/presets.md).")
        keys = preset_keys_text(engine["name"], engine["presets"], int(plug.get("trackmask", 0)))
        if keys:
            self.log(keys)
        return True

    def persist_presets(self, engines: list, dry_run: bool = False, yes: bool = False, off: bool = False) -> bool:
        """Write the boot file settings/plugs_patch.json: a plugs[] entry for every installed plug of ours that
        `engines` describes (boot_entries), or none with off=True. The OP-Z imports it at once (a file whose path
        contains "patch.json"), and again at its next start, before loading the preset caches; that start deletes it
        whether or not the import worked. So it brings the presets back after one restart and must be written again
        after each. It cannot be read back: verified by unchanged plug records and slot map; a copy goes into the
        backup."""
        bdir, plugs, slots = self.backup("boot-presets")
        entries, labels, warnings = ([], [], []) if off else boot_entries(plugs, engines)
        for w in warnings:
            self.log("warning: " + w)
        data = build_boot_patch(entries)
        if len(data) > BOOT_PATCH_MAX:
            self.log("error:   the boot file would be %d bytes (limit %d); nothing written." % (len(data), BOOT_PATCH_MAX))
            return False
        if entries:
            self.log("plan: boot file %s (%d bytes), %d plug%s (presets): %s. The OP-Z imports it now (the same "
                     "presets, no version bump) and again at its next start, which deletes it"
                     % (BOOT_PATCH, len(data), len(entries), "s" if len(entries) > 1 else "", ", ".join(labels)))
        else:
            self.log("plan: empty boot file %s: the OP-Z imports nothing now or at its next start, which deletes it"
                     % BOOT_PATCH)
        if dry_run:
            self.log("dry run: nothing written. plugs_patch.json:\n" + data.decode())
            return True
        if not yes and not self.confirm("Write the boot file now?"):
            self.log("cancelled.")
            return False
        result = self.push([(BOOT_PATCH, data)])
        self.log("sync: requested %s; skipped (unchanged) %s" % (result["requested"], result["skipped"]))
        if BOOT_PATCH in result["skipped"]:
            self.log("the OP-Z already holds this boot file (no restart since it was written): unchanged")
        plugs2, slots2 = (json.loads(x) for x in self.read_settings())
        before = {p["id"]: p for p in plugs.get("plugs", [])}
        after = {p["id"]: p for p in plugs2.get("plugs", [])}
        problems = ["plug %d changed: %s -> %s" % (pid, before.get(pid), after.get(pid))
                    for pid in sorted(set(before) | set(after)) if before.get(pid) != after.get(pid)]
        if slots2 != slots:
            problems.append("slot map changed")
        if problems:
            for p in problems:
                self.log("verify:  " + p)
            if slots2 != slots:
                self.log("restoring the slot map from %s" % bdir)
                self.restore(bdir, yes=True)
            self.log("to empty the boot file before the next start: opz.py presets persist --off")
            return False
        with open(os.path.join(bdir, "plugs_patch.json"), "wb") as f:
            f.write(data)
        if entries:
            self.log("boot file written and verified (plug records and slot map unchanged; the file cannot be read "
                     "back). At its next start the OP-Z restores these presets without a push and deletes the file: "
                     "after a restart, run opz.py presets persist (or any install or presets push) to cover the next.")
        else:
            self.log("boot file emptied and verified.")
        return True

    @contextlib.contextmanager
    def state_session(self, tracks=()):
        s = StateSession(self.transport, log=lambda m: self.log("  " + m), clock=self.clock)
        s.open(tracks)
        try:
            yield s
        finally:
            s.close()

    def audition(self, track: int, values, hold, label: str = "sound", yes: bool = False) -> bool:
        """Play `values` on `track` through `$0E` while hold(session) runs, then put the track's sound back."""
        values = check_sound(track, values)
        with self.state_session([track]) as s:
            original = s.sounds[track]
            self.log("track %s now: %s" % (TRACK_NAMES[track], format_sound(original)))
            self.log("if this is interrupted, put it back with: " + set_command(track, original))
            if not yes and not self.confirm("Audition %s on track %s (its sound comes back afterwards)?"
                                            % (label, TRACK_NAMES[track])):
                self.log("cancelled.")
                return False
            s.set(track, values)
            self.log("auditioning %s on track %s" % (label, TRACK_NAMES[track]))
            try:
                hold(s)
            finally:
                s.set(track, original)
                self.log("track %s restored" % TRACK_NAMES[track])
        return True

    def set_sound(self, track: int, values, dry_run: bool = False, yes: bool = False) -> bool:
        """Set a track's sound in the active pattern through `$0E` and leave it (saved with the project)."""
        values = check_sound(track, values)
        with self.state_session([track]) as s:
            original = s.sounds[track]
            self.log("track %s now: %s" % (TRACK_NAMES[track], format_sound(original)))
            self.log("plan:    track %s -> %s" % (TRACK_NAMES[track], format_sound(values)))
            if dry_run:
                self.log("dry run: nothing written.")
                return True
            if not yes and not self.confirm("Set track %s's sound (changes the active pattern)?" % TRACK_NAMES[track]):
                self.log("cancelled.")
                return False
            s.set(track, values)
            s.pump(0.5)
            self.log("set. to undo: " + set_command(track, original))
        return True

    def watch(self, tracks=(), seconds: float | None = None, knobs=None) -> bool:
        """Print the sounds the OP-Z reports (`$0E`): the current ones, then every change. Read-only."""
        with self.state_session(tracks) as s:
            for t in sorted(s.sounds):
                if not tracks or t in tracks:
                    self.log("%-7s %s" % (TRACK_NAMES.get(t, t), format_sound(s.sounds[t], knobs)))
            waited = 0.0
            while seconds is None or waited < seconds:
                for t, v in s.pump(1.0):
                    if not tracks or t in tracks:
                        self.log("%-7s %s" % (TRACK_NAMES.get(t, t), format_sound(v, knobs)))
                waited += 1.0
        return True

    # projects (research/patterns.md) ----------------------------------------------------------------------
    def pull_projects(self, numbers) -> dict[int, bytes]:
        """Read project files over the file service (ids 3-12): what the OP-Z last saved, not unsaved RAM edits."""
        with self.session() as s:
            return {n: s.pull(proj.project_path(n), file_id=proj.project_file_id(n)) for n in numbers}

    def backup_projects(self, numbers=range(1, proj.PROJECTS + 1), label: str = "projects") -> tuple[str, dict]:
        files = self.pull_projects(numbers)
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        d = os.path.join(self.backups, stamp + "-" + label)
        os.makedirs(os.path.join(d, "projects"), exist_ok=True)
        meta = {}
        for n, raw in files.items():
            path = proj.project_path(n)
            with open(os.path.join(d, path), "wb") as f:
                f.write(raw)
            meta[path] = hashlib.sha256(raw).hexdigest()
            try:
                p = proj.Project.parse(raw)
                note = "version %s, %s" % (p.version, "patterns with notes %s" % [x + 1 for x in
                                                                                   p.summary()["patterns_with_notes"]])
            except proj.FormatError as e:
                note = "UNREADABLE: %s" % e
            self.log("  %s: %d bytes, %s" % (path, len(raw), note))
        with open(os.path.join(d, "backup.json"), "w") as f:
            json.dump({"sha256": meta}, f, indent=2)
        self.log("backup: %s" % d)
        return d, files

    def put_project(self, number: int, data: bytes, overwrite: bool = False, dry_run: bool = False,
                    yes: bool = False) -> bool:
        """Replace project `number` (1-10) with a checked file: back up all ten, refuse a project with notes unless
        `overwrite`, write, read back, and put the backup back if the read-back differs."""
        path = proj.project_path(number)
        try:
            new = proj.Project.parse(data)
        except proj.FormatError as e:
            self.log("error:   %s" % e)
            return False
        problems = new.problems()
        for p in problems[:20]:
            self.log("error:   " + p)
        if problems:
            self.log("nothing written: the file has %d problem(s) (opz_project.py check)." % len(problems))
            return False
        bdir, before = self.backup_projects(label="put-project%02d" % number)
        old_raw = before[number]
        if old_raw == data:
            self.log("project %d already holds this file: nothing to do." % number)
            return True
        try:
            old = proj.Project.parse(old_raw)
            busy = old.summary()["patterns_with_notes"]
        except proj.FormatError:
            busy = ["unreadable"]
        if busy and not overwrite:
            self.log("error:   project %d is not spare (patterns with notes: %s); choose an empty project or pass "
                     "--overwrite (the backup above can put it back)" % (number, [b + 1 if isinstance(b, int) else b
                                                                                 for b in busy]))
            return False
        self.log("plan: %s <- %d bytes, version %s, tempo %d, patterns with notes %s" % (
            path, len(data), new.version, new.tempo, [x + 1 for x in new.summary()["patterns_with_notes"]]))
        self.log("The OP-Z reloads the project at once; unsaved changes to project %d on the device are lost. Stop "
                 "playback and keep another project open on the device." % number)
        if dry_run:
            self.log("dry run: nothing written.")
            return True
        if not yes and not self.confirm("Write project %d now?" % number):
            self.log("cancelled.")
            return False
        self.push([(path, data)])
        got = self.pull_projects([number])[number]
        if got == data:
            self.log("written and verified (sha256 %s). to undo: opz.py projects put %d %s --overwrite"
                     % (hashlib.sha256(data).hexdigest()[:16], number, os.path.join(bdir, path)))
            return True
        self.log("verify:  read-back differs from what was sent; putting the backup back")
        self.push([(path, old_raw)])
        back = self.pull_projects([number])[number]
        self.log("rollback %s" % ("verified" if back == old_raw else "FAILED: the backup is in %s" % bdir))
        return False


def _sound_from_args(a) -> tuple[list[int], str, list | None]:
    """(values, label, knob names) for presets audition/set from --values or ENGINE_DIR + --preset/--slot/--index."""
    if getattr(a, "values", None):
        return [int(v) for v in a.values.split(",")], "sound", None
    if not a.engine_dir:
        raise PlanError("give --values or ENGINE_DIR with --preset NAME (or --slot S --index N)")
    engine = load_manifest(a.engine_dir)
    p = find_preset(engine["presets"], a.preset, a.slot, a.index)
    return list(p["values"]), "%s preset %r" % (engine["name"], p.get("name", "")), \
        engine["manifest"]["plug"].get("parameter_names")


def _hold_until_enter(seconds):
    def hold(session):
        if seconds is not None:
            session.pump(seconds)
            return
        import threading
        done = threading.Event()
        threading.Thread(target=lambda: (input("press Enter to stop... "), done.set()), daemon=True).start()
        try:
            while not done.is_set():
                session.pump(0.25)
        except KeyboardInterrupt:
            pass
    return hold


USER_PRESETS = "config/userPresets.dat"


def _write_checked(path: str, data: bytes) -> bool:
    """Write in place (no temporary file: the OP-Z would take one for a config file of the wrong type and move it to
    rejected/), sync, and read back."""
    with open(path, "r+b") as f:
        f.write(data)
        f.truncate(len(data))
        f.flush()
        os.fsync(f.fileno())
    with open(path, "rb") as f:
        return f.read() == data


def find_content_disks(volumes: str = "/Volumes") -> list[str]:
    """Mounted volumes that look like the OP-Z's content-mode disk (named OP-Z…, with a config folder)."""
    try:
        names = sorted(os.listdir(volumes))
    except OSError:
        return []
    return [os.path.join(volumes, n) for n in names
            if n.lower().startswith("op-z") and os.path.isdir(os.path.join(volumes, n, "config"))]


def bake_presets(a, log=print, confirm=None, backups=None) -> int:
    """opz.py presets bake: offline, on the content-mode disk (or a copy of the file). Never touches MIDI."""
    confirm = confirm or (lambda q: input(q + " [y/N] ").strip().lower() == "y")
    if a.disk and not os.path.isdir(a.disk):
        found = find_content_disks()
        if len(found) != 1:
            log("error:   no disk at %s. %s" % (a.disk, "Mounted OP-Z disks: %s; pass one with --disk." % ", ".join(found)
                if found else "Start content mode (hold track while switching on) and wait for the disk to mount."))
            return 1
        log("no disk at %s; using the mounted content-mode disk %s" % (a.disk, found[0]))
        a.disk = found[0]
    path = a.file or os.path.join(a.disk, USER_PRESETS)
    if not os.path.exists(path):
        log("error:   %s not found. The OP-Z writes it when the first preset is stored by hand: store one (hold track "
            "+ a white key 2 s), then start content mode again (hold track at power-on)." % path)
        return 1
    with open(path, "rb") as f:
        data = f.read()
    try:
        records = userpresets.parse(data)
        if userpresets.serialise(records) != data:
            raise userpresets.FormatError("the file does not read back byte for byte")
    except userpresets.FormatError as e:
        log("error:   %s: %s. Nothing written." % (path, e))
        return 1
    log("read %s (%d bytes, sha256 %s)" % (path, len(data), hashlib.sha256(data).hexdigest()[:16]))
    for line in userpresets.summary(records):
        log(line)
    if a.show:
        return 0
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    bdir = os.path.join(backups or os.path.join(ROOT, "backups"), stamp + "-userpresets")
    os.makedirs(bdir, exist_ok=True)
    with open(os.path.join(bdir, "userPresets.dat"), "wb") as f:
        f.write(data)
    with open(os.path.join(bdir, "backup.json"), "w") as f:
        json.dump({"source": os.path.abspath(path), "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}, f,
                  indent=2)
    log("backup: %s" % bdir)
    if a.restore:
        src = os.path.join(a.restore, "userPresets.dat") if os.path.isdir(a.restore) else a.restore
        with open(src, "rb") as f:
            new = f.read()
        try:
            userpresets.parse(new)
        except userpresets.FormatError as e:
            log("error:   %s: %s. Nothing written." % (src, e))
            return 1
        log("plan: put %s back (sha256 %s)" % (src, hashlib.sha256(new).hexdigest()[:16]))
        engines_keys: list = []
    else:
        engines = [load_manifest(d) for d in a.engine_dir] if a.engine_dir else catalogue()
        plugs, engines_keys = [], []
        for e in engines:
            if not e["presets"]:
                continue
            if preset_problems(e["presets"], int(e["manifest"]["plug"].get("trackmask", 0)))[0]:
                log("warning: %s: presets.json has errors (opz.py presets check): left out" % e["name"])
                continue
            plugs.append((int(e["manifest"]["plug"]["id"]), e["name"], e["presets"]))
            engines_keys.append(e)
        after = [bytearray(r) for r in records]
        try:
            report = userpresets.bake(after, plugs, a.overwrite_user)
        except userpresets.FormatError as e:
            log("error:   %s. Nothing written." % e)
            return 1
        for name, r in report.items():
            kept = "; kept your stored " + ", ".join("slot %d index %d %r" % k for k in r["kept"]) if r["kept"] else ""
            log("plan: %s: %d new, %d updated (from an earlier bake), %d replaced, %d already there%s"
                % (name, r["new"], r["updated"], r["replaced"], r["same"], kept))
        new = userpresets.serialise(after)
        if new == data:
            log("nothing to change: these presets are already in the file as user presets.")
            return 0
        log("plan: %d of 200 records change; every other byte stays" % len(userpresets.changed_records(records, after)))
    if a.dry_run:
        log("dry run: nothing written.")
        return 0
    if not a.yes and not confirm("Write %s?" % path):
        log("cancelled.")
        return 1
    dot = os.path.join(os.path.dirname(path), "._userPresets.dat")
    dot_before = os.path.exists(dot)
    if not _write_checked(path, new):
        log("verify:  the file read back differently; putting the original back")
        _write_checked(path, data)
        return 1
    log("written and read back (sha256 %s)." % hashlib.sha256(new).hexdigest()[:16])
    if os.path.exists(dot):
        with open(dot, "rb") as f:
            apple_double = f.read(4) == b"\x00\x05\x16\x07"
        if apple_double and not dot_before:
            os.remove(dot)          # macOS wrote it with this write (file attributes on a FAT disk): ours to clear
            log("removed config/._userPresets.dat (macOS file attributes, written with this write)")
        else:
            log("warning: config/._userPresets.dat is on the disk; the OP-Z will move it to rejected/ (harmless).")
    log("Now eject the OP-Z disk: the OP-Z imports the file, reloads its presets and restarts. They are user presets "
        "from then on and stay with nothing connected. To undo: content mode again, then opz.py presets bake --disk "
        "<volume> --restore %s" % bdir)
    for e in engines_keys:
        keys = preset_keys_text(e["name"], e["presets"], int(e["manifest"]["plug"].get("trackmask", 0)))
        if keys:
            log(keys)
    return 0


def run_presets(opz: "Opz", a) -> int:
    if a.pcmd == "push":
        engines = [load_manifest(d) for d in a.engine_dir]
        boot = not a.no_boot and not a.dry_run
        ok = [opz.presets_push(e, a.dry_run, a.yes, boot) for e in engines]
        if a.dry_run and not a.no_boot:
            opz.log("then the boot file %s is written again (opz.py presets persist --dry-run shows it)" % BOOT_PATCH)
        if boot and any(ok) and not opz.persist_presets(catalogue(first=engines), yes=a.yes):
            return 1
        return 0 if all(ok) else 1
    if a.pcmd == "persist":
        engines = [load_manifest(d) for d in a.engine_dir] if a.engine_dir else catalogue()
        return 0 if opz.persist_presets(engines, a.dry_run, a.yes, a.off) else 1
    if a.pcmd == "watch":
        try:
            opz.watch(a.track or [], a.seconds)
        except KeyboardInterrupt:
            pass
        return 0
    values, label, _ = _sound_from_args(a)
    if a.pcmd == "audition":
        return 0 if opz.audition(a.track, values, _hold_until_enter(a.hold), label, a.yes) else 1
    return 0 if opz.set_sound(a.track, values, a.dry_run, a.yes) else 1


def connect(args):
    if args.fake:
        from fake_opz import FakeOpz
        return contextlib.nullcontext(FakeOpz())
    return MidoTransport(args.port)


def _test_args(argv: list[str]) -> list[str] | None:
    """`opz.py [--port P] [--fake] test ...` -> the arguments for opz_bench.main, else None."""
    i, pre = 0, []
    while i < len(argv) and argv[i].startswith("--"):
        if argv[i] == "--fake":
            pre.append("--fake")
            i += 1
        elif argv[i] == "--port" and i + 1 < len(argv):
            pre += ["--port", argv[i + 1]]
            i += 2
        else:
            return None
    if i < len(argv) and argv[i] == "test":
        return argv[i + 1:] + pre
    return None


def main(argv=None) -> int:
    bench_args = _test_args(sys.argv[1:] if argv is None else list(argv))
    if bench_args is not None:
        import opz_bench
        return opz_bench.main(bench_args)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="OP-Z")
    ap.add_argument("--fake", action="store_true", help="use an in-memory OP-Z (no hardware)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ports")
    nw = sub.add_parser("new", help="start an engine from an SDK template (offline)")
    opz_new.add_arguments(nw)
    b = sub.add_parser("backup")
    b.add_argument("--out", default=os.path.join(ROOT, "backups"))
    st = sub.add_parser("status")
    st.add_argument("--from", dest="source")
    i = sub.add_parser("install")
    i.add_argument("engine_dir")
    i.add_argument("--track", action="append", type=track_index, help="name or 0..11; repeatable (default: none)")
    i.add_argument("--slot", type=int, choices=range(1, 11))
    i.add_argument("--replace", action="store_true")
    i.add_argument("--engine", help="engine file (default build/bfin/NAME/NAME.engine)")
    i.add_argument("--no-boot", action="store_true", help="don't write the boot file (presets after a restart)")
    i.add_argument("--dry-run", action="store_true")
    i.add_argument("--yes", action="store_true")
    asg = sub.add_parser("assign")
    asg.add_argument("plug_id", type=int)
    asg.add_argument("--track", action="append", type=track_index, required=True)
    asg.add_argument("--slot", type=int, choices=range(1, 11))
    asg.add_argument("--replace", action="store_true")
    asg.add_argument("--dry-run", action="store_true")
    asg.add_argument("--yes", action="store_true")
    rc = sub.add_parser("restore-check", help="offline: will the OP-Z take this backup of its disk in content mode?")
    rc.add_argument("backup", help="the folder you copied off the OP-Z disk (samplepacks/, projects/, ...)")
    rc.add_argument("--disk", help="the OP-Z disk as it is now (content mode), e.g. /Volumes/OP-Z")
    rc.add_argument("--source", help="an opz.py backup folder with the OP-Z's settings (default: the newest in backups/)")
    rc.add_argument("--log", help="the disk's import.log: what the last ejects refused")
    sp_ = sub.add_parser("space", help="plug definitions and our engine files (read-only)")
    sp_.add_argument("--source", help="a backup folder instead of the OP-Z")
    r = sub.add_parser("remove")
    r.add_argument("plug_id", type=int, nargs="?")
    r.add_argument("--all-ours", action="store_true",
                   help="every plug of ours (60900-61999), the user's samples and TE's plugs untouched")
    r.add_argument("--put-back", metavar="BACKUP_DIR",
                   help="with --all-ours: TE's plugs back into the cells they had in this backup")
    r.add_argument("--unassign", action="store_true")
    r.add_argument("--no-boot", action="store_true", help="don't write the boot file again without this plug")
    r.add_argument("--dry-run", action="store_true")
    r.add_argument("--yes", action="store_true")
    rs = sub.add_parser("restore")
    rs.add_argument("backup_dir")
    rs.add_argument("--yes", action="store_true")
    rs.add_argument("--touch", action="store_true", help="re-serialise the slot map so the OP-Z must download it")
    pr = sub.add_parser("presets", aliases=["preset"], help="presets and live sound ($0E)")
    psub = pr.add_subparsers(dest="pcmd", required=True)
    pc = psub.add_parser("check", help="check presets.json offline")
    pc.add_argument("engine_dir")
    pp = psub.add_parser("push", help="replace installed plugs' presets (no version bump); after every restart")
    pp.add_argument("engine_dir", nargs="+")
    pp.add_argument("--no-boot", action="store_true", help="don't write the boot file (presets after a restart)")
    pp.add_argument("--dry-run", action="store_true")
    pp.add_argument("--yes", action="store_true")
    pb = psub.add_parser("bake", help="offline, in content mode: our presets into config/userPresets.dat as user presets")
    pb.add_argument("engine_dir", nargs="*", help="only these engines (default: every engine under engines/)")
    where = pb.add_mutually_exclusive_group(required=True)
    where.add_argument("--disk", help="the mounted content-mode disk, e.g. /Volumes/OP-Z (found by itself if that path is not there)")
    where.add_argument("--file", help="a userPresets.dat file (a copy, for trying it out)")
    pb.add_argument("--show", action="store_true", help="only list the user presets in the file (read-only)")
    pb.add_argument("--restore", help="put a backed-up userPresets.dat (file or backup directory) back")
    pb.add_argument("--overwrite-user", action="store_true", help="replace presets you stored by hand at the same keys")
    pb.add_argument("--dry-run", action="store_true")
    pb.add_argument("--yes", action="store_true")
    ps = psub.add_parser("persist", help="write the boot file: the OP-Z restores our plugs' presets at its next start")
    ps.add_argument("engine_dir", nargs="*", help="only these engines (default: every engine under engines/)")
    ps.add_argument("--off", action="store_true", help="write an empty boot file (nothing restored)")
    ps.add_argument("--dry-run", action="store_true")
    ps.add_argument("--yes", action="store_true")
    for name, helptext in (("audition", "play a preset on a track, then put the sound back"),
                           ("set", "set a track's sound in the active pattern")):
        q = psub.add_parser(name, help=helptext)
        q.add_argument("engine_dir", nargs="?" if name == "set" else None)
        q.add_argument("--track", type=track_index, required=True)
        q.add_argument("--preset", help="preset name")
        q.add_argument("--slot", type=int)
        q.add_argument("--index", type=int)
        q.add_argument("--yes", action="store_true")
        if name == "audition":
            q.add_argument("--hold", type=float, help="seconds (default: until Enter)")
        else:
            q.add_argument("--values", help="18 comma-separated values 0..255")
            q.add_argument("--dry-run", action="store_true")
    pw = psub.add_parser("watch", help="print the sounds the OP-Z reports (read-only)")
    pw.add_argument("--track", action="append", type=track_index)
    pw.add_argument("--seconds", type=float)
    sm = sub.add_parser("samples", aliases=["sample"], help="OP-1 format drum kits and synth samples")
    ssub = sm.add_subparsers(dest="scmd", required=True)
    sb = ssub.add_parser("build", help="build a drum pack from WAVs (offline)")
    sb.add_argument("out")
    sb.add_argument("--wav", action="append", help="a sound, in key order; repeatable (up to 24)")
    sb.add_argument("--dir", help="every .wav in DIR, sorted by name")
    sb.add_argument("--test-kit", nargs="?", const="full", choices=["full", "tiny"],
                    help="24 sounds synthesised by this tool (tiny: 24 blips of 40 ms, about 85 KB)")
    sb.add_argument("--name", help="name stored in the pack (default: the output file name)")
    sc = ssub.add_parser("check", help="check a pack the way the OP-Z reads it (offline)")
    sc.add_argument("file")
    sl = ssub.add_parser("list", help="sample plugs on the OP-Z: ours, the user's, TE's")
    sl.add_argument("--from", dest="source")
    si = ssub.add_parser("install", help="upload a pack as a plug of ours")
    si.add_argument("file")
    si.add_argument("--name", required=True, help="becomes samplepacks/opzk_NAME.aif")
    si.add_argument("--track", action="append", type=track_index, required=True)
    si.add_argument("--slot", type=int, choices=range(1, 11))
    si.add_argument("--replace", action="store_true")
    si.add_argument("--plug-id", type=int, help="61400-61499 (default: the first free)")
    si.add_argument("--version", type=int, help="plug version (default 1, or installed + 1 when updating)")
    si.add_argument("--dry-run", action="store_true")
    si.add_argument("--yes", action="store_true")
    sr = ssub.add_parser("remove", help="remove a sample plug of ours (and its pack file)")
    sr.add_argument("plug_id", type=int)
    sr.add_argument("--unassign", action="store_true")
    sr.add_argument("--dry-run", action="store_true")
    sr.add_argument("--yes", action="store_true")
    pj = sub.add_parser("projects", aliases=["project"], help="project files over the file service")
    pjsub = pj.add_subparsers(dest="jcmd", required=True)
    jb = pjsub.add_parser("backup", help="read projects (default all ten) into backups/<time>-projects/ (read-only)")
    jb.add_argument("numbers", nargs="*", type=int)
    jb.add_argument("--out", default=os.path.join(ROOT, "backups"))
    jp = pjsub.add_parser("put", help="replace one project with a checked .opz file")
    jp.add_argument("number", type=int)
    jp.add_argument("file")
    jp.add_argument("--overwrite", action="store_true", help="allow a project that has notes")
    jp.add_argument("--dry-run", action="store_true")
    jp.add_argument("--yes", action="store_true")
    sp = ssub.add_parser("purge", help="delete a pack of ours that no plug names (after an install stopped)")
    sp.add_argument("name", help="the NAME of samplepacks/opzk_NAME.aif")
    sp.add_argument("--dry-run", action="store_true")
    sp.add_argument("--yes", action="store_true")
    a = ap.parse_args(argv)

    if a.cmd == "new":
        return opz_new.run(a)
    try:
        if a.cmd == "ports":
            from opzsysex.device import list_ports
            ins, outs = list_ports()
            print("inputs: ", ins, "\noutputs:", outs)
            return 0
        if a.cmd in ("presets", "preset") and a.pcmd == "bake":
            return bake_presets(a)
        if a.cmd in ("presets", "preset") and a.pcmd == "check":
            engine = load_manifest(a.engine_dir)
            errors, warnings = preset_problems(engine["presets"], int(engine["manifest"]["plug"].get("trackmask", 0)))
            for p in engine["presets"]:
                print("  slot %s index %s %-12s octave %+d  %s" % (
                    p.get("slot", 0), p.get("index", 0), p.get("name", ""),
                    int(p.get("defaultOctave", p.get("default_octave", 0))),
                    format_sound(p.get("values", []), engine["manifest"]["plug"].get("parameter_names"))))
            for w in warnings:
                print("warning: " + w)
            for e in errors:
                print("error:   " + e)
            print("%d presets, %d errors, %d warnings" % (len(engine["presets"]), len(errors), len(warnings)))
            return 1 if errors else 0
        if a.cmd == "status" and a.source:
            with open(os.path.join(a.source, PLUGS)) as f1, open(os.path.join(a.source, SLOTS)) as f2:
                print(describe(json.load(f1), json.load(f2)))
            return 0
        if a.cmd == "restore-check":
            return restore_check(a)
        if a.cmd == "space" and a.source:
            with open(os.path.join(a.source, PLUGS)) as f1, open(os.path.join(a.source, SLOTS)) as f2:
                print(describe_space(space_report(json.load(f1), json.load(f2), engine_sizes())))
            return 0
        if a.cmd == "remove" and (a.plug_id is None) != bool(a.all_ours):
            print("error: give a plug id or --all-ours (not both)", file=sys.stderr)
            return 2
        if a.cmd == "remove" and a.put_back and not a.all_ours:
            print("error: --put-back goes with --all-ours", file=sys.stderr)
            return 2
        if a.cmd in ("samples", "sample") and a.scmd == "build":
            sounds, notes = samples_from_args(a)
            name = a.name or os.path.splitext(os.path.basename(a.out))[0]
            data = smp.build_drum_pack(sounds, name)
            os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
            with open(a.out, "wb") as f:
                f.write(data)
            for n in notes:
                print("note:    " + n)
            print("wrote %s: %d sounds on 24 keys, %.2f s, %d bytes" % (
                a.out, len(sounds), sum(len(s) for s in sounds) / smp.SAMPLE_RATE, len(data)))
            return 0
        if a.cmd in ("samples", "sample") and a.scmd == "check":
            with open(a.file, "rb") as f:
                pack = smp.parse(f.read())
            print("%s: %s, %d frames (%.2f s), %d ch, %d-bit, %g Hz" % (
                a.file, pack["kind"], pack["frames"], pack["frames"] / smp.SAMPLE_RATE, pack["channels"],
                pack["bits"], pack["rate"]))
            for i, (s, e) in enumerate(pack["keys"]):
                print("  key %2d  frames %6d-%6d  %.3f s" % (i + 1, s, e, (e - s) / smp.SAMPLE_RATE))
            for w in pack["warnings"]:
                print("warning: " + w)
            for e in pack["errors"]:
                print("error:   " + e)
            return 1 if pack["errors"] else 0
        if a.cmd in ("samples", "sample") and a.scmd == "list" and a.source:
            with open(os.path.join(a.source, PLUGS)) as f1, open(os.path.join(a.source, SLOTS)) as f2:
                print(describe_samples(json.load(f1), json.load(f2)))
            return 0
        pack_data = None
        if a.cmd in ("samples", "sample") and a.scmd == "install":
            with open(a.file, "rb") as f:
                pack_data = f.read()
        engine = load_engine(a.engine_dir, a.engine) if a.cmd == "install" else None
        project_data = None
        if a.cmd in ("projects", "project"):
            for n in (a.numbers if a.jcmd == "backup" else [a.number]):
                proj.project_path(n)
            if a.jcmd == "put":
                with open(a.file, "rb") as f:
                    project_data = f.read()
        with connect(a) as transport:
            opz = Opz(transport, backups=getattr(a, "out", os.path.join(ROOT, "backups")))
            if a.cmd == "backup":
                opz.backup()
                return 0
            if a.cmd == "status":
                plugs, slots = (json.loads(x) for x in opz.read_settings())
                print(describe(plugs, slots))
                return 0
            if a.cmd == "install":
                boot = not a.no_boot and not a.dry_run
                if not opz.install(engine, a.track or [], a.slot, a.replace, a.dry_run, a.yes, boot):
                    return 1
                if a.dry_run and not a.no_boot:
                    opz.log("after the install the boot file %s is written again (opz.py presets persist --dry-run "
                            "shows it then)" % BOOT_PATCH)
                return 0 if not boot or opz.persist_presets(catalogue(first=[engine]), yes=a.yes) else 1
            if a.cmd == "assign":
                return 0 if opz.assign(a.plug_id, a.track, a.slot, a.replace, a.dry_run, a.yes,
                                         catalogue() if max(a.track) >= FX_TRACK_MIN else None) else 1
            if a.cmd == "space":
                plugs, slots = (json.loads(x) for x in opz.read_settings())
                print(describe_space(space_report(plugs, slots, engine_sizes())))
                return 0
            if a.cmd == "remove" and a.all_ours:
                return 0 if opz.remove_ours(a.put_back, a.dry_run, a.yes) else 1
            if a.cmd == "remove":
                if not opz.remove(a.plug_id, a.unassign, a.dry_run, a.yes):
                    return 1
                if a.dry_run or a.no_boot:
                    return 0
                return 0 if opz.persist_presets(catalogue(), yes=a.yes) else 1
            if a.cmd == "restore":
                return 0 if opz.restore(a.backup_dir, a.yes, a.touch) else 1
            if a.cmd in ("presets", "preset"):
                return run_presets(opz, a)
            if a.cmd in ("samples", "sample"):
                if a.scmd == "list":
                    plugs, slots = (json.loads(x) for x in opz.read_settings())
                    print(describe_samples(plugs, slots))
                    return 0
                if a.scmd == "install":
                    return 0 if opz.samples_install(pack_data, a.name, a.track, a.slot, a.replace, a.plug_id,
                                                    a.version, a.dry_run, a.yes) else 1
                if a.scmd == "purge":
                    return 0 if opz.samples_purge(a.name, a.dry_run, a.yes) else 1
                return 0 if opz.samples_remove(a.plug_id, a.unassign, a.dry_run, a.yes) else 1
            if a.cmd in ("projects", "project"):
                if a.jcmd == "backup":
                    opz.backup_projects(a.numbers or range(1, proj.PROJECTS + 1))
                    return 0
                return 0 if opz.put_project(a.number, project_data, a.overwrite, a.dry_run, a.yes) else 1
    except (SessionError, PlanError, smp.PackError, proj.FormatError, OSError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
