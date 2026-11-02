#!/usr/bin/env python3
"""Look for the things that crash or corrupt an OP-Z, offline, in our engines.

Usage:
    opz_crashcheck.py engines/NAME [engines/NAME ...] [--all] [--static-only] [--full] [--json FILE]

What makes the OP-Z restart is a CPU exception inside an engine: TE's exception handler (opz.ldr 0x8002c2ec,
installed for every EXCAUSE 0x00-0x2e) logs it and reboots through the watchdog. What corrupts it silently is an
engine writing outside its own memory or handing back registers the OS relies on. Background, addresses and
evidence: research/crash-safety.md; the rules: sdk/docs/engine-abi.md §6.

Static checks, on the device build (bfin-elf-objdump of build/bfin/NAME/NAME.elf):
  * instructions an engine must never run: EXCPT (a software exception: the OS reboots), RAISE, CLI/STI, IDLE,
    RTI/RTX/RTN/RTE, writes to SYSCFG/SEQSTAT/RETx, cache flushes, and anything objdump cannot decode;
  * writes to L0-L3 (circular addressing; the OS assumes L = 0 at every call);
  * calls to libgcc's division helpers (a divisor of 0 does not trap on the Blackfin, it returns garbage; the
    helpers are also slow): listed per function, for review;
  * worst-case stack per entry point from the call graph (LINK size, pushes, SP adjustments); recursion and
    indirect calls (host callbacks) are named.

Runtime checks, in the GNU Blackfin simulator (tools/opz_cycles.py with tools/cycles/checks.S), on every call
of every benchmark case: the stack actually used per entry point, L0-L3 and LC0/LC1 left non-zero, callee-saved
registers or SP not restored, writes past the device's voice state (280 bytes) or FX state (200 bytes), past
the output buffer, the envelope or the FX buffer, into params or the FX input, into the engine's own code and
constants or past its .bss, the scratch buffer used beyond 256 bytes, and every exception the simulator raises
(misaligned access, NULL page, unmapped address, illegal instruction). After each case's first note-on and a
re-trigger at its end, render also runs on mixes of the state before and after onNoteOn: on the device the OS
starts the voice before it calls onNoteOn from a task, and the audio interrupt can run render in between.
Besides the benchmark cases it plays every preset in presets.json (held, then with the knobs moving), P1/P2
at words outside the knob range, and the OS's worst parameter streams (tools/opz_params.py).

--quick (the default) runs the short benchmark case lists; --full runs the complete ones.
Exit status: 0 = no errors, 1 = an engine has errors, 2 = usage or toolchain problem.
Python 3 standard library only.
"""

import argparse
import contextlib
import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import opz_build  # noqa: E402
import opz_cycles  # noqa: E402
import opz_params  # noqa: E402

ENTRY_POINTS = ("initModule", "init", "onNoteOn", "onNoteOff", "render", "renderFX")
DIVISION_HELPERS = ("__divsi3", "__udivsi3", "__modsi3", "__umodsi3", "__divdi3", "__udivdi3", "__moddi3",
                    "__umoddi3", "__udivmoddi4", "__divmoddi4")

# Stack per engine call. The OS runs render/renderFX in the audio interrupt on a 10 KB interrupt stack
# (opz.ldr: top 0x81aec9e0, base 0x81aea1e0, switched to at 0x11a02804) after ~0.5-1 KB of its own frames, and
# other interrupts nest on the same stack. [code; the limits themselves are our margin, inferred]
STACK_WARN = 1024
STACK_ERROR = 2048

