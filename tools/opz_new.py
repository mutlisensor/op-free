#!/usr/bin/env python3
"""Start a new engine from an SDK template: engines/NAME/ with a free test plug id and memory slot.

Usage:
    opz_new.py NAME --kind synth|drum|fx [--size auto|5|10|50] [--slot N] [--plug-id ID] [--dry-run]
    opz.py new NAME --kind ...                       (the same command)

Copies sdk/templates/<kind>/ (synth: the synth tracks; drum: the drum tracks; fx: FX1 and FX2) to engines/NAME/,
renames engine.c to NAME.c and fills in the name, the plug id and the memory slot. The plug id is the first free
test id (60900-60999). The slot follows sdk/docs/memory-slots.md: this project's range of the class, top down,
skipping every slot an engine under engines/ or a row of the allocation table already holds; --size auto takes the
5 KB class and falls back to 10 KB when it is full (50 KB slots are scarce: ask for them with --size 50). TE's
slots, slot 0 and slots above 89 are refused. The allocation is recorded as a row of the table in memory-slots.md,
as the project's rules ask. Nothing talks to an OP-Z.

Python 3 standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

KINDS = ("synth", "drum", "fx")
TEMPLATE_NAME = "my_engine"                 # the placeholder name in every template file
TEMPLATE_SOURCE = "engine.c"

# sdk/docs/memory-slots.md
TE_SLOTS = {4, 9, 10, 13, 16, 18, 27, 59, 60, 61, 62, 66, 68, 70, 77, 78, 79, 82, 83, 88, 89}
OUR_SLOTS = {                               # allocation policy, top down
    50: [7, 6, 5, 3, 2],
    10: list(range(57, 27, -1)),
    5: [87, 86, 85, 84, 81, 80, 76, 75, 74, 73, 72, 71],
}
MAX_SLOT = 89                               # 90-255 closed until hardware test H-16 passes
TEST_IDS = range(60900, 61000)
RELEASED_ID_MIN = 61000
# the same names as tools/opz.py TE_ENGINES (a unit test keeps them equal)
TE_ENGINES = {"analoga", "chord", "chorus", "chorus2", "cluster", "crush", "delay", "digital", "dist", "electric",
              "ep", "fdnreverb", "fm003b", "ministringshort", "organ", "reverb", "sampleplayergain", "saw013a",
              "synthsampler", "tapetwo", "volt"}
FORBIDDEN_WORDS = ("teenage", "mutable")    # CONTRIBUTING.md: no TE or Mutable Instruments names
NAME_RE = re.compile(r"^[a-z][a-z0-9_]{0,23}$")
TABLE_HEADER = "| Slot | Engine | Plug id | Status |"


class ScaffoldError(RuntimeError):
    pass


def slot_class(slot: int) -> int:
    return 50 if slot <= 7 else 10 if slot <= 57 else 5


def _engine_manifests(root: str) -> list[dict]:
    engines = os.path.join(root, "engines")
    out = []
    for name in sorted(os.listdir(engines)) if os.path.isdir(engines) else []:
        path = os.path.join(engines, name, "plug.json")
        if os.path.isfile(path):
            with open(path) as f:
                out.append(json.load(f))
    return out


def _table_rows(root: str) -> list[tuple[int, int | None]]:
    """(slot, plug id) of every row of memory-slots.md's allocation table."""
    path = os.path.join(root, "sdk", "docs", "memory-slots.md")
    if not os.path.isfile(path):
        return []
    rows, inside = [], False
    with open(path) as f:
        for line in f:
            if line.startswith(TABLE_HEADER):
                inside = True
                continue
            if inside:
                if not line.startswith("|"):
                    break
                cells = [c.strip() for c in line.strip().strip("|").split("|")]
                if cells and cells[0].isdigit():
                    pid = re.match(r"\d+", cells[2]) if len(cells) > 2 else None
                    rows.append((int(cells[0]), int(pid.group()) if pid else None))
    return rows


def used_slots(root: str = ROOT) -> set[int]:
    slots = {int(m["memoryslot"]) for m in _engine_manifests(root) if "memoryslot" in m}
    return slots | {s for s, _ in _table_rows(root)}


def used_plug_ids(root: str = ROOT) -> set[int]:
    ids = {int(m["plug"]["id"]) for m in _engine_manifests(root) if "plug" in m and "id" in m["plug"]}
    return ids | {p for _, p in _table_rows(root) if p is not None}


