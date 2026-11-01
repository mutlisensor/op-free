"""Tests for opz_verify.py and opz_pack.py.

Run from the repository root:

    python3 -m unittest discover -s tools/tests -v

The TE regression tests read the official engines from the local, git-ignored
firmware folder and are skipped when it is absent. Nothing from it is copied
into the repository; unpacked blocks go to a temporary directory.
"""

import io
import json
import os
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import opz_pack  # noqa: E402
import opz_verify as v  # noqa: E402

TE_ENGINES = os.path.join(REPO, "decrypted", "firmware", "blackfin", "system", "content",
                          "engines")

# Slots from sdk/docs/memory-slots.md for the engines in the 1.2.45 package.
TE_SLOTS = {
    "analoga": 27, "chorus": 60, "chorus2": 88, "cluster": 83, "crush": 79, "delay": 61,
    "dist": 78, "ep": 4, "fdnreverb": 89, "organ": 18, "reverb": 66,
    "sampleplayergain": 10, "synthsampler": 68,
}
EFFECTS = {"chorus", "chorus2", "crush", "delay", "dist", "fdnreverb", "reverb"}


def te_engine_files():
    if not os.path.isdir(TE_ENGINES):
        return []
    return sorted(os.path.join(TE_ENGINES, f) for f in os.listdir(TE_ENGINES)
                  if f.endswith(".engine"))


# --------------------------------------------------------------------------- synthetic engine

def make_image(base, kind="effect", names=None, magic=v.EXPORT_MAGIC):
    """A minimal engine image: export table, a few RTS-sized 'functions', name strings.
    Returns the payload to load at base."""
    if names is None:
        names = ["initModule", "init", "onNoteOn", "onNoteOff",
                 "renderFX" if kind == "effect" else "render"]
    table_size = 8 + 8 * (len(names) + 1)
    code_off = table_size
    code = b"\x10\x00" * 4 * len(names)  # 2-byte placeholder ops, 8 bytes per function
    str_off = code_off + len(code)
    strings = bytearray()
    entries = []
    for i, n in enumerate(names):
        entries.append((base + str_off + len(strings), base + code_off + 8 * i))
        strings += n.encode() + b"\0"
    table = struct.pack("<II", *magic)
    for ptr, target in entries:
        table += struct.pack("<II", ptr, target)
    table += struct.pack("<II", 0, 0)
    return table, code, bytes(strings)


def make_engine(slot=87, kind="effect", bss=64, **kw):
    base, _, _ = v.slot_window(slot)
    table, code, strings = make_image(base, kind, **kw)
    blocks = [
        {"kind": "data", "target": base, "payload": table},
        {"kind": "data", "target": base + len(table), "payload": code + strings},
    ]
    if bss:
        blocks.append({"kind": "fill", "target": base + len(table) + len(code) + len(strings),
                       "count": bss})
    return opz_pack.build_stream(blocks)


def patch_header(data, offset, **fields):
    """Rewrite fields of the header at offset; keep the checksum valid unless told not to."""
    flags, chk, marker, target, count, arg = v.HEADER.unpack_from(data, offset)
    flags = fields.get("flags", flags)
    marker = fields.get("marker", marker)
    target = fields.get("target", target)
    count = fields.get("count", count)
    arg = fields.get("argument", arg)
    chk = fields.get("hdrchk", v.header_checksum(flags, target, count, arg) ^ marker ^ v.MARKER)
    out = bytearray(data)
    v.HEADER.pack_into(out, offset, flags, chk, marker, target, count, arg)
    return bytes(out)


