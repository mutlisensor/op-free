#!/usr/bin/env python3
"""Verify an OP-Z .engine file the way the OS loader does, and print its maps.

Usage:
    opz_verify.py FILE.engine [--slot N] [--strict] [--json]

An engine is an ADI LDR boot stream (sdk/docs/engine-abi.md §1). This tool
re-implements the checks of the OS loader in dlm.cpp [code: FUN_8002ae50,
FUN_8002ac00, FUN_8002b02c, FUN_801419a8; strings in opz.ldr]:

  * byte 3 of every header is 0xAD                       ("Bad header")
  * the 16 header bytes XOR to 0                         ("Bad header checksum")
  * every loaded block lies inside the slot window       ("Bad address")
  * FILL blocks use value 0                              ("Unsupported fill value";
    the OS logs it and zero-fills anyway, so this is a warning)
  * export table magic 0xfeed0ad1 0xad1facef at the slot base
  * initModule, init, onNoteOn, onNoteOff and render or renderFX are exported
                                                         ("Could not lookup symbols")

It also checks that the stream matches the layout TE ships (FIRST|IGNORE header
whose argument is the offset of the FINAL header, FINAL at the end). Deviations
from that layout are warnings; --strict makes warnings fatal.

Exit status: 0 = pass, 1 = errors (or warnings with --strict), 2 = usage/IO error.
Python 3 standard library only.
"""

import argparse
import json
import struct
import sys

HEADER = struct.Struct("<HBBIII")  # flags, hdrchk, marker, target, count, argument
HEADER_SIZE = HEADER.size  # 16
MARKER = 0xAD

# ADI BF60x/BF70x LDR block flags (lower 16 bits of the block code). [notes: ADI loader manual]
BFLAG_FINAL = 0x8000
BFLAG_FIRST = 0x4000
BFLAG_INDIRECT = 0x2000
BFLAG_IGNORE = 0x1000
BFLAG_INIT = 0x0800
BFLAG_CALLBACK = 0x0400
BFLAG_QUICKBOOT = 0x0200
BFLAG_FILL = 0x0100
BFLAG_AUX = 0x0020
BFLAG_SAVE = 0x0010
FLAG_NAMES = [
    (BFLAG_FINAL, "FINAL"), (BFLAG_FIRST, "FIRST"), (BFLAG_INDIRECT, "INDIRECT"),
    (BFLAG_IGNORE, "IGNORE"), (BFLAG_INIT, "INIT"), (BFLAG_CALLBACK, "CALLBACK"),
    (BFLAG_QUICKBOOT, "QUICKBOOT"), (BFLAG_FILL, "FILL"), (BFLAG_AUX, "AUX"),
    (BFLAG_SAVE, "SAVE"),
]
# Flags seen in TE's engines; anything else is unknown to the device loader.
KNOWN_FLAGS = BFLAG_FINAL | BFLAG_FIRST | BFLAG_IGNORE | BFLAG_FILL

# TE's stream header and trailer target (outside every slot; never loaded). [code: all 13 engines]
STREAM_TARGET = 0x11A00000
TE_FIRST_FLAGS = BFLAG_FIRST | BFLAG_IGNORE  # 0x5000
TE_FINAL_FLAGS = BFLAG_FINAL  # 0x8000

EXPORT_MAGIC = (0xFEED0AD1, 0xAD1FACEF)
REQUIRED_EXPORTS = ("initModule", "init", "onNoteOn", "onNoteOff")
RENDER_EXPORTS = ("render", "renderFX")
MAX_EXPORTS = 64

# Memory slot classes (sdk/docs/memory-slots.md) [code: FUN_800fb556]
SLOT_CLASSES = (
    # first, last (inclusive), class base, window size, label
    (0, 7, 0x81B00000, 0xC800, "50 KB"),
    (8, 57, 0x81B64000, 0x2800, "10 KB"),
    (58, None, 0x81BE1000, 0x1400, "5 KB"),
)
MAX_CHECKED_SLOT = 89  # memory above slot 89 not yet checked (Q-16)


# --------------------------------------------------------------------------- slots

