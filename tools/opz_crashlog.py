#!/usr/bin/env python3
"""Decode an OP-Z crash log: which exception, where, and in which of our engines.

Usage:
    opz_crashlog.py LOG [LOG ...] [--toolchain DIR] [--build DIR]

When an engine (or the OS) takes a CPU exception, TE's handler in main.cpp (opz.ldr 0x8002c2ec) logs
"exception caught iid: 0x0013XXXX", the cause ("exc reason: ADI_EXC_..."), the CPLB fault address for a CPLB miss,
RETX (the faulting instruction) and the call stack ("0x%08x" per frame, callstack.cpp), then restarts the OP-Z.
On the next start the bootloader saves that log as /yaffs2/tmp/crash01.log (keeping five, crash01 = newest) and
copies them to systeminfo/crash01.log ... crash05.log on the update-mode disk (bootloader.ldr 0x8281f436).
See research/crash-safety.md and the recovery runbook in research/crash-recovery.md.

This tool prints the exception and maps every address in the log that falls inside a memory slot window to the
engine of ours that uses that slot (engines/*/plug.json) and, when its device build is in build/bfin/NAME/NAME.elf,
to function + offset. The mapping is only right if that build is the one installed on the OP-Z.
Python 3 standard library only (bfin-elf-nm for function names, if installed).
"""

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import opz_verify  # noqa: E402

# EXCAUSE values (SEQSTAT[5:0]) [notes: ADI Blackfin programming reference; names as in TE's table at 0x81003ea4]
EXCAUSE = {
    0x10: "single step", 0x11: "trace buffer full", 0x21: "undefined instruction",
    0x22: "illegal instruction combination", 0x23: "data access protection violation (CPLB)",
    0x24: "misaligned data access", 0x25: "unrecoverable event (an exception inside the exception handler)",
    0x26: "data CPLB miss (address not mapped: NULL page, wild pointer)", 0x27: "data CPLB multiple hits",
    0x28: "emulation watchpoint", 0x2a: "misaligned instruction", 0x2b: "instruction protection violation",
    0x2c: "instruction CPLB miss (jump or call to an unmapped address)", 0x2d: "instruction CPLB multiple hits",
    0x2e: "supervisor resource used in user mode",
}

IID_RE = re.compile(r"exception caught iid: 0x([0-9a-fA-F]{8}), arg: 0x([0-9a-fA-F]{8})")
REASON_RE = re.compile(r"exc reason: (\S+)")
FAULT_RE = re.compile(r"(DCPLB_FAULT_ADDR|ICPLB_FAULT_ADDR|L1DM_DSTAT|L1IM_ISTAT): 0x([0-9a-fA-F]{8})")
RETX_RE = re.compile(r"RETX: 0x([0-9a-fA-F]{8})")
ADDR_RE = re.compile(r"0x([0-9a-fA-F]{8})\b")
OVERLOAD_RE = re.compile(r"cpu overload: kill notes (\d+)")


def our_engines():
    """[(engine, slot, base, size)] for engines/*/plug.json."""
    out = []
    edir = os.path.join(ROOT, "engines")
    for name in sorted(os.listdir(edir)):
        p = os.path.join(edir, name, "plug.json")
        if os.path.exists(p):
            with open(p) as f:
                slot = int(json.load(f)["memoryslot"])
            base, size, _ = opz_verify.slot_window(slot)
            out.append((name, slot, base, size))
    return out


def symbols(elf, toolchain):
    nm = os.path.join(toolchain, "bin", "bfin-elf-nm")
    if not (os.path.exists(elf) and os.access(nm, os.X_OK)):
        return None
    r = subprocess.run([nm, "-n", elf], capture_output=True, text=True)
    syms = []
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            name = parts[2]
            syms.append((int(parts[0], 16), name[1:] if name.startswith("_") else name))
    return syms


def where(addr, engines, build, toolchain, cache):
    """'macro render+0x1a4 (slot 55)', 'slot 12 (not one of ours)', or None (outside the slot windows)."""
    for name, slot, base, size in engines:
        if base <= addr < base + size:
            if name not in cache:
                cache[name] = symbols(os.path.join(build, "bfin", name, name + ".elf"), toolchain)
            syms = cache[name]
            fn = ""
            if syms:
                best = [s for s in syms if s[0] <= addr]
                if best:
                    fn = " %s+0x%x" % (best[-1][1], addr - best[-1][0])
            else:
                fn = " +0x%x (no build/bfin/%s/%s.elf to name the function)" % (addr - base, name, name)
            return "%s%s (slot %d)" % (name, fn, slot)
    for slot in range(0, opz_verify.MAX_CHECKED_SLOT + 1):
        base, size, _ = opz_verify.slot_window(slot)
        if base <= addr < base + size:
            return "slot %d (not one of ours: a TE engine or an empty slot)" % slot
    return None


def decode(text, engines, build, toolchain):
    out, cache = [], {}
    for m in IID_RE.finditer(text):
        iid = int(m.group(1), 16)
        cause = iid & 0xffff
        kind = "exception" if (iid >> 16) == 0x13 else "event 0x%x" % (iid >> 16)
        out.append("%s, EXCAUSE 0x%02x: %s" % (kind, cause, EXCAUSE.get(cause, "?")))
    for m in REASON_RE.finditer(text):
        out.append("reason: %s" % m.group(1))
    for m in FAULT_RE.finditer(text):
        a = int(m.group(2), 16)
        w = where(a, engines, build, toolchain, cache) if "FAULT" in m.group(1) else None
        out.append("%s 0x%08x%s" % (m.group(1), a, (" = " + w) if w else ""))
    for m in RETX_RE.finditer(text):
        a = int(m.group(1), 16)
        w = where(a, engines, build, toolchain, cache)
        out.append("faulting instruction (RETX) 0x%08x%s" % (a, (" = " + w) if w else " (OS code)"))
    seen = set()
    frames = []
    for m in ADDR_RE.finditer(text):
        a = int(m.group(1), 16)
        if a in seen:
            continue
        w = where(a, engines, build, toolchain, cache)
        if w:
            seen.add(a)
            frames.append("  0x%08x = %s" % (a, w))
    if frames:
        out.append("addresses inside slot windows:")
        out += frames
    kills = [int(k) for k in OVERLOAD_RE.findall(text)]
    if kills:
        out.append("cpu overload: %d line(s), up to %d note(s) killed" % (len(kills), max(kills)))
    if not out:
        out.append("no exception report in this log")
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--toolchain", default=os.path.expanduser("~/opt/bfin-elf"))
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    a = ap.parse_args(argv)
    engines = our_engines()
    for path in a.logs:
        with open(path, "rb") as f:
            text = f.read().decode("latin-1")
        print("%s:" % path)
        for line in decode(text, engines, a.build, a.toolchain):
            print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
