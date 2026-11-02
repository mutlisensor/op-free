"""config/userPresets.dat: the OP-Z's own store of user presets (the ones stored with track + white key, 2 s).

Layout, from the OS image (research/presets.md, "Presets that stay"): a u16 version (1), then exactly 200 records
of 0x6dc bytes, little-endian. A record is a plug's preset block plus the plug id (u32 at +0x6d8; 0 = unused). In the
block, slot s (0-3) index i (0-13) is 31 bytes at s*434 + i*31: name (12 bytes, NUL-terminated, 11 characters), the
18 values, defaultOctave (signed byte). Then the preset masks u16[4] at 1736 and the user masks u16[4] at 1744. At
every start the OS copies, for each record whose plug is installed, the presets whose **user-mask** bit is set; it
rewrites the whole file from RAM whenever the user stores a preset.

Pure functions only (no files, no device): parse, check, read, bake our presets in as user presets, serialise.
"""

from __future__ import annotations

import struct

VERSION = 1
RECORDS = 200
RECORD = 0x6DC                  # 1756
ID_AT = 0x6D8                   # 1752
SLOT_STRIDE = 434               # 14 * 31
PRESET = 31
NAME = 12
MASK_AT = 1736
USER_AT = 1744
SLOTS = 4
INDEXES = 14
SIZE = 2 + RECORDS * RECORD     # 351,202
ALL_BITS = (1 << INDEXES) - 1
USER_PLUG_MIN = 60000


class FormatError(ValueError):
    pass


def parse(data: bytes) -> list[bytearray]:
    """The 200 records, after the checks the OS makes (version, size) and stricter ones of ours: no plug id twice,
    no mask bit past index 13. Raises FormatError."""
    if len(data) != SIZE:
        raise FormatError("userPresets.dat must be %d bytes, this one is %d" % (SIZE, len(data)))
    version = struct.unpack_from("<H", data, 0)[0]
    if version != VERSION:
        raise FormatError("userPresets.dat version is %d, expected %d" % (version, VERSION))
    records = [bytearray(data[2 + n * RECORD:2 + (n + 1) * RECORD]) for n in range(RECORDS)]
    seen: dict[int, int] = {}
    for n, r in enumerate(records):
        pid = plug_id(r)
        if pid == 0:
            continue
        if pid in seen:
            raise FormatError("plug %d has two records (%d and %d)" % (pid, seen[pid], n))
        seen[pid] = n
        for s in range(SLOTS):
            for at in (MASK_AT, USER_AT):
                if mask(r, at, s) & ~ALL_BITS:
                    raise FormatError("plug %d slot %d: mask bits past index 13 (0x%04x)" % (pid, s, mask(r, at, s)))
    return records


def serialise(records: list[bytearray]) -> bytes:
    if len(records) != RECORDS or any(len(r) != RECORD for r in records):
        raise FormatError("need 200 records of %d bytes" % RECORD)
    return struct.pack("<H", VERSION) + b"".join(bytes(r) for r in records)


def plug_id(record: bytes) -> int:
    return struct.unpack_from("<I", record, ID_AT)[0]


def mask(record: bytes, at: int, slot: int) -> int:
    return struct.unpack_from("<H", record, at + 2 * slot)[0]


def _set_mask(record: bytearray, at: int, slot: int, value: int) -> None:
    struct.pack_into("<H", record, at + 2 * slot, value)


def preset_bytes(name: str, values, octave: int) -> bytes:
    """31 bytes as the device keeps a preset: 11 characters + NUL (zero-padded), 18 values, octave (clamped -4..4,
    as readPresetSettings clamps it)."""
    if not name.isascii():
        raise FormatError("preset name %r: only ASCII fits the device" % name)
    values = list(values)
    if len(values) != 18 or not all(isinstance(v, int) and 0 <= v <= 255 for v in values):
        raise FormatError("preset %r needs 18 values in 0..255" % name)
    raw = name.encode("ascii")[:NAME - 1]
    return raw + bytes(NAME - len(raw)) + bytes(values) + struct.pack("<b", max(-4, min(4, int(octave))))