def slot_window(slot):
    """Return (base, size, class label) for a memory slot number."""
    if slot < 0:
        raise ValueError("slot must be >= 0")
    for first, last, base, size, label in SLOT_CLASSES:
        if last is None or slot <= last:
            return base + (slot - first) * size, size, label
    raise AssertionError("unreachable")


def infer_slot(addresses):
    """Slot (0..89) whose window contains every address range, preferring the
    slot whose base is the lowest target (the export table sits at the base)."""
    if not addresses:
        return None
    lo = min(a for a, _ in addresses)
    hi = max(a + n for a, n in addresses)
    candidates = []
    for slot in range(MAX_CHECKED_SLOT + 1):
        base, size, _ = slot_window(slot)
        if base <= lo and hi <= base + size:
            candidates.append((base != lo, slot))
    if not candidates:
        return None
    return min(candidates)[1]


# --------------------------------------------------------------------------- parsing

class Block:
    __slots__ = ("index", "offset", "flags", "hdrchk", "marker", "target", "count",
                 "argument", "payload", "xor")

    def __init__(self, index, offset, raw, payload):
        self.index = index
        self.offset = offset
        (self.flags, self.hdrchk, self.marker, self.target, self.count,
         self.argument) = HEADER.unpack(raw)
        x = 0
        for b in raw:
            x ^= b
        self.xor = x
        self.payload = payload

    @property
    def is_fill(self):
        return bool(self.flags & BFLAG_FILL)

    @property
    def is_ignore(self):
        return bool(self.flags & BFLAG_IGNORE)

    @property
    def is_final(self):
        return bool(self.flags & BFLAG_FINAL)

    @property
    def loads(self):
        """True if the device writes this block into memory."""
        return not self.is_ignore and self.count > 0

    @property
    def kind(self):
        names = [n for bit, n in FLAG_NAMES if self.flags & bit]
        if self.flags & 0x000F:
            names.append("DMA%d" % (self.flags & 0xF))
        if not names:
            return "data"
        return "|".join(names)


def header_checksum(flags, target, count, argument):
    """The hdrchk byte that makes all 16 header bytes XOR to 0."""
    raw = HEADER.pack(flags, 0, MARKER, target, count, argument)
    x = 0
    for b in raw:
        x ^= b
    return x


def pack_header(flags, target, count, argument):
    return HEADER.pack(flags, header_checksum(flags, target, count, argument), MARKER,
                       target, count, argument)


def parse_stream(data):
    """Walk the LDR stream. Returns (blocks, problems, end_offset) where problems
    is a list of (severity, message) for structural faults found while walking."""
    blocks = []
    problems = []
    off = 0
    final_seen = False
    while off < len(data):
        if len(data) - off < HEADER_SIZE:
            problems.append(("error", "truncated header at 0x%04x (%d bytes left)"
                             % (off, len(data) - off)))
            off = len(data)
            break
        raw = data[off:off + HEADER_SIZE]
        flags, _, _, _, count, _ = HEADER.unpack(raw)
        payload = b""
        plen = 0 if flags & BFLAG_FILL else count
        if plen:
            start = off + HEADER_SIZE
            payload = data[start:start + plen]
            if len(payload) < plen:
                problems.append(("error", "block %d at 0x%04x: payload truncated (%d of %d bytes)"
                                 % (len(blocks), off, len(payload), plen)))
        blocks.append(Block(len(blocks), off, raw, payload))
        off += HEADER_SIZE + plen
        if flags & BFLAG_FINAL:
            final_seen = True
            break
    off = min(off, len(data))
    if not final_seen:
        problems.append(("error", "stream has no FINAL block"))
    elif off < len(data):
        problems.append(("warning", "%d bytes after the FINAL block (TE engines have none)"
                         % (len(data) - off)))
    return blocks, problems, off


# --------------------------------------------------------------------------- verification

class Report:
    def __init__(self, path, size):
        self.path = path
        self.size = size
        self.blocks = []
        self.errors = []
        self.warnings = []
        self.slot = None
        self.slot_inferred = False
        self.window = None  # (base, size, label)
        self.exports = []  # (name, name_ptr, target)
        self.kind = None
        self.used = None
        self.payload_bytes = 0
        self.fill_bytes = 0

    def error(self, msg):
        self.errors.append(msg)

    def warn(self, msg):
        self.warnings.append(msg)

    def ok(self, strict=False):
        return not self.errors and not (strict and self.warnings)