FORBIDDEN = [
    (re.compile(r"\bEXCPT\b"), "EXCPT: a software exception; TE's handler logs it and reboots the OP-Z"),
    (re.compile(r"\bEMUEXCPT\b"), "EMUEXCPT: emulation exception"),
    (re.compile(r"\bRAISE\b"), "RAISE: raises an interrupt"),
    (re.compile(r"\bCLI\b"), "CLI: masks every interrupt (the audio, USB and timer interrupts stop)"),
    (re.compile(r"\bSTI\b"), "STI: unmasks interrupts the OS has masked (effect init runs with IMASK = 0)"),
    (re.compile(r"\bIDLE\b"), "IDLE: stops the core until an interrupt"),
    (re.compile(r"\bRT[IXNE]\b"), "return from interrupt/exception: corrupts the OS's interrupt state"),
    (re.compile(r"\b(SYSCFG|SEQSTAT|RETI|RETX|RETN|RETE|EMUDAT)\s*="), "writes a system register"),
    (re.compile(r"\b(FLUSH|FLUSHINV|IFLUSH)\b"), "cache flush: changes memory the OS is using"),
    (re.compile(r"ILLEGAL"), "objdump cannot decode this: data in .text or an illegal instruction"),
]
L_WRITE = re.compile(r"\bL[0-3]\s*=")

def cname(sym):
    """ELF symbol -> C name (the Blackfin ABI prefixes one underscore)."""
    return sym[1:] if sym.startswith("_") else sym


FUNC_RE = re.compile(r"^([0-9a-f]{8}) <([^>]+)>:$")
INSN_RE = re.compile(r"^\s*([0-9a-f]+):\t[0-9a-f ]+\t(.*)$")
CALL_RE = re.compile(r"\bCALL 0x[0-9a-f]+ <([^>+]+)(\+0x[0-9a-f]+)?>")
JUMP_RE = re.compile(r"\bJUMP(?:\.[LSX])? 0x[0-9a-f]+ <([^>+]+)(\+0x[0-9a-f]+)?>")
LINK_RE = re.compile(r"\bLINK 0x([0-9a-f]+)")
PUSHM_RE = re.compile(r"\[--SP\] = \(([^)]*)\)")
PUSH_RE = re.compile(r"\[--SP\] = [A-Z]")
SPADJ_RE = re.compile(r"\bSP \+= -0x([0-9a-f]+)")


# --------------------------------------------------------------------------- static analysis

def parse_listing(text):
    """objdump -d text -> {function: [(address, instruction text), ...]} (comments removed)."""
    funcs, cur = {}, None
    for line in text.splitlines():
        m = FUNC_RE.match(line)
        if m:
            cur = cname(m.group(2))
            funcs[cur] = []
            continue
        m = INSN_RE.match(line)
        if m and cur is not None:
            insn = re.sub(r"/\*.*?\*/", "", m.group(2)).strip()
            if insn:
                funcs[cur].append((int(m.group(1), 16), insn))
    return funcs


def push_bytes(regs):
    """Bytes pushed by [--SP] = (R7:4, P5:3)."""
    n = 0
    for part in regs.split(","):
        m = re.match(r"\s*[RP](\d):(\d)\s*$", part)
        if m:
            n += int(m.group(1)) - int(m.group(2)) + 1
    return 4 * n


def analyse_function(insns, name):
    frame, calls, tails, indirect = 0, set(), set(), False
    for _, insn in insns:
        for part in insn.split("||"):
            m = LINK_RE.search(part)
            if m:
                frame += int(m.group(1), 16) + 8          # LINK pushes RETS and FP, then allocates
            m = PUSHM_RE.search(part)
            if m:
                frame += push_bytes(m.group(1))
            elif PUSH_RE.search(part):
                frame += 4
            m = SPADJ_RE.search(part)
            if m:
                frame += int(m.group(1), 16)
            m = CALL_RE.search(part)
            if m:
                calls.add(cname(m.group(1)))
            elif re.search(r"\bCALL \(P\d\)", part):
                indirect = True
            m = JUMP_RE.search(part)
            if m and cname(m.group(1)) != name:
                tails.add(cname(m.group(1)))
            if re.search(r"\bJUMP \(P\d\)", part):
                pass                                        # switch tables jump inside the function
    return {"frame": frame, "calls": sorted(calls), "tails": sorted(tails), "indirect": indirect}


