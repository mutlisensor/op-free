# Crash safety: what crashes the OP-Z, how to catch it offline, how to get back

Research of 2026-10-19 (overnight session, offline only: no command was sent to the OP-Z). Sources: the decrypted
OS image `opz.ldr` 1.2.45 and `bootloader.ldr`, read with Ghidra (the Blackfin+ extension names the BF70x MMRs) and
with `bfin-elf-objdump` on the extracted blocks; the GNU Blackfin simulator for the offline checks. Evidence labels
as in [CONTRIBUTING.md](../CONTRIBUTING.md); addresses are device addresses in `opz.ldr` unless marked
`bootloader.ldr`. No TE code is quoted. The recovery steps are in
[roadmap/crash-recovery.md](crash-recovery.md), the rules for engine authors in
[engine-abi.md §6](../sdk/docs/engine-abi.md#6-rules-for-engine-authors).

## The short version

1. **A "crash and restart" is a CPU exception, as far as the OS code goes** (but see H-10's first look in §2: the
   "reed" crash left no crash log). Every exception cause (EXCAUSE 0x00–0x2e: misaligned access,
   access to an unmapped address such as the NULL page, illegal instruction, protection violation, `EXCPT n`, an
   exception inside the handler) goes to one handler in TE's `main.cpp`, which logs it and reboots through the
   watchdog [code]. Nothing else resets the OP-Z in normal operation: the watchdog is armed only to reboot on
   purpose, never as a guard [code]. A CPU overload kills a note and does not crash [code]. A hang (an engine
   that never returns, a failed OS assertion) freezes the unit instead [code + inferred].
2. **The OS keeps a crash log**, and we have never looked at it. The handler leaves the last ~7 KB of the OS log
   (exception, cause, fault address, faulting instruction, call stack) in L1 memory; the bootloader saves it as
   `crash01.log` (five kept) and shows it, with `system.log` and the version files, in `systeminfo/` on the
   **update-mode disk** [code, bootloader]. `tools/opz_crashlog.py` decodes it against our builds. This is the
   cheapest way to find the `macro` crash: hardware check H-10 below.
3. **`render` runs in the audio interrupt; `onNoteOn` runs in a task, and the OS marks the voice as playing
   before it calls `onNoteOn`**, with no interrupt masking found on that path [code]. So `render` can run on a
   state that `onNoteOn` has only partly written [inferred: possible, not observed].
4. Stack: `render`/`renderFX` run on a 10 KB interrupt stack shared with the OS's own frames and nested
   interrupts; our engines use 0–348 bytes per call [code; measured].
5. Registers: the OS clears L0–L3, LC0 and LC1 when an interrupt starts and assumes L = 0 in its own code; the C
   ABI's callee-saved registers apply [code + notes].
6. Recovery from a boot loop: **update mode (hold SCREEN at power-on) is always reachable**, because the
   bootloader runs before the OS. Content mode is reachable only if the crash happens after the OS's boot init,
   and it cannot touch `engines/` or `settings/` [code]. Details in the runbook.

## 1. What the OS does on a fault

| Event | Path | What the user sees |
|---|---|---|
| CPU exception, any EXCAUSE 0x00–0x2e | Reset code points EVT3 at the dispatcher `0x11a0251c` (saves R7/ASTAT in L1 scratch, turns the DCPLBs off while it runs, reads `SEQSTAT & 0x3f`, looks up the handler through a byte map at `0x11b01cc8` and the table at `0x11902204`). At reset the CCES CPLB manager `0x11a02050` is registered for 0x23, 0x26, 0x2c, 0x2d (`0x11a000cc`–`0x11a0011c`); later `main.cpp` registers its own handler `0x8002c2ec` for **every** cause 0x00–0x2e (47 calls, `0x8002c4c0`–`0x8002c8d8`, through `0x8013c2b8` → `0x11a0287c` → `0x80141c2c`), replacing the CPLB manager. So a CPLB miss is not refilled: it is fatal [code] | restart after a moment |
| `main.cpp` handler `0x8002c2ec` | Logs "exception caught iid: 0x%08x, arg: 0x%08x" (iid `0x13xxxx`, low half = EXCAUSE), "exc reason: %s" from a name table for 0x22–0x2e at `0x81003ea4`, for a data CPLB miss `DCPLB_FAULT_ADDR` + `L1DM_DSTAT`, for an instruction CPLB miss `ICPLB_FAULT_ADDR` + `L1IM_ISTAT`, then the call stack (`0x800283d0`, `callstack.cpp`: "RETX: 0x%08x" and one "0x%08x" per frame) unless the cause is 0x25 ("Unrecoverable event, skipping callstack.") [code] | — |
| … then | Copies the RAM log ring (logger `0x8012ef80`; ring at `0x81ae3608`) into the L1 mailbox (start `0x11b00410`, at most `0x1bf0` bytes, `0x8002c44e`–`0x8002c488`) and calls the reboot function with reason 10, keeping the log (`0x8002c48e` → `0x80028070`) [code] | — |
| Reboot function `0x80028076(reason, keep, msg)` | Mailbox at `0x11b00400` (pointer at `0x81003090`): `0xdeadf00d`, u16 reason at +4, log start/length at +8/+12 (cleared unless `keep`); clears `RCU0_STAT`, sets bits in `RCU0_BCODE` and `RCU0_CTL`, enables the SEC with source 3 as a fault (`SEC0_SCTL3` = 6, `SEC0_FCTL` = 0x41), loads `WDOG0_CNT` = 20,000,000, enables `WDOG0` and spins. The watchdog's expiry resets the chip [code; the reset itself: notes, BF70x watchdog → SEC fault → RCU] | — |
| Watchdog | `WDOG0_CTL`/`WDOG0_CNT` (`0x20046000`/`0x20046004`) are written **only** in that function (and its twin `0x8281c5ea` in `bootloader.ldr`); nothing kicks or arms it in normal operation [code: every reference to the WDOG0 MMRs in both images] | no protection against hangs |
| Other reboots | `main_task`'s exit path `0x8002cdc6`: reason 0x14 (normal restart) or 10 [code] | restart |
| NMI | Default NMI handler `0x08030000` (in the table at `0x1190220c`): L1 parity errors (`L1IM_IPERR_STAT`/`L1DM_DPERR_STAT`) and other `SEQSTAT` bits → fatal error `0x11a02a20(10, n)` [code] | **freeze** |
| Fatal error / failed assertion | `0x11a02a20`, `0x11a01fe0`, `0x11a03588`: CLI, store (code, sub-code, value, PC) at `0x815ca13c`, IDLE forever. Called by the runtime assertion `0x801385d4` (" -- Runtime Assertion"), `0x80138294`, `0x80138b00` and by the "unhandled interrupt" default `0x11a0264c` [code] | **freeze** until power-off |
| Hardware error (IVG5: bus errors, …) | No handler is registered for event `0x150000`; the only core interrupts the OS enables are IVG6 (core timer, `0x801322d4`), IVG11 (system events), IVG14 (context switch, `0x8013230a`) and IVG15 (`IMASK` = 0x8800 at `0x11a000ba`). IVG5 stays masked: hardware errors are latched and ignored [code] | nothing (silent) |
| CPU overload | `Synth` render kills the quietest releasing voice and logs "cpu overload: kill notes %d" ([ahd-drum-notes-cpu.md](ahd-drum-notes-cpu.md#the-overload-rule)) [code] | a note stops; the line lands in `system.log` |
| An engine call that never returns | The audio interrupt never ends; the tasks (UI, USB, sequencer) stop; no watchdog [inferred from the above] | **freeze** |

Divide by zero does not trap on the Blackfin: libgcc's `__divsi3`/`__udivsi3` return a value [proven in the
simulator; the helpers contain no `EXCPT`].

### Core MMRs on the BF70x

The Blackfin+ core MMRs sit at **`0x1FC0xxxx`**, not at `0xFFE0xxxx` as on the BF5xx: `L1DM_DCTL` `0x1fc00004`,
`DCPLB_FAULT_ADDR` `0x1fc0000c`, `ICPLB_FAULT_ADDR` `0x1fc0100c`, `EVT0` `0x1fc02000`, `IPEND` `0x1fc02108`, core
timer `0x1fc03000`, `BP_CFG` `0x1fc0a000`; system MMRs from `0x20000000` (RCU0 `0x20000000`, SEC0 `0x20004000`,
WDOG0 `0x20046000`, SPORT0 `0x2004d000`) [code: Ghidra's BF70x register labels; the reset code writes EVT2–EVT15
in order at `0x11a00054`]. The OS loads them as 32-bit immediates (`0xda0r`/`0xd800` + high half + low half), which
classic `bfin-elf-objdump` cannot decode (`ILLEGAL`).

## 2. The crash log

| Step | Where | Evidence |
|---|---|---|
| The exception handler leaves the log ring in L1 scratch and reboots with reason 10 | §1 | [code] |
| The OS's own boot stream zero-fills `0x11b00014`–`0x11b004cc`, so only the bootloader can read the mailbox | `opz.ldr` FILL block at `0x11b00014` | [code] |
| The bootloader reads the reason (`0x8281c696`: 0x14 unless the magic is there; clears the magic). With reason 10 it logs "should_boot_from_nand = false" (`bootloader.cpp` line 0x298), skips the key check and the OS, runs `0x8281f436`, then reboots with 0x14 (`0x8281c5bc(0x14, 0)`); the OS boots only when the reason is not 10 and neither boot-mode check (`0x11a04d62`, `0x11a04d7c`) fires | `bootloader.ldr` second stage `0x11a00fe2` | [code, decompiled; the control flow is partly unclear] |
| `0x8281f436`: writes `systeminfo/version.txt` and `serial.txt`; if the mailbox range lies in `0x11b00400`–`0x11b02000`, rotates `/yaffs2/tmp/crash05.log` … `crash01.log` and writes the new `crash01.log`; copies `crash01`–`05`, `import.log`, `upgrade.log`, `system.log`, `battery.log` to `systeminfo/` | `bootloader.ldr` `0x8281f436` (called from `0x11a017b8`) | [code] |
| The same function prepares the disk in update mode, so `systeminfo/` is on the "OP-Z Boot" disk | strings `how_to_upgrade.txt`, `systeminfo/…` | [code] |
| Not reachable otherwise: the normal-mode file service reads only a whitelist ([03](architecture/03-content-and-delivery.md)); content mode registers no `systeminfo` folder (the OS has no such string) | | [code] |

The log lines carry a timestamp, the source file and line, and the message ("%02d.%02d.%02d %02d:%02d:%02d]"
format at `0x815c8734`). `system.log` holds the running log, including "cpu overload" lines.

**Hardware check H-10 (read only; proposed, not run):** after the next crash (or now, for the `macro` crashes of
2026-10-18/19 if they are still among the last five): switch off, hold SCREEN and switch on (update mode, kick LED
blinking white, manual p.63); the Mac mounts "OP-Z Boot". Copy the `systeminfo/` folder to the Mac, change nothing,
copy no firmware file; eject and switch off and on. Then:

```bash
python3 tools/opz_crashlog.py "/path/to/systeminfo/crash01.log"
```

It prints the exception, the faulting instruction and every address inside a slot window as engine + function +
offset (correct only for the build that is installed).

**H-10, first look (user, 2026-10-20):** the update-mode disk's `systeminfo/` has `version.txt` (firmware 1.2.45,
os 1.2.43, bootloader 1.1.35), `serial.txt`, `system.log` (950 lines), `import.log`, `upgrade.log`, an empty
`battery.log`, and **no `crash0N.log` at all**. `system.log` covers the session of 2026-10-19 from the preset push
onwards (14 `plugs[]` imports from about "21:29", the OP-Z clock is wrong) to the `macro` removal ("22:02:34 remove
user plug 61010") and later. Every session in it ends with the keyboard chip's power-off request ("PSOC requested
SER_MSG_POWER_MODE_REQUEST_POWER_OFF"), except one: the log stops at "21:56:00" (USB power attached) and the OP-Z
boots again at "21:57:18". That is most likely the "reed" crash [inferred from the order in docs/STATUS.md]. It left
no exception line and no crash log, so **it did not go through TE's exception handler as read in §1** [proven: no
file; inferred: which event it was]. Remaining explanations: a hang that something outside the OS resets (the
keyboard or Bluetooth chip; the bootloader has "blackfin reset was requested", Q-22), an exception whose log the
bootloader did not save (the §2 reading is wrong), or a reset of another kind. So "crash and restart = exception"
(the short version, point 1) holds only for what the code shows; this crash may be a hang + external reset. Never type into the update-mode serial console
([SAFETY.md](../SAFETY.md) rule 1).

**H-10, second look (2026-11-01): `fold` froze the unit on a never-used pattern.** Project 9, pattern 4 (restored to
its never-used state after H-C1: chord sound P1 0, P2 0, LFO depth 199, speed 185, target 200, shape 128, plug 31):
the user selected `fold` (61012 v1) on chord with black key 8 and played; the OP-Z **froze** (no restart) until it
was power-cycled [proven, user]. After the restart, with P1/P2 nudged, fold plays there without trouble [proven, user],
and its live sound was not fold's preset 0 (P1 13, P2 20, attack 1, decay 20, sustain 255, release 60, LFO off), unlike
pattern 1 where selecting fold applied "fold pad" [proven]. The update-mode disk (`backups/20261101T185857Z-systeminfo`)
has **no `crash01.log`** — no CPU exception [proven] — and `system.log` has no line between the last StateSync
connection (22:39) and the next boot (22:44:19), then a second boot 24 s later (22:44:43): a hang leaves no trace
[proven for this case]. Fold passes `opz_crashcheck --full`; the cause is open: candidates are the never-used
pattern's sound bytes reaching fold through the preset switch (e.g. the LFO on P1/P2 at speed 185) or something in the
OS's handling of a never-used pattern (Q-50) [inferred]. Also seen right after, on that pattern: the Q-44 bank read
got "bank part malformed" twice and gave up (read-only, nothing written) [proven].

## 3. The context an engine runs in

| Entry point | Called from | Context, stack |
|---|---|---|
| `initModule` | engine load at boot: Synth init `0x80112ca0` → `0x801001c4` → `0x800fff1c` → dlm `0x8002b02c`, for the engines in the slot map, **before** the content-mode check (`main_task` `0x8002d23c`) | `main_task` (8 KB stack at `0x080020ac`, `0x800` words, priority 13) [code] |
| instrument `init` | `Voice::noteOn` `0x8012d278` when the voice's plug changes, after `0x8012e010` clears the voice (`Voice::init`, 0x118 bytes) | the task that plays the note [code] |
| instrument `onNoteOn` | `Voice::start` `0x8012e0a4`: writes the envelope (level 0, coefficient 1, **stage 1 at voice+0x40**, new-note flag), then `CALL (P4)` with `P4 = [voice+8]`, `R0` note, `R1` velocity, `R2` voice+0x48, params on the stack (`0x8012e20a`–`0x8012e280`) | `sequencer_task` (`0x80112840` → `0x80110550` → `0x80045880` → `Synth::noteOn` `0x80117f4c`) or `main_task` for live keys (`0x80107e20`) [code, call graph]. The only CLI found on this path guards a 64-bit `CYCLES` read (`0x80110bd2`) |
| FX `init` | `FxManager::noteOn` with `IMASK` = 0 ([engine-abi.md §3](../sdk/docs/engine-abi.md#init--when-a-voice-or-fx-slot-switches-to-this-plug)) | interrupts masked [code] |
| `render`, `renderFX` | SPORT0 DMA interrupt (`0x80030fb8`, reads `DMA0_STAT`; IVG11 through the SEC) calls the audio callback at `[0x11b004c8]` = `0x8002325c` (installed by `0x800310be` from `main_task` at `0x8002d42a`, **after** the content-mode check), which calls the Synth render `0x801184c0` (a ~360-byte frame) → voices → `Voice::render` `0x8012e2c0` → the engine. A second caller, `0x80121394` in `sequencer_task`, renders to a file ("done writing %llu bytes") | interrupt context, interrupt stack [code] |

**Interrupt entry** (`0x11a02766`, the dispatcher the OS registers while uC/OS-III runs): increments the nesting
count (`0x81aeeb00`), saves R7:4, P5:3, I0–3, B0–3, L0–3, M0–3, A0, A1, LC0/1, LT0/1, LB0/1, **sets LC0 = LC1 = 0 and
L0–L3 = 0**, and at the first nesting level saves the task's SP and switches to the interrupt stack: top
`0x81aec9e0` (`[0x815c99a8]`), base `0x81aea1e0` (`[0x815c99ac]`), **10 KB in DDR**, no guard. It pushes RETI, so
higher-priority interrupts nest on the same stack [code]. The OS's frames down to `render` take roughly 0.5–1 KB
[code, partial: the Synth render frame alone is ~360 bytes].

**Preemption.** Notes come from tasks; `render` runs in the interrupt, which outranks every task. Because the voice
is already in stage 1 when `onNoteOn` starts, the next audio interrupt can render that voice while `onNoteOn` is
half done [code for the order; inferred that it happens: the window is a few hundred cycles of every note-on, and
we found no lock around it]. A re-triggered or stolen voice (same plug) still holds the previous note's state at
that point.

**Registers.** OS code is built with ADI's compiler; its ABI, like GCC's, keeps R4–R7, P3–P5, FP and SP across
calls and assumes L0–L3 = 0 at every call and return [notes: ADI Blackfin C ABI; code: the interrupt entry clears
them before any C code]. LC/LT/LB are scratch; a non-zero LC only matters if that loop's last instruction runs
again without its `LSETUP`.

**Memory.** The DCPLB table (`0x815c9778`, `(address, data)` pairs ending with `0xffffffff`) covers L2
`0x08000000` (3 × 256 KB, 4 × 64 KB, the last uncached), `0x04000000` (2 × 256 KB), DDR `0x80000000` and
`0x84000000` (64 MB each), `0x40000000`/`0x44000000` (64 MB each), `0x70000000` and `0x38000000` (4 KB each); data
word `0x9d` (+ `0x1000` cacheable): valid, user read/write, supervisor write [code; field meanings: notes, Blackfin
CPLB format]. **The NULL page `0x00000000`–`0x03ffffff` is not covered**, so a NULL access is a data CPLB miss →
restart. L1 and the MMRs are not CPLB-checked: a wild write there is not caught and can reprogram a peripheral
[notes: Blackfin CPLBs do not cover L1/MMR space]. Engines run in supervisor mode, so the user-mode bits do not
protect anything.

**Instruments' scratch buffer** is one global, `0x8156f2b8`, passed to every voice's `init` (`0x8012d278`) [code]:
shared by all voices and tracks.

**Blackfin+ and our code.** We build for the classic ISA (`-mcpu=bf592-none`). Parallel P-register loads with
post-increment (`P5 = [P2++]` in a bundle, which the macro-crash session suspected) are legal: TE's own BF70x code has 22 of them
(e.g. `0x8000c5b8`, `0x801351ac`) and 1,598 parallel P-register loads in all [code].

## 4. Offline checks

`tools/opz_crashcheck.py engines/NAME` (or `--all`; `--full` for the complete benchmark cases) builds the device
image and runs two kinds of check. Exit status 1 if any engine has an error.

**Static** (on `bfin-elf-objdump` of the device ELF): instructions an engine must never run (`EXCPT` — a software
exception, so a restart; `RAISE`, `CLI`/`STI`, `IDLE`, `RTI`/`RTX`/`RTN`/`RTE`, writes to `SYSCFG`/`SEQSTAT`/`RETx`,
cache flushes, anything objdump cannot decode); writes to L0–L3; calls to libgcc's division helpers (listed per
function, review the divisors); the worst-case stack per entry point from the call graph (LINK size, pushes, `SP`
adjustments; recursion and indirect calls named). Limits: warning above 1 KB per call, error above 2 KB [inferred
margin against the 10 KB interrupt stack].

**Runtime**, in the GNU Blackfin simulator through `tools/opz_cycles.py` (`tools/cycles/checks.S`): every engine call
runs on a painted 8 KB stack of its own per entry point, with L0–L3/LC0/LC1 = 0 and marker values in the
callee-saved registers. After each call: L0–L3 and LC0/LC1 (L ≠ 0 is an error, LC ≠ 0 a note), R4–R7, P3–P5, FP and
SP, guards after the **280-byte** voice state / 200-byte FX state, around the output buffer, after the envelope and
the FX buffer, and that params and the FX input are unchanged. After each case: the engine's export table, code
and constants, the slot window past its `.bss`, the scratch buffer's high-water mark (note above 256 bytes). The
**NULL page is unmapped** (`--memory-region 0x4,0x4`; without a region at address 4 the simulator maps 128 MB at 0
and NULL reads and writes pass silently, as they did in every run before this one); the simulator itself stops on
misaligned accesses (EXCAUSE 0x24), unmapped addresses, undefined instructions and `EXCPT`, and the tool names the
case and the function (`tools/opz_cycles.py describe_crash`). `opz_crashcheck` adds:

- **renders on half-written states** (`BENCH_TORN`): after each case's first note-on and a re-trigger at its end,
  `render` runs on mixes of the state before and after `onNoteOn` (the changed words in address order as a prefix
  or a suffix, all but one, only one), each on its own guarded copy;
- **every preset** in the engine's `presets.json`, held and then with the knobs moving;
- **P1/P2 words outside the knob range** (`0x80000000`, `0xffffffff`), which LFO modulation might produce (Q-23).

Plain `opz_cycles.py` runs keep the register, guard, stack and NULL-page checks (printed after each engine). Cycle
numbers: the trampoline's cost is calibrated out, and the argument set-up of a direct call is no longer counted:
10–14 cycles less per render call than before (≤ 0.3 cycles/sample), up to 19 per note-on, 3–5 per `initModule`
(two `fold` cases go from 150.2 to 150.0 cycles/sample, "tight" → "ok").

What the checks cannot see: out-of-bounds **reads** that land in mapped memory (the slot region and the driver's
memory are mapped; on the device most of DDR is too), the real interleaving of interrupt and task (the torn
renders approximate it), the OS's real parameter values under LFO and portamento, Blackfin+-only behaviour, caches
and timing, the OS's own frames on the interrupt stack.

Tests: `tools/tests/test_opz_crashcheck.py` (listing parser, driver output, crash messages, the crash-log decoder,
and eleven engines with one planted fault each: state byte 280, L1 left set, LC1 left set, R5 clobbered, params
written, output overrun, a write past `.bss`, a misaligned read, a NULL read, an `EXCPT`, a pointer that is
misaligned only while `onNoteOn` is half done; and a clean one).

### Results on main (`729175d`), all 20 engines, `--full`

No errors and no warnings. Stack per call, static worst case / used in the simulator (`+cb`: plus a host callback;
the measured figure includes our callback's frames):

| Engine | `initModule` | `init` | `onNoteOn` | `onNoteOff` | `render`/`renderFX` | Divisions |
|---|---|---|---|---|---|---|
| clouds | 0/0 | 0/0 | 8/8 | 8/8 | 124/124 | — |
| dx | 0/0 | 0/0 | 84/84 | 40/40 | 348/348 +cb | — |
| echo | 0/0 | 0/0 | 8/8 | 8/0 | 292/280 | `pole_lag`, `renderFX` → `__udivsi3` |
| filter | 0/0 | 0/0 | 0/0 | 0/0 | 200/200 | — |
| fm2 | 0/0 | 0/0 | 4/4 | 0/0 | 60/72 +cb | — |
| fold | 0/0 | 0/0 | 0/0 | 0/0 | 100/112 +cb | — |
| glue | 0/0 | 0/0 | 0/0 | 0/0 | 192/192 | — |
| kit | 0/0 | 0/0 | 120/120 | 0/0 | 164/164 +cb | `onNoteOn` → `__modsi3` |
| **macro** | 0/0 | 0/0 | 52/52 | 0/0 | 148/156 +cb | `onNoteOn` → `__divsi3` |
| modal | 0/0 | 0/0 | 0/0 | 0/0 | 332/332 +cb | — |
| opz_test_passthrough | 0/0 | 0/0 | 0/0 | 0/0 | 0/0 | — |
| opz_test_sine | 0/0 | 0/0 | 0/0 | 0/0 | 116/128 +cb | — |
| plate | 0/0 | 0/0 | 0/0 | 0/0 | 216/216 | — |
| pluck | 0/0 | 4/4 | 8/8 | 0/0 | 212/212 +cb | — |
| resonator | 0/0 | 0/0 | 12/12 | 4/0 | 184/172 | `atan2_29` → `__udivsi3`; `renderFX` → `__divsi3`, `__udivsi3` |
| shimmer | 0/0 | 0/0 | 0/0 | 0/0 | 248/248 | — |
| super | 0/0 | 0/0 | 48/48 | 0/0 | 160/172 +cb | `onNoteOn` → `__divsi3` |
| swirl | 0/0 | 0/0 | 32/32 | 0/0 | 128/128 | `onNoteOn` → `__divsi3` |
| table | 8/8 | 0/0 | 0/0 | 0/0 | 212/212 +cb | — |
| vowel | 0/0 | 0/0 | 12/12 | 0/0 | 136/136 +cb | — |

One note: **`resonator`'s `onNoteOn` returns with LC1 = 3** — its "find a free string" loops (`break` in a counted
loop) became hardware loops with an early exit (`0x81bd48e4`–`0x81bd4918` in the current build). Harmless as GCC
lays the code out; listed so nobody mistakes it for the crash.

**`macro`** (not changed here): clean under every check, including all its presets ("reed" P1 220 / P2 20 among
them), knobs moving every block, the torn renders and the out-of-range P1/P2 words. Together with the stack (≤ 156
bytes), the 280-byte limit and the register checks, that excludes the simulator-visible causes: misaligned or NULL
accesses, unmapped addresses, illegal instructions and stack overflow on the paths these cases reach. What is left
needs the device: the crash log (H-10) names the cause and the instruction. Candidates, all [inferred]: a different
interleaving with the task (`onNoteOn` continuing after `render` changed the state), the OS's own parameter values
(preset application path, LFO, portamento), a Blackfin+ difference, or an out-of-bounds read that the device does
not map.

## 5. Recovery facts

| Question | Answer |
|---|---|
| Content mode before or after engines load? | After. `main_task` runs Synth init (`0x80112ca0`: `plugs.json`, slot map, `settings/plugs_patch.json` import, engine load with `initModule`) at `0x8002d23c`, then checks `/yaffs2/contentmode.plz` (`0x8002d244`, the `restart-into-content` flag) and the held key, then enters content mode (`0x8002d2e4` → `0x8003760c`) or starts audio (`0x8002d42a`) [code]. So a crash in the boot-file import or in `initModule` blocks content mode; a crash in `init`/`render` does not |
| What can content mode reach? | It formats a fresh FAT disk and registers `rejected`, `config` (`json\|dat`), `projects`, `bounces` and `samplepacks` (`wav\|aif\|aiff\|engine`, string at `0x810054c8`) (content-mode set-up `0x8003760c`: creates `projects`, `bounces`, `samplepacks`, then registers `rejected`, `config` with filter `json\|dat` at `0x81005280`, `projects` with `opz`, `bounces` through `0x800371b0`; the sample-pack folders through `0x800365c8`). **No `settings/` and no `engines/` folder** [code] |
| Can content mode import or delete engines (Q-10)? | Not ours. The only `.engine` names it builds are `"%s%d.engine"` / `"%d.engine"` (`0x800360a8`, prefix `"~"` from `0x81004ffc`) for numbered files inside the sample-pack logic (`0x80036298` looks a number ≤ 200 up in a list), next to the `samplepacks` slot folders [code]. What those files are (sample-pack containers, most likely) is open; the synth/FX engines in `/yaffs2/user/engines/` and the slot map are out of its reach |
| Is update mode reachable when the OS crashes at every start? | Yes: the bootloader runs first. After a crash reboot (reason 10) it saves the log and restarts once more with reason 0x14 without checking the keys; on that start (and any normal start) it checks them, so keep SCREEN held across the restarts [code, `bootloader.ldr` `0x11a00fe2`; decompiled, control flow partly unclear] |
| What does a firmware re-install change? | The upgrade copies every file of the package's `system/content/` to `/yaffs2/system/content/<path>` **and to `/yaffs2/user/<path>`**, overwriting (`bootloader.ldr` `0x828240e0`, `upgrade.cpp`, "failed to copy file %s"). TE's 1.2.45 package holds the 13 TE engines, `settings/plugs_patch.json` (TE's `auto_import` list), `settings/plugPresets.dat`, readmes and a sample pack. So a re-install **replaces a bad `settings/plugs_patch.json`** and TE's engines, and leaves our engine files, `plugs.json` and the slot map alone [code; that it deletes nothing else: inferred, only the copy callback was read]. TE's `auto_import` then re-imports TE's factory plugs at the next start [inferred] |
| Factory reset? | Update mode + SCREEN + STOP (manual p.63): copies every file of `/yaffs2/system/content/` to `/yaffs2/user/` ("[factory reset]", `bootloader.ldr` `0x828247d0`) and removes all custom content [manual + code] |
| Is a boot-file crash repeated? | Yes: boot init imports `settings/plugs_patch.json` and deletes it only after the import returns (`0x8010086c` → `0x80100894`; sibling branch `claude/determined-mirzakhani-2f2d48`, presets.md "The boot file") [code] |