class Image:
    """The slot window as the loader leaves it: bytes plus a 'written' mask."""

    def __init__(self, base, size):
        self.base = base
        self.size = size
        self.mem = bytearray(size)
        self.written = bytearray(size)
        self.is_code = bytearray(size)  # 1 where a data (non-FILL) block wrote

    def contains(self, addr, n=1):
        return self.base <= addr and addr + n <= self.base + self.size

    def write(self, addr, payload, fill=False):
        lo = max(addr, self.base)
        hi = min(addr + len(payload), self.base + self.size)
        if hi <= lo:
            return
        a, b = lo - self.base, hi - self.base
        self.mem[a:b] = payload[lo - addr:hi - addr]
        self.written[a:b] = b"\x01" * (b - a)
        self.is_code[a:b] = (b"\x00" if fill else b"\x01") * (b - a)

    def readable(self, addr, n):
        if not self.contains(addr, n):
            return False
        a = addr - self.base
        return all(self.written[a:a + n])

    def u32(self, addr):
        a = addr - self.base
        return struct.unpack_from("<I", self.mem, a)[0]

    def cstring(self, addr, limit=64):
        if not self.contains(addr):
            return None
        a = addr - self.base
        out = bytearray()
        while a < self.size and self.written[a] and len(out) <= limit:
            c = self.mem[a]
            if c == 0:
                return out.decode("ascii", "replace")
            out.append(c)
            a += 1
        return None


