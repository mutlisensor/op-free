# Quickstart: your first OP-Z engine

From a fresh clone to a sound in the simulator and a checked `.engine` file, on one page. Everything here runs
offline on a Mac; nothing talks to an OP-Z until the last section, which sends you to [SAFETY.md](../../SAFETY.md)
first.

```text
scaffold ──► simulator (sound, golden renders) ──► device build ──► crash check ──► cycle count ──► install (--fake)
 opz.py new     make -C sim golden / test           opz_build.py     opz_crashcheck   opz_cycles     opz.py --fake
                web simulator (play it, optional)
                make -C sim wasm, sim/web
```

## 0. What you need

| For | Install |
|---|---|
| Steps 1–3 (simulator) | macOS with the Xcode command-line tools (`xcode-select --install`: `clang`, `make`) and Python 3 |
| Playing it in the browser (optional, §3) | LLVM with the wasm32 target, `wasm-ld` and a WASI sysroot (below), Node 23.6 or newer, Chrome or Edge |
| Steps 4–6 (device build, crash and cycle checks) | the Blackfin cross-compiler and simulator in `~/opt/bfin-elf` (below) |
| Step 7 with a real OP-Z | `pip install mido python-rtmidi`, and [SAFETY.md](../../SAFETY.md) |

No teenage engineering files are needed for any step: the simulator, its host tables and the tools are this
project's own.

### The Blackfin toolchain

The OP-Z runs engines on an Analog Devices Blackfin, so the device build needs `bfin-elf-gcc` (GCC 15.3, C only)
and binutils 2.47, plus GDB 18.1's `bfin-elf-run` simulator for the crash and cycle checks. They are built from the
GNU release tarballs into `~/opt/bfin-elf` in about 10 minutes, with Homebrew's `gmp`, `mpfr`, `libmpc` and `isl`.
The exact configure lines, the tarball hashes and two macOS fixes for the simulator are in
[toolchain-setup.md](../../research/toolchain-setup.md). Check the result:

```bash
~/opt/bfin-elf/bin/bfin-elf-gcc --version
```

```bash
~/opt/bfin-elf/bin/bfin-elf-run --version
```

The tools look in `~/opt/bfin-elf` by default; pass `--toolchain DIR` to use another place.

### The WebAssembly toolchain (web simulator)

Apple's `clang` has no wasm32 target. Homebrew's LLVM has, with `lld` for `wasm-ld` and the WASI C library for the
host's `printf` and file reading (engines themselves still get no libc):

```bash
brew install llvm lld wasi-libc wasi-runtimes
```