def stack_depths(funcs):
    """Worst-case stack per function: frame + deepest call; a tail jump replaces the frame. Returns
    ({function: bytes}, {function: uses an indirect call somewhere below}, [recursive functions])."""
    info = {f: analyse_function(i, f) for f, i in funcs.items()}
    depth, indirect, recursive, visiting = {}, {}, set(), set()

    def walk(f):
        if f in depth:
            return depth[f]
        if f not in info:
            return 0
        if f in visiting:
            recursive.add(f)
            return 0
        visiting.add(f)
        fi = info[f]
        below = max([walk(c) for c in fi["calls"]] or [0])
        tail = max([walk(t) for t in fi["tails"]] or [0])
        indirect[f] = fi["indirect"] or any(indirect.get(c) for c in fi["calls"] + fi["tails"])
        visiting.discard(f)
        depth[f] = max(fi["frame"] + below, tail)
        return depth[f]

    for f in info:
        walk(f)
    return depth, indirect, sorted(recursive)


def static_checks(listing):
    funcs = parse_listing(listing)
    findings = []
    for f, insns in funcs.items():
        for addr, insn in insns:
            for part in insn.split("||"):
                for rx, why in FORBIDDEN:
                    if rx.search(part):
                        findings.append({"level": "error", "check": "instruction", "function": f,
                                         "address": "0x%08x" % addr, "insn": insn, "detail": why})
                        break
                if L_WRITE.search(part):
                    findings.append({"level": "warning", "check": "L register", "function": f,
                                     "address": "0x%08x" % addr, "insn": insn,
                                     "detail": "sets an L register (circular addressing): it must be 0 again before "
                                               "every return and call (the runtime check verifies the return)"})
    divisions = {}
    for f, insns in funcs.items():
        if f in DIVISION_HELPERS:
            continue
        for _, insn in insns:
            for m in CALL_RE.finditer(insn):
                callee = cname(m.group(1))
                if callee in DIVISION_HELPERS:
                    divisions.setdefault(f, set()).add(callee)
    depth, indirect, recursive = stack_depths(funcs)
    stacks = {e: depth[e] for e in ENTRY_POINTS if e in depth}
    for e, n in stacks.items():
        if n > STACK_ERROR:
            findings.append({"level": "error", "check": "stack (static)", "function": e,
                             "detail": "worst case %d bytes per call (limit %d)" % (n, STACK_ERROR)})
        elif n > STACK_WARN:
            findings.append({"level": "warning", "check": "stack (static)", "function": e,
                             "detail": "worst case %d bytes per call (warning above %d)" % (n, STACK_WARN)})
    for f in recursive:
        findings.append({"level": "warning", "check": "recursion", "function": f,
                         "detail": "recursive: the stack bound above does not hold"})
    return {"stack": stacks, "indirect": {e: bool(indirect.get(e)) for e in stacks},
            "divisions": {f: sorted(h) for f, h in sorted(divisions.items())},
            "findings": findings}


# --------------------------------------------------------------------------- runtime (simulator)

