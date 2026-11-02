# CPU headroom: ways to give our synths more cycles

Research of 2026-10-27, offline only (no command was sent to the OP-Z). It builds on [cpu-budget.md](cpu-budget.md)
(the audio block, the overload rule, the OS's share) and asks: **where could a bigger per-voice budget come from**,
with and without changing TE's firmware? Sources: `opz.ldr` 1.2.45 in Ghidra (Blackfin+ extension) and raw scans of its
load blocks, our engines in the GNU Blackfin simulator (`opz_cycles.py --profile`). Evidence labels as in
[CONTRIBUTING.md](../CONTRIBUTING.md); addresses are device addresses in `opz.ldr`. No TE code is quoted.

## The short version

| Lever | Needs firmware? | Gain at 19 voices (today 167 cycles/sample/voice) | Cost | Verdict |
|---|:-:|---|---|---|
| Treat the budget as a **shared pool**, not per voice | no | a synth used on one track can spend 2–5× | none; a convention | **do now** |
| **Fewer sounding voices**: mono note style, or an engine-side voice cap | no | V = 12 → 268, V = 8 → 405 | polyphony | **do now** for heavy engines |
| Plug flag **0x04** (skip the OS track filter) | no | +16 (184) if all 8 tracks set it | that track's filter knob | per engine |
| **Blackfin+ 32 × 32 multiply** in our inner loops | no (SDK) | engine cost −7 … −35 % (measured instruction mix) | hand-encoded instructions; the simulator must learn them | **best engine-side project** |
| Tables and buffers (data) in **free L2 SRAM** | no (SDK) | unknown until measured on the device | probe H-L2 first | worth a probe |
| Heavy synthesis on an **FX track** (note-reactive FX) | no | its own budget, outside the timed stage | uses up FX1/FX2; no overload protection | niche |
| **Raise the overload line** (two numbers) | **yes**, 16-byte data patch | 300k → 196, 340k → 229 | less room for FX and tasks; stock units still kill | **best firmware lever**, after H-C1 |
| **Smaller voice table** | **yes**, data patch | same as the voice row, but guaranteed | polyphony for every engine, TE's too | only together with the line |
| Remove tape, input track, master chorus | yes, code patch | pays for ~+25 on the line | features the user hears | weak |
| Rewrite the OS's per-track mix, per-voice pitch code | yes, code patch | ~+12 | large, risky code patches | no |
| Overclock, lower sample rate, bigger blocks | yes | — | out of spec / breaks tuning everywhere | no |

The two big numbers to remember: the timed instrument stage may use **265,000** of the **580,499** cycles in a block,
and the OS takes **~61,500** of those 265,000 itself [cpu-budget.md, code, estimated floor]. Everything below moves one
of those three numbers or makes our own code cheaper.

## 1. What the "budget" really is

The 167 cycles/sample/voice figure is a **design target for the worst case** (all 19 voices of all 8 instrument tracks
sounding at once), not a wall [cpu-budget.md §4]. Three facts make it softer than it looks:

1. **It is one pool.** The OS times the whole instrument stage, all tracks together, against 265,000 cycles
   [code, `0x80118670`–`0x80118e38`]. Pool = 265,000 − OS. It is shared by whichever voices are actually sounding.
   With one track of a heavy synth and everything else silent, that synth may spend nearly the whole pool.
2. **The line only shortens release tails.** Over the line, the OS fades the quietest voice that is *already in
   release or drum decay* [code, `handleOverload` `0x8012d142`]. Held notes, attacks and FX are never cut. So going
   over the line costs the user tails, not a crash.
3. **The real wall is the block** (580,499 cycles). Past it, audio glitches and tasks starve [cpu-budget.md §2,
   inferred]. The line keeps the instrument stage under half of that so FX, tape, master and the UI fit.

**Budget formula** (from cpu-budget.md §4; T active instrument tracks, V sounding voices, L the line):

```text
OS          = 1,330 + 770 + 6,600·T + 350·V          (4,200 per track instead of 6,600 with flag 0x04)
per voice   = (L − OS) / V / 64   cycles per sample
```

| Case (T = 8) | V | OS | per voice | vs today |
|---|---:|---:|---:|---:|
| Today, full layout | 19 | 61,550 | **167** | — |
| Synth tracks 2 voices each (lead 3 → 2, chord 4 → 2) | 16 | 60,500 | 200 | +19 % |
| Drums 1 voice each, synth tracks 2 each | 12 | 59,100 | 268 | +60 % |
| Every track mono | 8 | 57,700 | 405 | +142 % |
| Full layout, flag 0x04 on every track | 19 | 41,580 | 184 | +10 % |
| Line 340,000, full layout | 19 | 61,550 | 229 | +37 % |
| Line 340,000 + flag 0x04 + 12 voices | 12 | 39,130 | **392** | **2.3×** |

[inferred: arithmetic on the cpu-budget.md constants, which are an estimated floor; the device measurement H-C1 may
move every row by ~10 %]

## 2. Without touching the firmware

### 2.1 Spend the pool deliberately [idea; tool built]

Engines today aim at 150 cycles/sample/voice so that *any* layout fits. A flagship synth can instead declare a
**CPU class** in its README and `plug.json` notes, for example "heavy: use on at most two synth tracks, others
light". `opz_cycles.py --layout` sums a whole set of tracks against the pool instead of judging each engine against the
per-voice figure (built 2026-10-27; example in §6). This needs no firmware and no ABI change.

### 2.2 Fewer voices [code for the OS side; simulator for the engine side]

- **Note style mono** (`params[17]`, a per-track setting the user already has) makes the OS use one voice record on
  that track [code, `0x8012d278`; host-tables-flags.md]. Fewer records sounding = more pool for the rest.
- **An engine-side voice cap: [`sdk/include/opz_voices.h`](../sdk/include/opz_voices.h)** (built 2026-10-27). One
  static table per engine, shared by every track that uses it; at note-on past the cap the oldest admitted voice is
  handed the host's own fade (stage 5, as `Voice::killNote` does), so it is gone in ~10 ms without a click and the
  host frees it. With cap N the engine may spend about 19/N times the per-voice target. Proven in opzsim (host check
  `voicecap_probe`: cap 2, three evictions, no jump, every voice freed) [proven, simulator]; that an engine may start
  the host fade itself is [inferred] from the OS doing the same and needs a listen on the device.
- Note the other direction exists too: `generous_chords` in `general.json` raises chord to 6 voices
  [code; cpu-budget.md §3]. Our budget tables assume it is off.

### 2.3 Plug flag 0x04 [code]

A plug with flag `0x04` skips the OS track filter on its track (`0x80118986`): ~2,400 cycles per block per track,
plus ~770 on the bass track [cpu-budget.md §1]. The The cost is the track's filter/resonance knob, so it suits drum
engines and synths with their own filter. **Survey 2026-10-27:** none of our released instruments reads the filter
or resonance knobs (only `opz_test_macro_unbound`, simulator only, which sets 0x04), so turning the flag on for any of
them would silently take the filter knob away from the user. Left as a per-engine sound decision; no flag changed.

### 2.4 Cheaper engines: the Blackfin+ multiply [measured in the simulator; encoding from code]

Our engines are built for the classic Blackfin ISA (`-mcpu=bf592-none`, [tools/opz_build.py](../tools/opz_build.py)).
There, a 32 × 32 fractional multiply (`opz_mul_q31`, `__builtin_bfin_mult_fr1x32x32`) is a sequence of three 16 × 16
multiplies plus accumulator shifts and an add. TE's code on the BF70x uses the Blackfin+ single 32 × 32 multiply
(`Mult32` in the Ghidra listings) [code; cpu-budget.md §8].

Instruction mix of our render paths (one voice, two blocks, `opz_cycles.py --profile`, `initModule` excluded;
"multiply" = an instruction with a `*`, "accumulator" = other `A0`/`A1` operations):

| Engine | Render instructions | multiply | accumulator | Upper bound of the saving |
|---|---:|---:|---:|---:|
| `macro` | 14,894 | 23 % | 23 % | ~35 % |
| `super` | 13,853 | 17 % | 21 % | ~29 % |
| `fm2` | 13,241 | 14 % | 11 % | ~19 % |
| `dx` | 21,323 | 13 % | 8 % | ~16 % |
| `sync` | 4,111 | 10 % | 11 % | ~16 % |
| `modal` | 29,638 | 13 % | 2 % | ~11 % |
| `table` | 11,332 | 12 % | 2 % | ~10 % |
| `pd` | 8,209 | 9 % | 2 % | ~8 % |
| `swarm` | 10,807 | 9 % | 0 % | ~7 % |

[measured, simulator; the saving column is inferred: about three of every four multiply-family instructions go away
when a sequence becomes one instruction, and some of these are already single 16 × 16 multiplies, so it is an upper
bound.] So the Blackfin+ multiply is worth **7–35 %** per engine, most for FM and multi-oscillator engines: real,
but not the 2–3× that the "six instructions for one" note suggested.

What it takes: the encoding, an SDK macro that emits it as raw instruction words (GNU `as` does not know
Blackfin+), and **the same instruction added to our `bfin-elf-run`**, or the cycle tool, the crash check and the
golden renders stop working for those engines (**Q-47**).

**Done 2026-10-27:** TE's code uses one form only for fractional products, `Rd = Ra * Rb (T)` = halfwords `0xC231`,
`0x2000 | d << 6 | a << 3 | b` (136 times in `opz.ldr`, e.g. the track filter at `0x11a00e52` and the output meter
at `0x80119d74`) [code]; the plain form (`0xC211`) never appears. **(T) truncates** [inferred, strong]: the desktop
app's arm64 build of the same `trackFilterProcess` computes these products as `(long)a·b >> 31` with −1·−1
special-cased, no rounding constant (app `0x51e3c`), so the ADI compiler's Mult32 (T) on the device must give the same
floor(a·b / 2³¹) [the Ghidra Blackfin+ spec names the two rounding modes the other way round; its naming is wrong
here]. `opz_engine.h` emits it for `opz_mul_q31` when an engine is built with `OPZ_BLACKFIN_PLUS`.

**Simulator and A/B (2026-10-27):** a second `bfin-elf-run` with this instruction (`~/opt/bfin-elf-bfp`,
[toolchain-setup.md](toolchain-setup.md#second-build-the-simulator-with-the-blackfin-multiply-q-47)); the classic one
stops with "illegal instruction" on it. `opz_build.py`, `opz_cycles.py` and `opz_crashcheck.py` take
`--blackfin-plus`. Results [measured, simulator]:

| Engine | Cases | Cycles saved per voice | Worst block | Output |
|---|---:|---|---|---|
| `macro` | 13 (full) | **25–32 %, mean 29 %** | 14,092 → 9,860 (220 → 154 cycles/sample: from over the line to under it) | bit-identical in every case |
| `super` | 12 (quick) | 7–10 %, mean 7.5 % (its multiplies are mostly 16 × 16 already) | 12,606 → 11,449 | bit-identical in every case |
| SDK check (`sdk_check.c`) | every `opz_mul_q31` test value | — | — | same bits as the host |

`opz_crashcheck.py engines/macro --full --blackfin-plus`: 0 errors. So the gain is real where the instruction mix said
it would be, the sound does not change, and nothing else in an engine has to change. **Left:** a device test (one
engine built with the flag, installed with the user's approval, listened to and crash-checked on the OP-Z), then a
decision whether `OPZ_BLACKFIN_PLUS` becomes the default for every engine.

### 2.5 Free on-chip memory: L1 is full, L2 is mostly empty [code]

- **Caches** [code]: at boot the OS calls the runtime's CPLB set-up (`0x11a02114`, called from `0x11a00140`) with the
  control word `0x3b`. Read against the set-up code itself (`0x11a02282`), that is: instruction cache on, CPLBs on,
  data cache on **bank A only**. So the core has a 16 KB instruction cache and a 16 KB data cache [notes: BF70x L1
  cache sizes]; data bank B is plain SRAM, used for the OS's mix buses (`0x119012xx`–`0x119020xx`).
- **L1 is full** [code]: the OS loads 48 KB of code at `0x11a00000` (to the last byte, `0x11a0bfff`), uses data bank A
  up to `0x11803ffc` (stack + data) and bank B up to `0x11907ff8`, and 8 KB at `0x11b00000`. There is no L1 for us
  without moving TE's code out.
- **L2 is mostly unused** [code, negative]: L2 is 1 MB (`0x08000000`–`0x080FFFFF`: the DCPLB table covers exactly that,
  the last 64 KB uncached, and the OS's address check `0x8002834a` accepts L2 up to `0x08100000`). The OS loads code and
  data only up to `0x08040000`; the highest address any instruction loads as a constant is `0x0803fff8`, and the C heap
  is a single DDR region (memory-map.md). **No constant reference to `0x08040000`–`0x080FFFFF` (768 KB) was found.**
  The last 64 KB (`0x080F0000`, uncached) looks reserved for DMA; avoid it.
- **Why it matters:** engine code and data sit in DDR and reach the core through those 16 KB caches, which the OS's
  1.3 MB of DDR code shares every block. A cache miss to L2 is far cheaper than one to DDR [notes: BF70x memory
  hierarchy; not measured]. **Data only:** L2 is write-back cached (DCPLB data `0x109d`), so code copied there with
  data writes would need a data-cache flush before it could run, and engines may not flush caches (engine-abi.md
  rule 15). Tables, wavetables and small delay lines can live there; code stays in DDR. The gain is **unknown** until
  measured, and a negative scan is not proof that nothing uses L2 at run time (DMA set up through pointers, the
  bootloader's leftovers): **Q-45**. Hardware check **H-L2** is written: `engines/opz_test_l2` scans the region
  read only, then (armed) writes and checks a canary, and times strided reads from L2 against DDR.

### 2.6 Synthesis on an FX track [inferred]

FX tracks are rendered outside the timed stage, are never killed, receive notes and have no voice limit
[cpu-budget.md §2, engine-abi.md §3]. A "synth as an effect" on FX1 or FX2 (ignore the send input, play the FX track's
notes) therefore has its own budget: whatever is left of the block after the instrument stage, the other FX, tape,
master and the tasks. That is a lot (cpu-budget.md §5 leaves ~190,000 cycles at the line), but there is **no
safety net**: an overloaded FX starves the UI and sequencer instead of fading a tail. Worth it only for a showcase
engine, with a hard self-limit on its own voices.

## 3. With a firmware change (P8: only after recovery is proven on a spare unit)

All of these are bound by firmware/CLAUDE.md: never on the user's main OP-Z, patches not
images, a revert path first. They share one drawback: **an engine that needs the patch misbehaves on stock
firmware** (more release tails cut). Our engines should keep fitting the stock budget; a patch only buys headroom.

### 3.1 Raise the overload line — the best firmware lever [code]

The two thresholds are initialised 64-bit data, not code: **290,000 at `0x8110f5fc`** and **265,000 at
`0x8110f604`**, read once per block at `0x80118e0a` (verified in the load image). Changing them is a 16-byte data patch
with no code change. How far is safe depends on everything outside the timed stage:

| Part of the block | Cycles |
|---|---:|
| Interrupt, callback, bus clearing | ~4,500 |
| Input, tape, FX1, FX2, master (OS part) | ~28,000 |
| Tape plug + master `chorus` | ~17,500 |
| FX1 + FX2 plugs at the 600 cycles/frame target | 76,800 |
| **Sum outside the line** | **~127,000** |

[cpu-budget.md §5, estimated.] With the line at 340,000 the block is at ~467,000 (80 %), leaving ~114,000 for the
tasks (UI, sequencer, USB, BLE) and other interrupts; at 380,000 only ~73,000. How much the tasks need is **not
known** (Q-46). A first candidate is **265,000 → 330,000 and 290,000 → 355,000** (+32 % pool, ~221 cycles/sample/voice
at 19 voices), but only after H-C1 has measured the OS's share and the tasks' idle time on the device.

**Patch record (draft, 2026-10-27):** firmware/patches/overload-line-330k.json,
applied offline by `tools/opz_fwpatch.py`, which checks the source's size, SHA-256 and CRC-32, maps each file offset
through the LDR blocks to the stated device address and refuses any mismatch. The two words sit in a plain data block
(file offset `0xc1054`, no per-block CRC) and nothing else loads their address: `0x80118e0a` sets the base, the
three-block test reads it as `[P1 + 8]` at `0x80119bdc` [code]. `opz.crc32` beside the image is simply the decimal
CRC-32 of `opz.ldr` [proven: recomputed], and the patcher writes a new one. Still open before any install: packaging
(the `.zfw` is encrypted) and whether the bootloader checks `os.ldr` against a stored CRC (it runs a hardware CRC over
`/yaffs2/os.ldr`; where the expected value comes from is not read yet, **Q-48**).

### 3.2 A smaller voice table [code]

The per-track voice counts are initialised data at `0x815C7ACC` (default) and `0x815C7AEC` (`generous_chords`), eight
32-bit words each, read only by `getVoiceEndIndex` (`0x8012d1e0`/`0x8012d1e8`); the first-record table at `0x815C7B0C`
is separate, so lowering a count leaves records unused and nothing moves [code; that nothing else depends on the
counts is inferred from the single reference]. It guarantees the worst case for every engine, TE's included, at the
cost of polyphony the user notices. On its own it adds nothing that mono note style and engine caps (2.2) can't do;
it is useful only to make a raised line safe.

### 3.3 Remove OS features to pay for a higher line [code for the costs; idea]

Outside the line: the input track (~5,000–10,000; already skipped when byte `0x81059d50` is set, `0x80118e70`), the
tape track (~4,000 + the tape plug, assumed ~10,000) and the master `chorus` plug (~7,500). Dropping all three frees
~25,000–30,000, enough to move the line by the same amount (~+20 cycles/sample/voice). Each is a feature the user hears
(tape tricks, mic/line input, master chorus). Weak trade; the input byte's meaning is worth reading first.

### 3.4 Rewrite OS code inside the timed stage [idea]

The OS's own share (~61,500) is mostly the per-track mix (~2,950), track filter (~2,400) and DC blocker (~670) per track,
and ~350 per voice (two ROM divisions for the cents-to-pitch path) [cpu-budget.md §1]. Tighter code might save
~15,000 cycles (~+12 cycles/sample/voice). Code patches in TE's audio interrupt are the riskiest kind there is; not
worth it for this gain.

### 3.5 Not possible or not sensible

- **More clock:** 400 MHz (Q-04) is the BF70x's rated maximum [notes: ADI datasheet]. Overclocking is out of spec,
  heats the part, and the line is in cycles anyway.
- **Lower sample rate or larger blocks:** tuning words, the codec, USB audio and every engine assume 44.1 kHz and
  64 frames; the OS's cost is mostly per sample, so larger blocks save little.
- **A second core:** the BF70x is single-core; there is no second DSP to offload to.

## 4. Recommended order

1. **Now, no firmware:** pool thinking (2.1), engine voice caps for heavy synths (2.2), flag 0x04 where an engine owns
   its filter (2.3). A flagship synth meant for one or two tracks can be 2–3× heavier than today's target.
2. **Measure:** hardware check H-C1 ([cpu-budget.md §7](cpu-budget.md#7-hardware-check-h-c1-run-2026-11-01-a-b-c)),
   extended with a task-headroom reading (Q-46). Every firmware number above depends on it.
3. **SDK:** the Blackfin+ multiply (Q-47, 7–35 % per engine), then an L2 probe (Q-45).
4. **Firmware (P8, spare unit, recovery proven):** raise the line by a measured amount (3.1); add the voice table
   (3.2) or feature removal (3.3) only if the measured task headroom asks for it.

## 5. Open questions raised here

- **Q-45:** is L2 SRAM `0x08040000`–`0x080EFFFF` really unused at run time, and how much does running an engine's hot
  code and tables from it save over DDR?
- **Q-46:** how much of the block do the tasks (UI, sequencer, USB, BLE) need, i.e. how far can the line be raised?
  uC/OS-III's idle task (`0x801329f8`) loops: interrupts off, `OSIdleTaskCtr` (`0x81AEEAFC`) + 1, interrupts on,
  then a hook pointer (`0x81AEEAEC`, called when set) [code]. `engines/opz_test_load` reads that counter once per block
  (P2 zone 4), so H-C1's run C turns loops per block into free cycles. No statistics task string was found.
- **Q-48:** does the bootloader compare its hardware CRC of `/yaffs2/os.ldr` with a stored value (a copy of
  `opz.crc32`?) and refuse a patched OS? Read the bootloader's boot path before any patched image is installed.
- **Q-47:** can our toolchain emit the Blackfin+ 32 × 32 multiply, and can `bfin-elf-run` be taught it? **Yes,
  offline (2026-10-27)**: see §2.4; only the device test is left.

## 6. Built so far (2026-10-27, offline)

| Step from §4 | What exists | Where | Still needed |
|---|---|---|---|
| 1. Pool | `opz_cycles.py --layout "bass=table chord=modal*2 …" [--line N] [--report JSON]`: a whole layout against the shared pool, both lines, flag 0x04 and silent tracks counted | `tools/opz_cycles.py` | — |
| 1. Voice cap | `opz_voices.h`, host check `voicecap_probe` | `sdk/include/` | a heavy engine that uses it; a listen on the device |
| 1. Flag 0x04 | survey: no released instrument reads the filter knobs | §2.3 | a sound decision per engine |
| 2. Measure | `opz_test_load` (slot 67, id 60907): stage span, OS work between tracks and voices, own cost, idle loops, calls, block period, kills; decoder `tools/opz_loadtest.py` | `engines/opz_test_load/` | **H-C1 on the device** (user approval per write) |
| 3. Blackfin+ multiply | encoding from TE's code, opt-in `OPZ_BLACKFIN_PLUS`, a second simulator that runs it, `--blackfin-plus` in the build, cycle and crash tools; A/B: `macro` −29 %, `super` −7.5 %, bit-identical | `sdk/include/opz_engine.h`, toolchain-setup.md | a device test of one engine; then make it the default |
| 3. L2 | `opz_test_l2` (slot 65, id 60908), decoder table `--table l2`; the cycle driver maps L2 and the sample cache | `engines/opz_test_l2/` | **H-L2 on the device** |
| 4. Firmware | patch record format, `tools/opz_fwpatch.py`, draft record for the line | `firmware/patches/` | P8 step 1 (spare unit, recovery), packaging, Q-48, a value from H-C1 |

**Example** (`opz_cycles.py --quick --layout`, simulator, each engine at its dearest case, so pessimistic): bass
`macro`, lead `super`, arp `fm2`, chord `table`, kick and snare `kit`, perc `hats`, sample `snap`, every voice sounding:
269,290 cycles per block steady, 306,518 worst, **102 % of the line: release tails get cut**. The same with chord and
lead held to 2 voices and the kick mono: 224,741 / 251,254, **85 %, fits**. The full layout with the draft patch's
line (330,000 / 355,000): **82 %, fits**. [measured, simulator; the OS share is the estimated floor]

Found on the way: `opz_test_param` and `opz_test_sampler` were both linked for slot 72 (never installed); `opz_test_param`
moved to 69 (version 2).

