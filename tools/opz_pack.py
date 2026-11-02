#!/usr/bin/env python3
"""Pack and unpack OP-Z .engine files (ADI LDR boot streams).

Usage:
    opz_pack.py unpack FILE.engine -o DIR        engine -> DIR/blocks.json + one .bin per data block
    opz_pack.py pack DIR/blocks.json -o OUT.engine [--slot N] [--no-verify]
    opz_pack.py elf FILE.elf -o OUT.engine [--slot N] [--no-verify]

`pack` writes TE's layout (sdk/docs/engine-abi.md §1):

    FIRST|IGNORE  0x11a00000  count 0  argument = offset of the FINAL header
    data / FILL blocks from the block list, in order
    FINAL         0x11a00000  count 0

`unpack` followed by `pack` reproduces a TE engine byte for byte. `elf` builds the
block list from the allocated sections of a 32-bit little-endian ELF (PROGBITS ->
data block, NOBITS -> FILL value 0), in address order. Every output is checked
with opz_verify unless --no-verify is given.

blocks.json:

    {
      "format": "opz-ldr-blocks/1",
      "first": {"flags": "0x5000", "target": "0x11a00000"},      optional, TE default
      "final": {"flags": "0x8000", "target": "0x11a00000", "argument": "0x0"},  optional
      "blocks": [
        {"kind": "data", "target": "0x81bfb400", "file": "01_81bfb400.bin"},
        {"kind": "fill", "target": "0x81bfb85c", "count": "0x40"}
      ]
    }

Numbers may be JSON integers or "0x" strings. A data block may override "flags"
(default 0x0000) and "argument" (default 0); a fill block may override "flags"
(default 0x0100) and "value" (default 0). Python 3 standard library only.
"""

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import opz_verify as v  # noqa: E402

FORMAT = "opz-ldr-blocks/1"
MANIFEST = "blocks.json"
DATA_FLAGS = 0x0000
FILL_FLAGS = v.BFLAG_FILL


class PackError(Exception):
    pass


def num(x, what):
    if isinstance(x, bool):
        raise PackError("%s: expected a number, got %r" % (what, x))
    if isinstance(x, int):
        val = x
    elif isinstance(x, str):
        try:
            val = int(x, 0)
        except ValueError:
            raise PackError("%s: not a number: %r" % (what, x))
    else:
        raise PackError("%s: expected a number, got %r" % (what, x))
    if not 0 <= val <= 0xFFFFFFFF:
        raise PackError("%s: 0x%x does not fit in 32 bits" % (what, val))
    return val


def hx(n, width=0):
    return "0x%0*x" % (width, n)


# --------------------------------------------------------------------------- pack

def build_stream(blocks, first=None, final=None):
    """blocks: list of dicts {kind: data|fill, target, payload|count, flags?, argument?/value?}
    with numbers already resolved. Returns the .engine bytes."""
    first = first or {}
    final = final or {}
    body = bytearray()
    for i, b in enumerate(blocks):
        if b["kind"] == "data":
            flags = b.get("flags", DATA_FLAGS)
            if flags & v.BFLAG_FILL:
                raise PackError("block %d: data block has the FILL flag" % i)
            payload = b["payload"]
            body += v.pack_header(flags, b["target"], len(payload), b.get("argument", 0))
            body += payload
        elif b["kind"] == "fill":
            flags = b.get("flags", FILL_FLAGS)
            if not flags & v.BFLAG_FILL:
                raise PackError("block %d: fill block lacks the FILL flag" % i)
            body += v.pack_header(flags, b["target"], b["count"], b.get("value", 0))
        else:
            raise PackError("block %d: unknown kind %r" % (i, b["kind"]))
    final_offset = v.HEADER_SIZE + len(body)
    head = v.pack_header(first.get("flags", v.TE_FIRST_FLAGS),
                         first.get("target", v.STREAM_TARGET), 0, final_offset)
    tail = v.pack_header(final.get("flags", v.TE_FINAL_FLAGS),
                         final.get("target", v.STREAM_TARGET), 0, final.get("argument", 0))
    return bytes(head + body + tail)