class VerifySynthetic(unittest.TestCase):
    def test_good_effect(self):
        rep = v.verify(make_engine())
        self.assertEqual(rep.errors, [])
        self.assertEqual(rep.warnings, [])
        self.assertEqual(rep.slot, 87)
        self.assertTrue(rep.slot_inferred)
        self.assertEqual(rep.kind, "effect")
        self.assertEqual(rep.fill_bytes, 64)

    def test_good_instrument_each_class(self):
        for slot in (7, 57, 58, 89):
            rep = v.verify(make_engine(slot=slot, kind="instrument"))
            self.assertTrue(rep.ok(strict=True), (slot, rep.errors, rep.warnings))
            self.assertEqual(rep.slot, slot)
            self.assertEqual(rep.kind, "instrument")

    def test_slot_windows(self):
        self.assertEqual(v.slot_window(0)[:2], (0x81B00000, 0xC800))
        self.assertEqual(v.slot_window(8)[:2], (0x81B64000, 0x2800))
        self.assertEqual(v.slot_window(58)[:2], (0x81BE1000, 0x1400))
        self.assertEqual(v.slot_window(89)[0] + 0x1400, 0x81C09000)
        self.assertEqual(v.slot_window(7)[0] + 0xC800, v.slot_window(8)[0])
        self.assertEqual(v.slot_window(57)[0] + 0x2800, v.slot_window(58)[0])

    def test_explicit_wrong_slot(self):
        rep = v.verify(make_engine(slot=87), slot=86)
        self.assertFalse(rep.ok())
        self.assertTrue(any("Bad address" in e for e in rep.errors))

    def test_bad_marker(self):
        data = patch_header(make_engine(), 16, marker=0xAC)
        rep = v.verify(data)
        self.assertTrue(any("Bad header'" in e for e in rep.errors), rep.errors)

    def test_bad_checksum(self):
        data = bytearray(make_engine())
        data[16 + 2] ^= 0x01
        rep = v.verify(bytes(data))
        self.assertTrue(any("Bad header checksum" in e for e in rep.errors), rep.errors)

    def test_block_outside_window(self):
        base, size, _ = v.slot_window(87)
        data = patch_header(make_engine(), 16 + 16 + 0x38, target=base + size - 8)
        rep = v.verify(data, slot=87)
        self.assertTrue(any("Bad address" in e for e in rep.errors), rep.errors)

    def test_nonzero_fill_is_warning(self):
        data = make_engine()
        off = len(data) - 32  # FILL header sits right before FINAL
        data = patch_header(data, off, argument=1)
        rep = v.verify(data)
        self.assertTrue(rep.ok())
        self.assertFalse(rep.ok(strict=True))
        self.assertTrue(any("Unsupported fill value" in w for w in rep.warnings))

    def test_bad_magic(self):
        rep = v.verify(make_engine(magic=(0xFEED0AD1, 0)))
        self.assertTrue(any("magic" in e for e in rep.errors), rep.errors)

    def test_missing_export(self):
        rep = v.verify(make_engine(names=["initModule", "init", "onNoteOn", "renderFX"]))
        self.assertTrue(any("onNoteOff" in e for e in rep.errors), rep.errors)

    def test_render_xor_renderfx(self):
        both = ["initModule", "init", "onNoteOn", "onNoteOff", "render", "renderFX"]
        self.assertFalse(v.verify(make_engine(names=both)).ok())
        neither = ["initModule", "init", "onNoteOn", "onNoteOff"]
        self.assertFalse(v.verify(make_engine(names=neither)).ok())

    def test_truncated(self):
        data = make_engine()
        self.assertFalse(v.verify(data[:40]).ok())
        self.assertFalse(v.verify(data[:-16]).ok())  # no FINAL

    def test_trailing_bytes_warn(self):
        rep = v.verify(make_engine() + b"\0" * 4)
        self.assertTrue(rep.ok())
        self.assertFalse(rep.ok(strict=True))

    def test_first_argument_mismatch_warns(self):
        data = patch_header(make_engine(), 0, argument=0x1234)
        rep = v.verify(data)
        self.assertTrue(rep.ok())
        self.assertTrue(any("FIRST argument" in w for w in rep.warnings))

    def test_cli_exit_codes(self):
        with tempfile.TemporaryDirectory() as tmp:
            good = os.path.join(tmp, "good.engine")
            bad = os.path.join(tmp, "bad.engine")
            with open(good, "wb") as f:
                f.write(make_engine())
            with open(bad, "wb") as f:
                f.write(make_engine(magic=(0, 0)))
            with redirect_stdout(io.StringIO()) as out, redirect_stderr(io.StringIO()):
                self.assertEqual(v.main([good]), 0)
                self.assertEqual(v.main([good, "--slot", "87", "--json"]), 0)
                self.assertEqual(v.main([bad]), 1)
                self.assertEqual(v.main([os.path.join(tmp, "missing.engine")]), 2)
            self.assertIn("PASS", out.getvalue())