def read_preset(record: bytes, slot: int, index: int) -> dict:
    at = slot * SLOT_STRIDE + index * PRESET
    raw = record[at:at + PRESET]
    name = raw[:NAME].split(b"\0", 1)[0].decode("latin-1")
    return {"slot": slot, "index": index, "name": name, "values": list(raw[NAME:NAME + 18]),
            "defaultOctave": struct.unpack_from("<b", raw, 30)[0],
            "user": bool(mask(record, USER_AT, slot) >> index & 1), "raw": bytes(raw)}


def user_presets(record: bytes) -> list[dict]:
    """The presets the OS would load from this record (user-mask bit set)."""
    return [read_preset(record, s, i) for s in range(SLOTS) for i in range(INDEXES) if mask(record, USER_AT, s) >> i & 1]


def summary(records: list[bytearray]) -> list[str]:
    lines = []
    used = [r for r in records if plug_id(r)]
    lines.append("%d plug records, %d free; %d with user presets" % (
        len(used), RECORDS - len(used), sum(1 for r in used if user_presets(r))))
    for r in used:
        ups = user_presets(r)
        if ups:
            lines.append("  plug %d: %s" % (plug_id(r), ", ".join(
                "slot %d index %d %r" % (p["slot"], p["index"], p["name"]) for p in ups)))
    return lines


def bake(records: list[bytearray], plugs: list[tuple[int, str, list]], overwrite_user: bool = False):
    """Write our presets into `records` (in place) as user presets. plugs = [(plug id, label, presets)], presets in
    presets.json form (slot, index, name, values, defaultOctave or default_octave). A plug without a record gets a free
    one (zeroed first). A user preset at the same key that carries our preset's name is ours from an earlier bake and
    is updated; any other one (a preset stored on the device gets no name) is kept unless overwrite_user.
    Returns {label: {"new": n, "same": n, "updated": n, "replaced": n, "kept": [(slot, index, their name)]}}."""
    report: dict[str, dict] = {}
    by_id = {plug_id(r): r for r in records if plug_id(r)}
    for pid, label, presets in plugs:
        if pid < USER_PLUG_MIN:
            raise FormatError("plug %d is in TE's range: not ours to bake" % pid)
        r = by_id.get(pid)
        if r is None:
            r = next((x for x in records if plug_id(x) == 0), None)
            if r is None:
                raise FormatError("no free record for plug %d (all 200 used)" % pid)
            r[:] = bytes(RECORD)
            struct.pack_into("<I", r, ID_AT, pid)
            by_id[pid] = r
        rep = report.setdefault(label, {"new": 0, "same": 0, "updated": 0, "replaced": 0, "kept": []})
        for p in presets:
            slot, index = int(p.get("slot", 0)), int(p.get("index", 0))
            if not (0 <= slot < SLOTS and 0 <= index < INDEXES):
                raise FormatError("%s preset %r: slot 0..3 and index 0..13" % (label, p.get("name")))
            octave = p.get("defaultOctave", p.get("default_octave", 0))
            want = preset_bytes(str(p.get("name", "")), p["values"], octave)
            have = read_preset(r, slot, index)
            if have["user"] and have["raw"] == want:
                rep["same"] += 1
                continue
            ours = have["user"] and have["name"] != "" and have["raw"][:NAME] == want[:NAME]
            if have["user"] and not ours and not overwrite_user:
                rep["kept"].append((slot, index, have["name"]))
                continue
            at = slot * SLOT_STRIDE + index * PRESET
            r[at:at + PRESET] = want
            _set_mask(r, MASK_AT, slot, mask(r, MASK_AT, slot) | 1 << index)
            _set_mask(r, USER_AT, slot, mask(r, USER_AT, slot) | 1 << index)
            rep["updated" if ours else "replaced" if have["user"] else "new"] += 1
    return report


def changed_records(before: list[bytearray], after: list[bytearray]) -> list[int]:
    return [n for n in range(RECORDS) if before[n] != after[n]]