def verify(data, path="<engine>", slot=None):
    rep = Report(path, len(data))
    blocks, problems, _ = parse_stream(data)
    rep.blocks = blocks
    for sev, msg in problems:
        (rep.error if sev == "error" else rep.warn)(msg)
    if not blocks:
        rep.error("empty file")
        return rep

    # --- per-header checks (marker, checksum, flags) -------------------------
    for b in blocks:
        where = "block %d at 0x%04x" % (b.index, b.offset)
        if b.marker != MARKER:
            rep.error("%s: marker 0x%02x != 0xAD (OS: 'Bad header')" % (where, b.marker))
        if b.xor != 0:
            rep.error("%s: header XOR is 0x%02x, must be 0 (OS: 'Bad header checksum')"
                      % (where, b.xor))
        unknown = b.flags & ~KNOWN_FLAGS & 0xFFFF
        if unknown:
            rep.warn("%s: flags 0x%04x not used by TE engines (%s)" % (where, unknown, b.kind))
        if b.is_fill and b.argument != 0:
            rep.warn("%s: FILL value 0x%x; the OS logs 'Unsupported fill value' and "
                     "zero-fills the block anyway" % (where, b.argument))

    # --- TE stream layout -----------------------------------------------------
    first, last = blocks[0], blocks[-1]
    if first.flags != TE_FIRST_FLAGS or first.target != STREAM_TARGET or first.count:
        rep.warn("block 0 is %s target 0x%08x count %d; TE uses FIRST|IGNORE (0x5000) at "
                 "0x%08x with no payload" % (first.kind, first.target, first.count, STREAM_TARGET))
    if last.is_final:
        if first.flags & BFLAG_FIRST and first.argument != last.offset:
            rep.warn("FIRST argument 0x%x != offset of the FINAL header 0x%x (TE layout)"
                     % (first.argument, last.offset))
        if last.flags != TE_FINAL_FLAGS or last.target != STREAM_TARGET or last.count:
            rep.warn("FINAL block is %s target 0x%08x count %d; TE uses FINAL (0x8000) at "
                     "0x%08x with no payload" % (last.kind, last.target, last.count, STREAM_TARGET))
    for b in blocks[1:-1]:
        if b.flags & (BFLAG_FIRST | BFLAG_FINAL):
            rep.warn("block %d: %s in the middle of the stream" % (b.index, b.kind))

    loaded = [b for b in blocks if b.loads]
    for b in loaded:
        if b.is_fill:
            rep.fill_bytes += b.count
        else:
            rep.payload_bytes += b.count
    if not loaded:
        rep.error("no loadable blocks")
        return rep

    # --- slot window ------------------------------------------------------------
    ranges = [(b.target, b.count) for b in loaded]
    if slot is None:
        slot = infer_slot(ranges)
        rep.slot_inferred = True
        if slot is None:
            lo = min(a for a, _ in ranges)
            hi = max(a + n for a, n in ranges)
            rep.error("no slot 0-%d window holds 0x%08x-0x%08x; pass --slot to check against one"
                      % (MAX_CHECKED_SLOT, lo, hi))
            return rep
    elif slot > MAX_CHECKED_SLOT:
        rep.warn("slot %d is above %d; memory there has not been checked (Q-16)"
                 % (slot, MAX_CHECKED_SLOT))
    rep.slot = slot
    base, size, label = slot_window(slot)
    rep.window = (base, size, label)
    img = Image(base, size)
    for b in loaded:
        end = b.target + b.count
        if not img.contains(b.target, b.count):
            rep.error("block %d: 0x%08x-0x%08x outside slot %d window 0x%08x-0x%08x "
                      "(OS: 'Bad address')" % (b.index, b.target, end, slot, base, base + size))
    # Overlaps are legal for the loader (later blocks win) but never intended.
    spans = sorted((b.target, b.target + b.count, b.index) for b in loaded)
    for (a0, a1, i), (b0, b1, j) in zip(spans, spans[1:]):
        if b0 < a1:
            rep.warn("blocks %d and %d overlap at 0x%08x-0x%08x" % (i, j, b0, min(a1, b1)))
    for b in loaded:
        # The OS zero-fills FILL blocks whatever the value (see above).
        img.write(b.target, bytes(b.count) if b.is_fill else b.payload, fill=b.is_fill)
    rep.used = max(a + n for a, n in ranges) - base

    # --- export table -----------------------------------------------------------
    if loaded[0].target != base or loaded[0].is_fill:
        rep.warn("first loaded block is at 0x%08x, not the slot base 0x%08x (TE layout puts "
                 "the export table there)" % (loaded[0].target, base))
    if not img.readable(base, 8) or (img.u32(base), img.u32(base + 4)) != EXPORT_MAGIC:
        got = "unloaded" if not img.readable(base, 8) else "%08x %08x" % (img.u32(base),
                                                                         img.u32(base + 4))
        rep.error("no export table magic feed0ad1 ad1facef at slot base 0x%08x (found %s)"
                  % (base, got))
        return rep
    addr = base + 8
    terminated = False
    for _ in range(MAX_EXPORTS):
        if not img.readable(addr, 8):
            break
        name_ptr, target = img.u32(addr), img.u32(addr + 4)
        addr += 8
        if name_ptr == 0:
            terminated = True
            if target != 0:
                rep.warn("export terminator at 0x%08x has non-zero target 0x%08x"
                         % (addr - 8, target))
            break
        name = img.cstring(name_ptr)
        if name is None:
            rep.error("export name pointer 0x%08x is not a NUL-terminated string inside the "
                      "slot" % name_ptr)
            name = "<0x%08x>" % name_ptr
        rep.exports.append((name, name_ptr, target))
    if not terminated:
        rep.error("export table at 0x%08x has no zero terminator within %d entries"
                  % (base, MAX_EXPORTS))

    names = [n for n, _, _ in rep.exports]
    for n in set(names):
        if names.count(n) > 1:
            rep.warn("export %r appears %d times; the loader uses the first" % (n, names.count(n)))
    for name, _, target in rep.exports:
        if not img.contains(target):
            rep.error("export %s -> 0x%08x is outside the slot window" % (name, target))
        elif not img.is_code[target - base]:
            rep.warn("export %s -> 0x%08x is not inside a data block" % (name, target))
    missing = [n for n in REQUIRED_EXPORTS if n not in names]
    if missing:
        rep.error("missing exports: %s (OS: 'Could not lookup symbols for engine')"
                  % ", ".join(missing))
    has = [n for n in RENDER_EXPORTS if n in names]
    if not has:
        rep.error("exports neither render (instrument) nor renderFX (effect)")
    elif len(has) == 2:
        rep.error("exports both render and renderFX; the OS would treat it as an instrument "
                  "and ignore renderFX")
    else:
        rep.kind = "instrument" if has[0] == "render" else "effect"
    return rep