class PackSynthetic(unittest.TestCase):
    def test_layout(self):
        data = make_engine()
        blocks, problems, end = v.parse_stream(data)
        self.assertEqual(problems, [])
        self.assertEqual(end, len(data))
        self.assertEqual(blocks[0].flags, 0x5000)
        self.assertEqual(blocks[0].target, 0x11A00000)
        self.assertEqual(blocks[0].argument, len(data) - 16)
        self.assertEqual(blocks[-1].flags, 0x8000)
        self.assertEqual(blocks[-2].flags, 0x0100)
        for b in blocks:
            self.assertEqual(b.xor, 0)
            self.assertEqual(b.marker, 0xAD)

    def test_roundtrip_and_cli(self):
        data = make_engine()
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "x.engine")
            out = os.path.join(tmp, "y.engine")
            d = os.path.join(tmp, "x")
            with open(src, "wb") as f:
                f.write(data)
            with redirect_stdout(io.StringIO()):
                self.assertEqual(opz_pack.main(["unpack", src, "-o", d]), 0)
                self.assertEqual(opz_pack.main(["pack", d, "-o", out, "--slot", "87"]), 0)
            with open(out, "rb") as f:
                self.assertEqual(f.read(), data)

    def test_pack_refuses_unverified(self):
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, "t.bin"), "wb") as f:
                f.write(b"\0" * 16)
            with open(os.path.join(tmp, "blocks.json"), "w") as f:
                json.dump({"format": opz_pack.FORMAT, "blocks": [
                    {"kind": "data", "target": "0x81c07c00", "file": "t.bin"}]}, f)
            out = os.path.join(tmp, "o.engine")
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                self.assertEqual(opz_pack.main(["pack", tmp, "-o", out]), 1)
                self.assertFalse(os.path.exists(out))
                self.assertEqual(opz_pack.main(["pack", tmp, "-o", out, "--no-verify"]), 0)
            self.assertTrue(os.path.exists(out))

    def test_elf(self):
        base, _, _ = v.slot_window(86)
        table, code, strings = make_image(base, "effect")
        elf = build_elf([
            (".exports", 1, base, table),
            (".text", 1, base + len(table), code),
            (".rodata", 1, base + len(table) + len(code), strings),
            (".bss", 8, base + len(table) + len(code) + len(strings), 32),
        ])
        blocks = opz_pack.blocks_from_elf(elf)
        self.assertEqual([b["kind"] for b in blocks], ["data", "data", "data", "fill"])
        stream = opz_pack.build_stream(blocks)
        rep = v.verify(stream, slot=86)
        self.assertTrue(rep.ok(strict=True), (rep.errors, rep.warnings))
        self.assertEqual(rep.fill_bytes, 32)


def build_elf(sections):
    """Minimal ELF32 LE relocatable-style image with section headers only.
    sections: (name, sh_type, addr, bytes-or-size)."""
    shstr = bytearray(b"\0")
    name_off = []
    for name, _, _, _ in sections:
        name_off.append(len(shstr))
        shstr += name.encode() + b"\0"
    shstr_name = len(shstr)
    shstr += b".shstrtab\0"
    body = bytearray()
    offs = []
    for _, stype, _, content in sections:
        offs.append(52 + len(body))
        if stype != 8:
            body += content
    shstr_off = 52 + len(body)
    body += shstr
    while (52 + len(body)) % 4:
        body += b"\0"
    shoff = 52 + len(body)
    sh = bytearray(40)  # null section
    for (name, stype, addr, content), no, off in zip(sections, name_off, offs):
        size = content if stype == 8 else len(content)
        flags = 0x2 | (0x4 if name == ".text" else 0) | (0x1 if stype == 8 else 0)
        sh += struct.pack("<IIIIIIIIII", no, stype, flags, addr, off, size, 0, 0, 4, 0)
    sh += struct.pack("<IIIIIIIIII", shstr_name, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    ident = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    hdr = ident + struct.pack("<HHIIIIIHHHHHH", 2, opz_pack.EM_BLACKFIN, 1, 0, 0, shoff, 0,
                              52, 0, 0, 40, len(sections) + 2, len(sections) + 1)
    return bytes(hdr + body + sh)


@unittest.skipUnless(te_engine_files(), "TE engines not present at %s" % TE_ENGINES)
class TEEngines(unittest.TestCase):
    """Regression over the official engines in the local firmware folder."""

    def test_count(self):
        self.assertGreaterEqual(len(te_engine_files()), 13)

    def test_verify_all(self):
        for path in te_engine_files():
            name = os.path.basename(path)[:-len(".engine")]
            with self.subTest(engine=name):
                rep = v.verify_file(path)
                self.assertEqual(rep.errors, [])
                self.assertTrue(rep.slot_inferred)
                if name in TE_SLOTS:
                    self.assertEqual(rep.slot, TE_SLOTS[name])
                    # The inferred slot must also pass when given explicitly.
                    self.assertTrue(v.verify_file(path, TE_SLOTS[name]).ok())
                self.assertEqual(rep.kind, "effect" if name in EFFECTS else "instrument")
                self.assertEqual(len(rep.exports), 5)
                # Only ep ships non-zero FILL values; everything else is clean.
                if name != "ep":
                    self.assertEqual(rep.warnings, [])
                else:
                    self.assertTrue(all("fill value" in w.lower() for w in rep.warnings))

    def test_roundtrip_all(self):
        with tempfile.TemporaryDirectory() as tmp:
            for path in te_engine_files():
                name = os.path.basename(path)
                with self.subTest(engine=name):
                    with open(path, "rb") as f:
                        data = f.read()
                    d = os.path.join(tmp, name + ".d")
                    opz_pack.unpack(data, d, name)
                    blocks, first, final = opz_pack.load_manifest(os.path.join(d, "blocks.json"))
                    self.assertEqual(opz_pack.build_stream(blocks, first, final), data)
                    # TE's header layout is the packer's default: dropping first/final from
                    # the manifest must give the same bytes.
                    self.assertEqual(opz_pack.build_stream(blocks), data)


if __name__ == "__main__":
    unittest.main()