def extra_cases(engine_dir, is_fx, quick):
    """Cases beyond the cycle benchmark's: every preset of presets.json (choosing macro's "reed" preset crashed the
    OP-Z on 2026-10-19), held, with P1 and P2 then sweeping from there; and P1/P2 at parameter words outside the
    knob range (0x80000000 and 0xffffffff), which the OS never sends but a wrong guess about it might; and the
    OS's worst real parameter streams (opz_params.stream_cases: LFO at full depth and rate, jumps, step locks,
    pitch and pitch bend extremes; research/param-paths.md)."""
    with open(os.path.join(engine_dir, "plug.json")) as f:
        mask = int(json.load(f)["plug"]["trackmask"])
    drum = not is_fx and not (mask & 0xf0)
    blocks = 24 if quick else 64
    out = []
    path = os.path.join(engine_dir, "presets.json")
    presets = []
    if os.path.exists(path):
        with open(path) as f:
            presets = json.load(f)
    for pr in presets:
        values = list(pr["values"])[:18]
        if len(values) != 18:
            continue
        if is_fx:
            notes, track = [255], 8
        elif drum:
            notes, track = [53, 57, 61, 65], 0
        else:
            shift = 12 * int(pr.get("default_octave", 0))
            notes, track = [min(127, max(0, n + shift)) for n in opz_cycles.CHORD], 4 + int(pr.get("slot", 0)) % 4
        label = "preset %s/%s %s" % (pr.get("slot"), pr.get("index"), pr.get("name", ""))
        out.append(opz_cycles.case(label, values, notes, track, blocks, warmup=8 if is_fx else 0, group="presets"))
        out.append(opz_cycles.case(label + ", knobs moving", values, notes, track, max(blocks, 24),
                                   warmup=8 if is_fx else 0, knobs=8, group="presets"))
    notes, track = ([255], 8) if is_fx else (([53, 60], 0) if drum else (opz_cycles.CHORD, 4))
    for raw in ((0x80000000, 0x80000000), (0xffffffff, 0xffffffff), (0x80000000, 0x7fffffff)):
        out.append(opz_cycles.case("P1/P2 words 0x%08x/0x%08x (outside the knob range)" % raw, opz_cycles.params(),
                                   notes, track, 16, warmup=8 if is_fx else 0, raw=raw, group="raw"))
    return out + opz_params.stream_cases(is_fx, drum, quick)


def runtime_checks(engine_dir, toolchain, out_root, quick, blackfin_plus=False):
    """Run the benchmark cases with the checks of tools/cycles/checks.S. Returns the check part of the report."""
    name = os.path.basename(os.path.abspath(engine_dir).rstrip("/"))
    with open(os.path.join(engine_dir, "plug.json")) as f:
        is_fx, drum = opz_cycles.plug_kind(json.load(f))
    cases = opz_cycles.cases_for(name, is_fx, quick, drum) + extra_cases(engine_dir, is_fx, quick)
    try:
        rep = opz_cycles.measure(engine_dir, toolchain, out_root, cases=cases, quiet=True, torn=True,
                                 blackfin_plus=blackfin_plus)
    except opz_cycles.SimulatorCrash as e:
        return {"crash": e.info, "findings": [{"level": "error", "check": "exception", "function": e.info.get("where"),
                                                "detail": e.info["message"]}]}
    return rep["checks"]


# --------------------------------------------------------------------------- report

def check_engine(engine_dir, toolchain, out_root, static_only=False, quick=True, blackfin_plus=False):
    edir = os.path.abspath(engine_dir)
    name = os.path.basename(edir.rstrip("/"))
    with contextlib.redirect_stdout(io.StringIO()):
        elf, _, slot, _, _ = opz_build.build(
            edir, toolchain, os.path.join(out_root, "bfin-bfp" if blackfin_plus else "bfin"),
            defines=("OPZ_BLACKFIN_PLUS",) if blackfin_plus else ())
    listing = opz_build.run([os.path.join(toolchain, "bin", "bfin-elf-objdump"), "-d", elf])
    report = {"engine": name, "slot": slot, "static": static_checks(listing)}
    findings = list(report["static"]["findings"])
    if not static_only:
        rt = runtime_checks(edir, toolchain, out_root, quick, blackfin_plus)
        if "renderFX" in report["static"]["stack"]:          # the driver calls both "render"
            if "render" in rt.get("stack", {}):
                rt["stack"]["renderFX"] = rt["stack"].pop("render")
            for f in rt["findings"]:
                if f.get("function") == "render":
                    f["function"] = "renderFX"
        for e, n in rt.get("stack", {}).items():
            if n > STACK_ERROR or (n > STACK_WARN and report["static"]["stack"].get(e, 0) <= STACK_WARN):
                rt["findings"].append({"level": "error" if n > STACK_ERROR else "warning", "check": "stack (used)",
                                       "function": e, "detail": "%d bytes in one call (warning above %d, error "
                                                                "above %d)" % (n, STACK_WARN, STACK_ERROR)})
        report["runtime"] = rt
        findings += rt["findings"]
    report["errors"] = sum(1 for f in findings if f["level"] == "error")
    report["warnings"] = sum(1 for f in findings if f["level"] == "warning")
    return report