def load_manifest(path):
    with open(path, "r", encoding="utf-8") as f:
        m = json.load(f)
    if m.get("format") != FORMAT:
        raise PackError("%s: format is %r, expected %r" % (path, m.get("format"), FORMAT))
    root = os.path.dirname(os.path.abspath(path))
    first = {k: num(val, "first." + k) for k, val in m.get("first", {}).items()
             if k in ("flags", "target")}
    final = {k: num(val, "final." + k) for k, val in m.get("final", {}).items()
             if k in ("flags", "target", "argument")}
    blocks = []
    for i, b in enumerate(m.get("blocks", [])):
        kind = b.get("kind")
        out = {"kind": kind, "target": num(b.get("target"), "blocks[%d].target" % i)}
        if "flags" in b:
            out["flags"] = num(b["flags"], "blocks[%d].flags" % i)
        if kind == "data":
            if "file" not in b:
                raise PackError("blocks[%d]: data block needs \"file\"" % i)
            fpath = os.path.join(root, b["file"])
            with open(fpath, "rb") as f:
                out["payload"] = f.read()
            if "count" in b and num(b["count"], "blocks[%d].count" % i) != len(out["payload"]):
                raise PackError("blocks[%d]: count %s != size of %s (%d)"
                                % (i, b["count"], b["file"], len(out["payload"])))
            if "argument" in b:
                out["argument"] = num(b["argument"], "blocks[%d].argument" % i)
        elif kind == "fill":
            out["count"] = num(b.get("count"), "blocks[%d].count" % i)
            if "value" in b:
                out["value"] = num(b["value"], "blocks[%d].value" % i)
        else:
            raise PackError("blocks[%d]: kind must be \"data\" or \"fill\", got %r" % (i, kind))
        blocks.append(out)
    if not blocks:
        raise PackError("%s: no blocks" % path)
    return blocks, first, final


# --------------------------------------------------------------------------- unpack

def unpack(data, outdir, source=None):
    """Split an engine into outdir/blocks.json and one .bin per data block.
    Returns the manifest dict."""
    blocks, problems, _ = v.parse_stream(data)
    errors = [msg for sev, msg in problems if sev == "error"]
    if errors:
        raise PackError("; ".join(errors))
    if len(blocks) < 2:
        raise PackError("stream too short")
    head, tail = blocks[0], blocks[-1]
    if not head.flags & v.BFLAG_FIRST or head.count:
        raise PackError("block 0 is %s with %d payload bytes; expected an empty FIRST block"
                        % (head.kind, head.count))
    if head.argument != tail.offset:
        raise PackError("FIRST argument 0x%x != FINAL offset 0x%x; pack could not reproduce it"
                        % (head.argument, tail.offset))
    if tail.count:
        raise PackError("FINAL block carries %d bytes; not supported" % tail.count)
    if len(data) != tail.offset + v.HEADER_SIZE:
        raise PackError("data after the FINAL block; pack could not reproduce it")
    for b in blocks:
        if b.marker != v.MARKER or b.xor != 0:
            raise PackError("block %d: bad marker or header checksum" % b.index)

    os.makedirs(outdir, exist_ok=True)
    m = {"format": FORMAT}
    if source:
        m["source"] = source
    m["first"] = {"flags": hx(head.flags, 4), "target": hx(head.target, 8)}
    m["final"] = {"flags": hx(tail.flags, 4), "target": hx(tail.target, 8),
                  "argument": hx(tail.argument)}
    m["blocks"] = []
    for b in blocks[1:-1]:
        if b.flags & (v.BFLAG_FIRST | v.BFLAG_FINAL):
            raise PackError("block %d: %s in the middle of the stream" % (b.index, b.kind))
        if b.is_fill:
            e = {"kind": "fill", "target": hx(b.target, 8), "count": hx(b.count)}
            if b.flags != FILL_FLAGS:
                e["flags"] = hx(b.flags, 4)
            if b.argument:
                e["value"] = hx(b.argument)
        else:
            name = "%02d_%08x.bin" % (b.index, b.target)
            with open(os.path.join(outdir, name), "wb") as f:
                f.write(b.payload)
            e = {"kind": "data", "target": hx(b.target, 8), "count": hx(b.count), "file": name}
            if b.flags != DATA_FLAGS:
                e["flags"] = hx(b.flags, 4)
            if b.argument:
                e["argument"] = hx(b.argument)
        m["blocks"].append(e)
    with open(os.path.join(outdir, MANIFEST), "w", encoding="utf-8") as f:
        json.dump(m, f, indent=2)
        f.write("\n")
    return m