def check_slot(slot: int, used: set[int]) -> None:
    if slot <= 0:
        raise ScaffoldError("slot %d: 0 is where a definition without a slot lands, and 256 and up wrap round; use "
                            "1..%d" % (slot, MAX_SLOT))
    if slot > MAX_SLOT:
        raise ScaffoldError("slot %d is above %d: slots 90-255 stay closed until hardware test H-16 passes "
                            "(sdk/docs/memory-slots.md)" % (slot, MAX_SLOT))
    if slot in TE_SLOTS:
        raise ScaffoldError("slot %d belongs to a TE engine" % slot)
    if slot in used:
        raise ScaffoldError("slot %d is already taken (an engine under engines/ or a row of memory-slots.md)" % slot)


def pick_slot(used: set[int], size: str = "auto") -> int:
    """The first free slot of our range, top down. size: 'auto' (5 KB, then 10 KB), '5', '10' or '50'."""
    classes = [5, 10] if size == "auto" else [int(size)]
    for cls in classes:
        for slot in OUR_SLOTS[cls]:
            if slot not in used and slot not in TE_SLOTS:
                return slot
    raise ScaffoldError("no free slot left in this project's %s KB range (sdk/docs/memory-slots.md); pass --size "
                        "or --slot" % " and ".join(str(c) for c in classes))


def check_plug_id(pid: int, used: set[int]) -> None:
    if pid not in TEST_IDS and pid < RELEASED_ID_MIN:
        raise ScaffoldError("plug id %d: test plugs use %d-%d, released ones %d and up"
                            % (pid, TEST_IDS[0], TEST_IDS[-1], RELEASED_ID_MIN))
    if pid in used:
        raise ScaffoldError("plug id %d is already taken" % pid)


def pick_plug_id(used: set[int]) -> int:
    for pid in TEST_IDS:
        if pid not in used:
            return pid
    raise ScaffoldError("every test plug id %d-%d is taken; pass --plug-id" % (TEST_IDS[0], TEST_IDS[-1]))


def check_name(name: str, root: str = ROOT) -> None:
    if not NAME_RE.match(name):
        raise ScaffoldError("name %r: lowercase letters, digits and _, starting with a letter, at most 24 characters"
                            % name)
    if name in TE_ENGINES or any(w in name for w in FORBIDDEN_WORDS):
        raise ScaffoldError("name %r is or names a TE or Mutable Instruments engine; pick another" % name)
    if os.path.exists(os.path.join(root, "engines", name)):
        raise ScaffoldError("engines/%s already exists" % name)


def plan(name: str, kind: str, root: str = ROOT, size: str = "auto", slot: int | None = None,
         plug_id: int | None = None) -> dict:
    """Check everything and choose the slot and plug id. Writes nothing."""
    if kind not in KINDS:
        raise ScaffoldError("kind must be one of %s" % ", ".join(KINDS))
    check_name(name, root)
    slots = used_slots(root)
    if slot is None:
        slot = pick_slot(slots, size)
    check_slot(slot, slots)
    ids = used_plug_ids(root)
    if plug_id is None:
        plug_id = pick_plug_id(ids)
    check_plug_id(plug_id, ids)
    return {"name": name, "kind": kind, "slot": slot, "slot_class": slot_class(slot), "plug_id": plug_id,
            "template": os.path.join(root, "sdk", "templates", kind), "dest": os.path.join(root, "engines", name)}


def _fill(text: str, p: dict) -> str:
    return (text.replace(TEMPLATE_NAME, p["name"]).replace("@SLOT@", str(p["slot"]))
            .replace("@SLOT_CLASS@", "%d KB" % p["slot_class"]).replace("@PLUG_ID@", str(p["plug_id"])))


def _fill_plug_json(text: str, p: dict) -> str:
    for old, new in (('"memoryslot": 0,', '"memoryslot": %d,' % p["slot"]), ('"id": 0,', '"id": %d,' % p["plug_id"])):
        if old not in text:
            raise ScaffoldError("template plug.json has no %s" % old)
        text = text.replace(old, new)
    return _fill(text, p)