def print_report(r, out=sys.stdout):
    st = r["static"]
    verdict = "FAIL" if r["errors"] else ("warnings" if r["warnings"] else "ok")
    out.write("%s (slot %d): %s, %d error(s), %d warning(s)\n" % (r["engine"], r["slot"], verdict, r["errors"],
                                                                 r["warnings"]))
    rt = r.get("runtime", {})
    used = rt.get("stack", {})
    line = []
    for e in ENTRY_POINTS:
        if e in st["stack"] or e in used:
            s = "%s %s" % (e, st["stack"].get(e, "?"))
            if e in used:
                s += "/%d" % used[e]
            if st["indirect"].get(e):
                s += "+cb"
            line.append(s)
    out.write("  stack bytes (static worst case/used in the simulator; +cb = plus a host callback): %s\n"
              % ", ".join(line))
    if "scratch" in rt:
        out.write("  scratch buffer used: %d bytes\n" % rt["scratch"])
    if st["divisions"]:
        out.write("  divisions (no trap on 0; check the divisor): %s\n" % "; ".join(
            "%s -> %s" % (f, ", ".join(h)) for f, h in st["divisions"].items()))
    for f in st["findings"] + rt.get("findings", []):
        where = f.get("function") or "?"
        if f.get("address"):
            where += " " + f["address"]
        out.write("  %-7s %-16s %s: %s%s\n" % (f["level"], f["check"], where, f["detail"],
                                               (" [%s]" % f["insn"]) if f.get("insn") else ""))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("engine_dirs", nargs="*")
    ap.add_argument("--all", action="store_true", help="every engine under engines/")
    ap.add_argument("--static-only", action="store_true", help="skip the simulator run")
    ap.add_argument("--full", action="store_true", help="the complete benchmark cases (default: --quick ones)")
    ap.add_argument("--toolchain", default=os.path.expanduser("~/opt/bfin-elf"))
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "build"))
    ap.add_argument("--json", help="write the reports as JSON")
    ap.add_argument("--blackfin-plus", action="store_true",
                    help="EXPERIMENTAL: check the OPZ_BLACKFIN_PLUS build on our Blackfin+ simulator (Q-47)")
    a = ap.parse_args(argv)
    dirs = list(a.engine_dirs)
    if a.all:
        edir = os.path.join(ROOT, "engines")
        dirs += sorted(os.path.join(edir, d) for d in os.listdir(edir)
                       if os.path.isfile(os.path.join(edir, d, "plug.json")))
    if not dirs:
        ap.error("name an engine directory or pass --all")
    if not os.access(os.path.join(a.toolchain, "bin", "bfin-elf-objdump"), os.X_OK) or (
            not a.static_only and not opz_cycles.available(a.toolchain)):
        sys.stderr.write("opz_crashcheck: bfin-elf toolchain not found in %s/bin (research/toolchain-setup.md)\n"
                         % a.toolchain)
        return 2
    reports = []
    for d in dirs:
        r = check_engine(d, a.toolchain, a.out, static_only=a.static_only, quick=not a.full,
                         blackfin_plus=a.blackfin_plus)
        print_report(r)
        reports.append(r)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(reports, f, indent=1)
    return 1 if any(r["errors"] for r in reports) else 0


if __name__ == "__main__":
    sys.exit(main())