# --------------------------------------------------------------------------- ELF

SHF_ALLOC = 0x2
SHT_NOBITS = 8
EM_BLACKFIN = 106


def blocks_from_elf(data):
    """Allocated, non-empty sections in address order: PROGBITS -> data, NOBITS -> fill."""
    if data[:4] != b"\x7fELF":
        raise PackError("not an ELF file")
    if data[4] != 1 or data[5] != 1:
        raise PackError("only 32-bit little-endian ELF is supported")
    (e_type, e_machine, _, _, _, e_shoff, _, _, _, _, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    if e_machine != EM_BLACKFIN:
        print("opz_pack: warning: ELF machine %d is not Blackfin (%d)" % (e_machine, EM_BLACKFIN),
              file=sys.stderr)
    if e_shoff == 0 or e_shnum == 0:
        raise PackError("ELF has no section headers")
    secs = []
    for i in range(e_shnum):
        (name, stype, flags, addr, off, size, _, _, _, _) = struct.unpack_from(
            "<IIIIIIIIII", data, e_shoff + i * e_shentsize)
        secs.append((name, stype, flags, addr, off, size))
    strtab = secs[e_shstrndx] if e_shstrndx < len(secs) else None

    def secname(n):
        if not strtab:
            return "?"
        start = strtab[4] + n
        return data[start:data.index(b"\0", start)].decode("ascii", "replace")

    blocks = []
    for name, stype, flags, addr, off, size in sorted(secs, key=lambda s: s[3]):
        if not flags & SHF_ALLOC or size == 0:
            continue
        if stype == SHT_NOBITS:
            blocks.append({"kind": "fill", "target": addr, "count": size, "section": secname(name)})
        else:
            payload = data[off:off + size]
            if len(payload) != size:
                raise PackError("section %s truncated" % secname(name))
            blocks.append({"kind": "data", "target": addr, "payload": payload,
                           "section": secname(name)})
    if not blocks:
        raise PackError("ELF has no allocated sections")
    return blocks


# --------------------------------------------------------------------------- CLI

def write_checked(stream, out, slot, no_verify):
    if not no_verify:
        rep = v.verify(stream, out, slot)
        if not rep.ok():
            v.print_report(rep, out=sys.stderr)
            raise PackError("verification failed; nothing written (use --no-verify to force)")
    with open(out, "wb") as f:
        f.write(stream)
    print("wrote %s (%d bytes)" % (out, len(stream)))


def main(argv=None):
    ap = argparse.ArgumentParser(description="Pack and unpack OP-Z .engine LDR streams.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("unpack", help="engine -> blocks.json + raw block files")
    p.add_argument("engine")
    p.add_argument("-o", "--out", required=True, help="output directory")
    p = sub.add_parser("pack", help="blocks.json -> engine")
    p.add_argument("manifest", help="blocks.json, or the directory holding it")
    p.add_argument("-o", "--out", required=True)
    p.add_argument("--slot", type=int)
    p.add_argument("--no-verify", action="store_true")
    p = sub.add_parser("elf", help="ELF -> engine")
    p.add_argument("elf")
    p.add_argument("-o", "--out", required=True)
    p.add_argument("--slot", type=int)
    p.add_argument("--no-verify", action="store_true")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "unpack":
            with open(args.engine, "rb") as f:
                data = f.read()
            m = unpack(data, args.out, os.path.basename(args.engine))
            print("wrote %s (%d blocks)" % (os.path.join(args.out, MANIFEST), len(m["blocks"])))
        elif args.cmd == "pack":
            path = args.manifest
            if os.path.isdir(path):
                path = os.path.join(path, MANIFEST)
            blocks, first, final = load_manifest(path)
            write_checked(build_stream(blocks, first, final), args.out, args.slot,
                          args.no_verify)
        else:
            with open(args.elf, "rb") as f:
                data = f.read()
            blocks = blocks_from_elf(data)
            for b in blocks:
                n = len(b["payload"]) if b["kind"] == "data" else b["count"]
                print("  %-5s %-16s 0x%08x  %d bytes" % (b["kind"], b["section"], b["target"], n))
            write_checked(build_stream(blocks), args.out, args.slot, args.no_verify)
    except (PackError, OSError, ValueError) as e:
        print("opz_pack: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