def record_slot(p: dict, root: str = ROOT) -> None:
    """Add the allocation as the last row of memory-slots.md's table."""
    path = os.path.join(root, "sdk", "docs", "memory-slots.md")
    with open(path) as f:
        lines = f.readlines()
    try:
        i = next(n for n, line in enumerate(lines) if line.startswith(TABLE_HEADER)) + 1
    except StopIteration:
        raise ScaffoldError("%s has no allocation table (%s)" % (path, TABLE_HEADER)) from None
    while i < len(lines) and lines[i].startswith("|"):
        i += 1
    lines.insert(i, "| %d | `%s` (%s, from the SDK template) | %d | scaffolded |\n"
                 % (p["slot"], p["name"], p["kind"], p["plug_id"]))
    with open(path, "w") as f:
        f.writelines(lines)


def scaffold(p: dict, root: str = ROOT) -> list[str]:
    """Copy the template to engines/NAME/ and record the slot. Returns the files written, relative to root."""
    if os.path.exists(p["dest"]):
        raise ScaffoldError("%s already exists" % p["dest"])
    written = []
    shutil.copytree(p["template"], p["dest"], ignore=shutil.ignore_patterns("golden", "*.dylib", ".DS_Store"))
    for d, _, files in os.walk(p["dest"]):
        for fn in files:
            path = os.path.join(d, fn)
            with open(path) as f:
                text = f.read()
            text = _fill_plug_json(text, p) if fn == "plug.json" else _fill(text, p)
            if fn == TEMPLATE_SOURCE and d == p["dest"]:
                os.remove(path)
                path = os.path.join(d, p["name"] + ".c")
            with open(path, "w") as f:
                f.write(text)
            written.append(os.path.relpath(path, root))
    record_slot(p, root)
    written.append(os.path.join("sdk", "docs", "memory-slots.md"))
    return sorted(written)


def next_steps(p: dict) -> list[str]:
    n = p["name"]
    return [
        "edit engines/%s/%s.c (the sound), plug.json (knob names), presets.json, tests/*.scn, README.md" % (n, n),
        "make -C sim golden ENGINE=%s   (then listen to sim/build/renders/%s/)" % (n, n),
        "make -C sim test",
        "python3 tools/opz_build.py engines/%s" % n,
        "python3 tools/opz_crashcheck.py engines/%s --full" % n,
        "python3 tools/opz_cycles.py engines/%s" % n,
        "python3 tools/opz.py --fake install engines/%s --track %s --yes" % (
            n, {"synth": "bass", "drum": "kick", "fx": "fx1"}[p["kind"]]),
        "sdk/docs/QUICKSTART.md explains each step; read SAFETY.md before any real OP-Z",
    ]


def add_arguments(ap: argparse.ArgumentParser) -> None:
    ap.add_argument("name", help="engine name: lowercase, digits, _ (becomes engines/NAME/ and NAME.engine)")
    ap.add_argument("--kind", choices=KINDS, required=True,
                    help="synth (bass/lead/arp/chord), drum (kick/snare/perc/sample) or fx (FX1/FX2)")
    ap.add_argument("--size", choices=("auto", "5", "10", "50"), default="auto",
                    help="slot class in KB (default auto: 5, then 10)")
    ap.add_argument("--slot", type=int, help="a specific memory slot instead of the next free one")
    ap.add_argument("--plug-id", type=int, help="a specific plug id instead of the next free test id")
    ap.add_argument("--dry-run", action="store_true", help="only show what would be created")


def run(a: argparse.Namespace, root: str = ROOT, out=None) -> int:
    out = out or sys.stdout
    try:
        p = plan(a.name, a.kind, root, a.size, a.slot, a.plug_id)
    except ScaffoldError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    print("%s engine %s: plug id %d, memory slot %d (%d KB), from sdk/templates/%s/"
          % (p["kind"], p["name"], p["plug_id"], p["slot"], p["slot_class"], p["kind"]), file=out)
    if a.dry_run:
        print("dry run: nothing written", file=out)
        return 0
    for path in scaffold(p, root):
        print("  wrote %s" % path, file=out)
    if not p["name"].startswith("opz_test_"):
        print("note: %d is a test plug id; give the engine a released id (61000+, sdk/docs/memory-slots.md) before "
              "you publish it" % p["plug_id"], file=out)
    print("next:", file=out)
    for step in next_steps(p):
        print("  " + step, file=out)
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    add_arguments(ap)
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
