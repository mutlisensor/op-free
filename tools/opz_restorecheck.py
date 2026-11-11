#!/usr/bin/env python3
"""Before a content-mode restore: will the OP-Z take this backup of its disk? (docs/roadmap/restore-space.md)

Reads only file names and sizes (the backup folder, optionally the OP-Z disk as it is now) and the settings our tools
last read, and predicts what the importer of OS 1.2.45 does with each slot folder (`0x80038174`, `0x80038cd8`,
`0x80037df4` [code]; folders merged by Finder partly [inferred]). Nothing is written. Port in core
src/restorecheck.ts (same numbers, same lines).

    python3 tools/opz.py restore-check ~/OP-Z-backup [--disk /Volumes/OP-Z] [--source backups/<time>] [--log import.log]
"""

from __future__ import annotations

import os
import re

PLUG_LIMIT = 200
SAMPLE_LIMIT = 24 * 1024 * 1024
DISK_BYTES = 0x2100000          # the content-mode FAT in RAM (0x8002ede0) [code]
OUR_PLUG_MIN, OUR_PLUG_MAX = 60900, 61999

SLOT_RE = re.compile(r"(?:^|/)(samplepacks/(\d+)-[^/]+/(\d+))/([^/]+)$")
SAMPLE_RE = re.compile(r"\.(wav|aiff?)$", re.I)
ENGINE_RE = re.compile(r"\.engi(ne?)?$", re.I)     # the export cuts ours to `~61020.engi` [proven: import.log]