# --------------------------------------------------------------------------- output

def report_dict(rep, strict=False):
    d = {
        "file": rep.path,
        "size": rep.size,
        "pass": rep.ok(strict),
        "errors": rep.errors,
        "warnings": rep.warnings,
        "slot": rep.slot,
        "slot_inferred": rep.slot_inferred,
        "kind": rep.kind,
        "blocks": [{
            "index": b.index, "offset": b.offset, "flags": b.flags, "kind": b.kind,
            "target": b.target, "count": b.count, "argument": b.argument,
        } for b in rep.blocks],
        "exports": [{"name": n, "name_ptr": p, "target": t} for n, p, t in rep.exports],
    }
    if rep.window:
        base, size, label = rep.window
        d.update(window_base=base, window_size=size, slot_class=label, used=rep.used,
                 remaining=size - rep.used, payload_bytes=rep.payload_bytes,
                 fill_bytes=rep.fill_bytes)
    return d


def print_report(rep, strict=False, out=None):
    w = (out or sys.stdout).write
    w("engine   %s  (%d bytes, %d blocks)\n" % (rep.path, rep.size, len(rep.blocks)))
    if rep.window:
        base, size, label = rep.window
        w("slot     %d  (%s class%s)  window 0x%08x-0x%08x\n"
          % (rep.slot, label, ", inferred from load addresses" if rep.slot_inferred else "",
             base, base + size))
    w("\nblocks\n")
    w("    #  offset  flags   kind           target      end         count   argument\n")
    for b in rep.blocks:
        end = "-" if not b.loads else "0x%08x" % (b.target + b.count)
        w("  %3d  0x%04x  0x%04x  %-13s  0x%08x  %-10s  0x%04x  0x%x\n"
          % (b.index, b.offset, b.flags, b.kind, b.target, end, b.count, b.argument))
    if rep.exports:
        w("\nexports%s\n" % ("  (%s)" % rep.kind if rep.kind else ""))
        for name, ptr, target in rep.exports:
            off = ""
            if rep.window:
                off = "  base+0x%04x" % (target - rep.window[0])
            w("  %-12s 0x%08x%s   (name @ 0x%08x)\n" % (name, target, off, ptr))
    if rep.window and rep.used is not None:
        base, size, _ = rep.window
        w("\nusage    %d of %d bytes used (0x%x), %d free (%.1f%%); payload %d, bss %d\n"
          % (rep.used, size, rep.used, size - rep.used, 100.0 * rep.used / size,
             rep.payload_bytes, rep.fill_bytes))
    w("\n")
    for msg in rep.errors:
        w("ERROR    %s\n" % msg)
    for msg in rep.warnings:
        w("WARNING  %s\n" % msg)
    w("result   %s  (%d errors, %d warnings%s)\n"
      % ("PASS" if rep.ok(strict) else "FAIL", len(rep.errors), len(rep.warnings),
         ", strict" if strict else ""))


def verify_file(path, slot=None):
    with open(path, "rb") as f:
        data = f.read()
    return verify(data, path, slot)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Verify an OP-Z .engine file against the OS "
                                             "loader checks.")
    ap.add_argument("engine", help="the .engine file")
    ap.add_argument("--slot", type=int, help="memory slot to check against (default: infer "
                                             "from load addresses)")
    ap.add_argument("--strict", action="store_true", help="treat warnings as failures")
    ap.add_argument("--json", action="store_true", help="print the report as JSON")
    args = ap.parse_args(argv)
    if args.slot is not None and args.slot < 0:
        ap.error("--slot must be >= 0")
    try:
        rep = verify_file(args.engine, args.slot)
    except OSError as e:
        print("opz_verify: %s" % e, file=sys.stderr)
        return 2
    if args.json:
        json.dump(report_dict(rep, args.strict), sys.stdout, indent=2)
        sys.stdout.write("\n")
    else:
        print_report(rep, args.strict)
    return 0 if rep.ok(args.strict) else 1


if __name__ == "__main__":
    sys.exit(main())