`make -C sim wasm` finds them through `brew --prefix`. Elsewhere (Linux, or a [wasi-sdk](https://github.com/WebAssembly/wasi-sdk)
download) point it at your install: `make -C sim wasm WASM_CC=/opt/wasi-sdk/bin/clang
WASM_LD=/opt/wasi-sdk/bin/wasm-ld WASI_SYSROOT=/opt/wasi-sdk/share/wasi-sysroot`.

## 1. Check the simulator

```bash
make -C sim test
```

This builds `opzsim` (our model of the OP-Z's engine host, [sim/README.md](../../sim/README.md)) and every engine
under `engines/`, runs the host checks and compares every engine's golden renders. It ends with
`N passed, 0 failed`. With the toolchain installed it also builds each engine for the device.

## 2. Make an engine

```bash
python3 tools/opz.py new wobble --kind synth
```

`--kind` picks the template ([sdk/templates/](../templates/)):

| Kind | Tracks | Starts as |
|---|---|---|
| `synth` | bass, lead, arp, chord | a sine → saw oscillator with a sub-octave, on the host's ADSR envelope |
| `drum` | kick, snare, perc, sample | 24 pitched hits (sine with a pitch drop → noise), on the host's drum envelope |
| `fx` | FX1, FX2 | drive into a soft clipper and a tone control, with the SDK's dry/wet law, level and output limiter |

The command creates `engines/wobble/`:

| File | What it is |
|---|---|
| `wobble.c` | the engine, commented line by line: the place to make your sound |
| `plug.json` | engine name, memory slot, plug id, the tracks it may go on, the two knob names ([plug-format.md](plug-format.md)) |
| `presets.json` | the 18 parameter bytes of each preset |
| `tests/*.scn` | simulator scenarios: notes, knob sweeps and `expect` lines ([scenario format](../../sim/README.md#scenario-format-scn)) |
| `README.md` | the engine's one-page manual |

It also picks, and records as a row of [memory-slots.md](memory-slots.md):

- **a plug id** — the first free test id, 60900–60999. Released engines take an id from 61000 up, one block of ten
  per family, before they are published.
- **a memory slot** — each engine is linked to its own fixed address window. The scaffold takes the next free slot
  of this project's range, top down, in the 5 KB class (falling back to 10 KB when 5 KB is full). It never takes a
  slot TE's engines use, slot 0, or a slot above 89 (not proven safe yet). `--size 10` or `--size 50` asks for a
  bigger class, `--slot N` for one slot. Two engines in one slot overwrite each other on the OP-Z, so the record
  matters.

`--dry-run` shows the choice without writing anything. Names are lowercase letters, digits and `_`; TE's engine
names are refused.

## 3. Hear it, change it, test it

Render the scenarios once and keep the results as the engine's golden references:

```bash
make -C sim golden ENGINE=wobble
```

The WAVs are in `sim/build/renders/wobble/`: listen to them. The hashes go to `engines/wobble/tests/golden/`.
Now run everything, the way every change is checked:

```bash
make -C sim test
```

Edit `engines/wobble/wobble.c`, then render again with `make -C sim golden ENGINE=wobble`, listen, and run
`make -C sim test`. A golden file is a promise that the engine sounds exactly like this: re-render only when you
meant to change the sound, and commit the new hashes with the change. To try one scenario by hand with the event
log:

```bash
sim/build/opzsim -v -e sim/build/engines/wobble.dylib --slot 71 --trackmask 240 -o wobble.wav engines/wobble/tests/basic.scn
```

(use the slot and trackmask from your `plug.json`).

### Play it in the browser

The web simulator runs your engine with the same host C as `opzsim`, compiled to WebAssembly, in an AudioWorklet at
44.1 kHz with 64-frame blocks. It has a synth section and effect sections on FX1 and FX2: play the synth from the
computer keyboard (A W S E D F T G Y H U J K O L P ; ', Z/X for octaves) or a MIDI keyboard, turn up its FX1 or FX2
send to hear it through that effect, turn every section's knobs and load their presets. An effect can also run on
a saw, noise or a sweep. Each engine's scenarios run in the page with their `expect` results and golden hashes.

```bash
make -C sim wasm
```

```bash
npm --prefix sim/web ci
```

```bash
npm --prefix sim/web run dev
```

Open http://localhost:5174/?synth=engines/wobble&fx1=engines/filter&fx2=engines/plate in Chrome or Edge (`none`
leaves a section empty; an effect only goes on a track its trackmask allows) and press **Start audio**. Rebuild with
`make -C sim wasm` after each edit and reload the page. The *Host checks* panel shows the state budget, the
"adds into out" check, voices and host CPU (this browser's WebAssembly, **not Blackfin cycles**: §6 counts those).
A scenario render in the page is bit-identical to `opzsim`'s, so its hash matches your golden file; `npm --prefix
sim/web test` checks that for every engine. More in [sim/README.md](../../sim/README.md#web-simulator).

### The rules your code must keep

The simulator checks most of these and fails the test when one breaks; the full list with the evidence is
[engine-abi.md §6](engine-abi.md#6-rules-for-engine-authors).

- **Integers only.** No floating point, no libc, no heap. Audio and gains are Q31 (`fract32`, `0x7fffffff` = 1.0),
  phases `uint32_t` turns. Multiply and add audio with `opz_mul_q31` / `opz_add_q31` from `opz_engine.h` (one
  saturating Blackfin instruction each); `opz_dsp.h` has oscillators, filters, noise and exponentials.
- **No division and no 64-bit maths in the sample loop.** Work out coefficients once per 64-frame block.
- **State:** at most **280 bytes per voice** (instruments) or **200 bytes per FX track** (`OPZ_VOICE_STATE_BYTES`,
  `OPZ_FX_STATE_BYTES`), in the `State` struct; keep the template's `_Static_assert`. Globals are shared by every voice of every track.
- **Instruments add into `out`** (interleaved stereo, 128 values), never overwrite it, and let the envelope reach
  stage 0 so the voice is freed. **Effects write all 128 values** of `out`.
- **Read `pitch` every block** (synths): with the legato note style new notes don't call `onNoteOn`. On drum tracks
  `pitch` is a fixed rate; the pitch comes from the note.
- **Accept any parameter word** 0…0x7FFFFFFF and any `pitch`, in any order: derive indices with shifts or clamps.
- **Saturate, never wrap**, and keep `onNoteOn`/`onNoteOff` short and safe to interrupt.
- Peak level per voice at full velocity: **−12 dBFS** (`OPZ_VOICE_PEAK`).
- **Apply the track's level and pan yourself** (instruments): the OS doesn't, and its FX sends are taken after the
  engine. The templates do it with `opz_mix.h` (`opz_mix_start` in `onNoteOn`, `opz_mix_mono` at the end of `render`);
  keep it, or the level and pan dials do nothing ([engine-abi.md §4](engine-abi.md#level-pan-and-the-sends)).

## 4. Build for the device

```bash
python3 tools/opz_build.py engines/wobble
```

It compiles with `bfin-elf-gcc` into the slot's address window, packs the ELF into an `.engine` file and checks it
the way the OP-Z's loader does (`result PASS`). Output: `build/bfin/wobble/wobble.engine`, with `.elf`, `.map` and
`.lst` (disassembly) beside it. The `usage` line says how much of the slot is used. If the image does not fit, the
linker refuses it: make it smaller, or move to a bigger class (change `memoryslot` in `plug.json` and its row in
memory-slots.md together, or delete the engine and its row and scaffold again with `--size 10`).

## 5. Crash check

```bash
python3 tools/opz_crashcheck.py engines/wobble --full
```

Any CPU exception reboots the OP-Z. This runs the device build in the Blackfin simulator with static checks, stack
and register checks, guards around the state and the slot, an unmapped NULL page, every preset, renders on
half-written states and the OS's worst parameter and pitch streams ([crash-safety.md](../../research/crash-safety.md)).
It must end `ok, 0 error(s)`.

## 6. Cycle count

```bash
python3 tools/opz_cycles.py engines/wobble
```

Cycles per sample, per case, on the device build. The budget ([cpu-budget.md](../../research/cpu-budget.md)):
**about 167 cycles per sample per voice** for instruments with every voice busy (aim for 150 or less), about 600 per
sample for effects. Check the worst block (`max`), not only the mean. The templates as scaffolded measure about
126 (synth), 72 (drum) and 126 (fx) cycles per sample. An engine with no benchmark cases of its own gets a P1/P2
grid and moving knobs; add cases for your engine's expensive settings in `cases_for` in
[tools/opz_cycles.py](../../tools/opz_cycles.py).

## 7. Install

First offline, against an in-memory OP-Z. This runs the whole installer flow — backup, plan, push, read back —
without hardware:

```bash
python3 tools/opz.py --fake install engines/wobble --track bass --yes
```

(`--track kick` for a drum engine, `--track fx1` for an effect.) The plug goes into the track's first free sound
slot; on a full track (the fake OP-Z's lead track is one) name a slot to replace with `--slot N --replace`. Add
`--dry-run` to see the plan only.

**On a real OP-Z, read [SAFETY.md](../../SAFETY.md) first.** Its short version: close the official OP-Z app, back
up, run every command with `--dry-run` first and read the plan, and install one thing at a time. An installed
engine stays registered on the unit for good (removing the plug does not free its slot), so install only engines
that passed steps 3–6. The installer backs up the settings before every write, verifies the result and rolls the
slot map back if verification fails.

## 8. Before you share it

The definition of done is in [development-workflow.md](development-workflow.md#definition-of-done-for-an-engine):
golden tests, `opz_verify`, the crash check and the CPU cost documented in the README; presets and the manual
written; any ported DSP code MIT, BSD, ISC or public domain and credited; a released plug id instead of the test id;
then a hardware run (install, play, remove, reinstall). Bump both `version` and `engine_version` in `plug.json`
whenever the binary changes: the OP-Z re-imports a plug only when its version goes up.

## When something fails

| Message | Meaning |
|---|---|
| `no golden file; run make -C sim golden ENGINE=…` | a new scenario, or a new engine: render, listen, commit the hashes |
| `sha256 differs; spectral distance … dB` | the sound changed. Meant it? `make -C sim golden ENGINE=…`. Not? find what changed |
| opzsim exit 3, `expect … FAILED` | a scenario's `expect` line does not hold (sound, silence, idle voices, peak) |
| opzsim exit 4 | an ABI or budget rule broke: state overflow, `out` overwritten, globals per voice |
| `static assertion failed: voice state must fit` | `State` is over 280 (or 200) bytes: move big buffers to a static pool ([engine-abi.md §7](engine-abi.md#7-pattern-per-voice-buffers-larger-than-280-bytes)) |
| linker: region overflowed | the engine does not fit its slot (step 4) |
| `track lead has no free slot (use --slot N --replace)` | every sound slot of that track is taken: name one with `--slot N --replace` |
| `memory slot N is already used by …` (install) | another engine owns that slot on the unit: take another slot |