def mb(n: int) -> str:
    tenths = (n * 10 + 524288) // 1048576
    return "%d.%d MB" % (tenths // 10, tenths % 10)


def _bare(name: str) -> str:
    return name[1:] if name.startswith("~") else name


def _basename(path: str) -> str:
    return path.split("/")[-1]


def _placeholder_id(name: str) -> int:
    m = re.match(r"\d+", _bare(name).split(".")[0])
    return int(m.group(0)) if m else 0


def _user_sample(p: dict) -> bool:
    return bool(int(p.get("flags", 0)) & 2)


def _ours(p: dict) -> bool:
    return OUR_PLUG_MIN <= int(p["id"]) <= OUR_PLUG_MAX and not _user_sample(p)


def list_folder(root: str) -> list[dict]:
    """Every file under root as {path (relative, '/'), size}."""
    out = []
    for d, _, files in os.walk(root):
        for f in files:
            full = os.path.join(d, f)
            out.append({"path": os.path.relpath(full, root).replace(os.sep, "/"), "size": os.path.getsize(full)})
    return out


def slot_folders(entries: list[dict]) -> dict:
    out: dict = {}
    for e in entries:
        m = SLOT_RE.search(e["path"].replace("\\", "/"))
        if not m or m.group(4).startswith("."):
            continue
        if not SAMPLE_RE.search(m.group(4)) and not ENGINE_RE.search(m.group(4)):
            continue
        n, s = int(m.group(2)), int(m.group(3))
        if not (1 <= n <= 8 and 1 <= s <= 10):
            continue
        out.setdefault((n - 1, s), {"dir": m.group(1), "files": []})["files"].append({"name": m.group(4), "size": e["size"]})
    for f in out.values():
        f["files"].sort(key=lambda x: x["name"])
    return out


def check(backup: list[dict], device: dict | None, disk: list[dict] | None = None) -> dict:
    """device: {"plugs": plugs.json, "slots": slotConfiguration.json} our tools last read, or None."""
    plugs = (device or {}).get("plugs", {}).get("plugs", [])
    by_id = {p["id"]: p for p in plugs}
    named = {_basename(p.get("samples", "")) for p in plugs
             if re.match(r"^samplepacks/(user/)?[^/]+$", p.get("samples", ""))}
    cell = {(t["index"], q["slot"]): q["id"] for t in (device or {}).get("slots", {}).get("tracks", [])
            for q in t.get("packs", [])}
    folders = slot_folders(backup)
    problems, warnings, imports, assigns, dangling, our_slots = [], [], [], [], [], []
    restored, names = set(), set()
    unchanged = placeholders = 0
    for key in sorted(folders):
        t, s = key
        d, files = folders[key]["dir"], folders[key]["files"]
        names |= {_bare(f["name"]) for f in files if SAMPLE_RE.search(f["name"])}
        now = cell.get(key)
        plug = by_id.get(now) if now is not None else None
        if plug and ((plug.get("samples") and any(_bare(f["name"]) == _basename(plug["samples"]) for f in files))
                     or (not plug.get("samples") and now <= 200
                         and any(ENGINE_RE.search(f["name"]) and _placeholder_id(f["name"]) == now for f in files))):
            unchanged += 1
            continue
        restored.add(key)
        if plug and _ours(plug):
            our_slots.append((t, s))
        sample_seen = False
        for f in files:
            path = "%s/%s" % (d, f["name"])
            if ENGINE_RE.search(f["name"]):
                pid = _placeholder_id(f["name"])
                if not pid:
                    warnings.append("%s: not a plug id; the OP-Z rejects it" % path)
                elif device and pid not in by_id:
                    dangling.append({"path": path, "id": pid})
                continue
            if sample_seen:
                warnings.append("%s: a second sample in one slot folder; the OP-Z uses only the first that imports" % path)
                continue
            sample_seen = True
            if f["size"] == 0:
                if device and _bare(f["name"]) not in named:
                    warnings.append("%s: a placeholder for a pack this OP-Z doesn't have; that slot ends up empty" % path)
                else:
                    placeholders += 1
            elif device and _bare(f["name"]) in named:
                assigns.append({"path": path, "size": f["size"]})
            else:
                imports.append({"path": path, "size": f["size"]})

    user_cells: dict = {}
    for k, pid in cell.items():
        if pid in by_id and _user_sample(by_id[pid]):
            user_cells.setdefault(pid, []).append(k)
    kept = [pid for pid, ks in user_cells.items()
            if any(k not in restored for k in ks) or _basename(by_id[pid].get("samples", "")) in names]
    kept_bytes = disk_bytes = None
    if disk is not None:
        disk_bytes = sum(e["size"] for e in disk)
        kept_bytes = 0
        seen = set()
        for key, folder in slot_folders(disk).items():
            for f in folder["files"]:
                if f["size"] == 0 or not SAMPLE_RE.search(f["name"]) or f["name"] in seen:
                    continue
                if key not in restored or f["name"] in names:
                    seen.add(f["name"])
                    kept_bytes += f["size"]
    import_bytes = sum(e["size"] for e in imports)
    peak = (kept_bytes or 0) + import_bytes + max([0] + [e["size"] for e in assigns])
    if peak > SAMPLE_LIMIT:
        problems.append("sample memory: the OP-Z keeps 24 MB of your samples; this restore needs about %s" % mb(peak)
                        + ("" if kept_bytes is None else " (%s that stay on it, %s brought back)" % (mb(kept_bytes), mb(import_bytes)))
                        + ". It refuses the rest (\"sample memory full\", logged only at the next content mode) and those slots "
                        "stay as they were. Restore fewer slots at a time, or first delete samples you don't need on the disk "
                        "and eject")
    elif kept_bytes is None and kept and import_bytes > SAMPLE_LIMIT / 2:
        warnings.append("%d of your samples stay on the OP-Z and count towards its 24 MB too (their size is known only "
                        "with the disk: check it in content mode)" % len(kept))

    free = None
    ours = sum(1 for p in plugs if _ours(p))
    helps = bool(our_slots)
    if device:
        dropped = sum(1 for pid in user_cells if pid not in kept)
        free = PLUG_LIMIT - len(plugs) + dropped
        if len(imports) > free:
            problems.append("sound definitions: this restore brings back %d samples, and only %d of the OP-Z's %d "
                            "definitions will be free. The OP-Z logs \"SUCCESS\" for the others, then deletes them"
                            % (len(imports), free, PLUG_LIMIT) + (". Taking our %d plugs off first frees %d" % (ours, ours) if ours else ""))
            if ours and len(imports) <= free + ours:
                helps = True
    if dangling:
        ids = []
        for x in dangling:
            if x["id"] not in ids:
                ids.append(x["id"])
        problems.append("%d slot%s in the backup name a plug that isn't installed now (%s): the OP-Z would put that id in "
                        "the slot anyway and the slot would be silent. Delete those ~<number>.engine files from the copy "
                        "you restore, or install those engines again first"
                        % (len(dangling), "" if len(dangling) == 1 else "s", ", ".join(str(i) for i in ids)))
    if our_slots:
        warnings.append("%d slot%s the backup restores hold our sounds now. If the copy merges folders, our sound can stay "
                        "in a slot whose sample fails to import. Take our plugs off first, or replace whole slot folders"
                        % (len(our_slots), "" if len(our_slots) == 1 else "s"))
    backup_bytes = sum(e["size"] for e in backup)
    if backup_bytes > DISK_BYTES:
        problems.append("the backup is %s; the OP-Z disk holds %s in all. Copy it in parts: samplepacks, then projects"
                        % (mb(backup_bytes), mb(DISK_BYTES)))
    elif disk_bytes is not None and disk_bytes + backup_bytes > DISK_BYTES:
        warnings.append("the disk holds %s now and the backup is %s; together more than the disk's %s. If the computer "
                        "says there isn't enough space, delete the old files on the OP-Z disk first (empty the Trash, or "
                        "delete them for good), then copy" % (mb(disk_bytes), mb(backup_bytes), mb(DISK_BYTES)))
    projects = sum(1 for e in backup if re.search(r"(?:^|/)projects/[^/]+\.opz$", e["path"].replace("\\", "/"), re.I))
    return {"folders": len(folders), "unchanged": unchanged, "imports": imports, "assigns": assigns,
            "placeholders": placeholders, "dangling": dangling, "our_slots": our_slots, "kept_bytes": kept_bytes,
            "kept_samples": len(kept), "import_bytes": import_bytes, "peak_bytes": peak, "free_definitions": free,
            "ours": ours, "projects": projects, "backup_bytes": backup_bytes, "disk_bytes": disk_bytes,
            "problems": problems, "warnings": warnings, "remove_ours_helps": helps}


def describe(r: dict) -> list[str]:
    lines = [
        "backup: %s, %d slot folders (%d as on the OP-Z now), %d projects" % (mb(r["backup_bytes"]), r["folders"], r["unchanged"], r["projects"]),
        "samples: %d brought back (%s), %d already on the OP-Z, %d placeholders" % (
            len(r["imports"]), mb(r["import_bytes"]), len(r["assigns"]), r["placeholders"]),
        "sample memory: about %s of 24.0 MB at the worst point" % mb(r["peak_bytes"])
        + (" (+ %d of your samples that stay, size unknown without the disk)" % r["kept_samples"]
           if r["kept_bytes"] is None and r["kept_samples"] else ""),
    ]
    if r["free_definitions"] is not None:
        lines.append("sound definitions: %d free for %d new samples (%d of ours on the OP-Z)" % (
            r["free_definitions"], len(r["imports"]), r["ours"]))
    if r["disk_bytes"] is not None:
        lines.append("OP-Z disk now: %s of %s" % (mb(r["disk_bytes"]), mb(DISK_BYTES)))
    lines += ["problem: " + p for p in r["problems"]] + ["warning: " + w for w in r["warnings"]]
    if r["remove_ours_helps"]:
        lines.append("advice:  take our plugs off first (opz.py remove --all-ours, or Remove all of ours in the web app), then restore")
    if not r["problems"]:
        lines.append("ok with warnings: the restore should go through" if r["warnings"] else "ok: the restore should go through")
    return lines


def parse_import_log(text: str) -> dict:
    out = {"used": None, "memory_full": [], "failed": [], "imports": 0}
    for raw in text.splitlines():
        line = raw.strip()
        m = re.search(r"Calculating used sample space\.\.\.([\d.]+/[\d.]+ MB)", line)
        if m:
            out["used"] = m.group(1)
        m = re.search(r"Can't import (.+), sample memory full", line)
        if m:
            out["memory_full"].append(m.group(1))
        if re.match(r"^importing .+\.\.\.SUCCESS", line):
            out["imports"] += 1
        elif re.match(r"^importing .+\.\.\.", line) or line.startswith("rejecting "):
            out["failed"].append(line)
    return out
