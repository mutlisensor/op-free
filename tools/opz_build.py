#!/usr/bin/env python3
"""Build an OP-Z engine for the Blackfin: compile, link into its memory slot, pack, verify.

Usage:
    opz_build.py engines/NAME [--toolchain ~/opt/bfin-elf] [-o build/bfin]

Reads engines/NAME/plug.json for the memory slot, fills sdk/ld/engine.ld.in with that slot's window,
compiles every .c in the engine directory with bfin-elf-gcc, links with libgcc, converts the ELF to an
.engine with opz_pack.py and checks it with opz_verify.py. Outputs in build/bfin/NAME/:
NAME.elf, NAME.map, NAME.lst (disassembly), NAME.engine.

Python 3 standard library only.
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import opz_verify  # noqa: E402

CFLAGS = [
    "-std=c11", "-O2", "-Wall", "-Wextra",
    "-mcpu=bf592-none",            # classic Blackfin ISA, no silicon-anomaly workarounds (BF70x is a superset)
    "-ffreestanding", "-fno-builtin", "-fno-common", "-fno-asynchronous-unwind-tables",
    "-ffunction-sections", "-fdata-sections",
]
LDFLAGS = ["-nostdlib", "-nostartfiles", "-Wl,--gc-sections", "-Wl,--no-undefined"]


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.stderr.write(" ".join(cmd) + "\n" + r.stdout + r.stderr)
        sys.exit(r.returncode)
    return r.stdout


def build(engine_dir, toolchain, out_root, defines=()):
    """Compile, link, pack and verify one engine. Returns (elf path, engine path, slot, base, size).
    defines: extra preprocessor names, e.g. ("OPZ_BLACKFIN_PLUS",)."""
    edir = os.path.abspath(engine_dir)
    name = os.path.basename(edir.rstrip("/"))
    with open(os.path.join(edir, "plug.json")) as f:
        plug = json.load(f)
    slot = int(plug["memoryslot"])
    base, size, _ = opz_verify.slot_window(slot)

    out = os.path.join(os.path.abspath(out_root), name)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(ROOT, "sdk", "ld", "engine.ld.in")) as f:
        ld = f.read().replace("@BASE@", "0x%08x" % base).replace("@SIZE@", "0x%x" % size)
    ld_path = os.path.join(out, "engine.ld")
    with open(ld_path, "w") as f:
        f.write(ld)

    gcc = os.path.join(toolchain, "bin", "bfin-elf-gcc")
    objdump = os.path.join(toolchain, "bin", "bfin-elf-objdump")
    sources = sorted(os.path.join(edir, s) for s in os.listdir(edir) if s.endswith(".c"))
    elf = os.path.join(out, name + ".elf")
    run([gcc] + CFLAGS + ["-D" + d for d in defines] + ["-I", os.path.join(ROOT, "sdk", "include")] + sources + LDFLAGS
        + ["-T", ld_path, "-Wl,-Map," + os.path.join(out, name + ".map"), "-o", elf, "-lgcc"])
    with open(os.path.join(out, name + ".lst"), "w") as f:
        f.write(run([objdump, "-d", "-t", elf]))

    engine = os.path.join(out, name + ".engine")
    run([sys.executable, os.path.join(HERE, "opz_pack.py"), "elf", elf, "-o", engine, "--slot", str(slot)])
    print(run([sys.executable, os.path.join(HERE, "opz_verify.py"), engine, "--slot", str(slot)]).rstrip())
    print("built %s (slot %d, window 0x%08x, %d bytes)" % (engine, slot, base, os.path.getsize(engine)))
    return elf, engine, slot, base, size


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("engine_dir")
    ap.add_argument("--toolchain", default=os.path.expanduser("~/opt/bfin-elf"))
    ap.add_argument("-o", "--out", help="output folder (default build/bfin, build/bfin-bfp with --blackfin-plus)")
    ap.add_argument("--blackfin-plus", action="store_true",
                    help="EXPERIMENTAL: build with OPZ_BLACKFIN_PLUS (one-instruction opz_mul_q31; Q-47)")
    args = ap.parse_args()
    out = args.out or os.path.join(ROOT, "build", "bfin-bfp" if args.blackfin_plus else "bfin")
    build(args.engine_dir, args.toolchain, out, defines=("OPZ_BLACKFIN_PLUS",) if args.blackfin_plus else ())


if __name__ == "__main__":
    main()
