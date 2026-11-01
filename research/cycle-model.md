# Blackfin cycle model: what our engines cost (ROADMAP 1.4)

Offline cycle counts for every engine, measured by running the **device build of each engine** in the GNU Blackfin
instruction-set simulator. No hardware involved. Measured 2026-10-18, twice: first on swirl v2, macro v2, kit v3,
`opz_test_sine` v5 and `opz_test_passthrough` v2 (**before**), then after the optimisation pass on swirl v3,
macro v3, kit v4 and `opz_test_sine` v6 (**after**; passthrough unchanged).

**Verdict after the pass: the step 11 layout fits** (before the OS's own share; with it, counted on 2026-10-23, it
sits just over the 265,000 line: see [the budget](#budget-and-headroom) and [cpu-budget.md](cpu-budget.md)). One `macro` voice now costs 7,000–13,500 cycles per block
(110–211 per sample), a `kit` hit 5,600–17,000 (88–265 per sample, hats 161), against about 13,900 per voice
(218 per sample) when all 19 instrument voices play. The 19-voice stress layout of runbook step 11 (11 `macro` +
8 `kit` hats) needs about **220,000 cycles per block**, under the OS's 265,000 line before its own share
(see [the budget](#budget-and-headroom)); before the pass it needed ~900,000. `swirl` went from 1,590 to 414
cycles per stereo frame, under its 600 target. The sound is unchanged: every benchmark case and every golden
scenario nulls against the old builds at −151 dBFS or lower (see [sound](#sound-null-tests-against-the-old-builds)).

Before the pass all three engines were 2–6× over budget, because of 64-bit C arithmetic in the inner loops
(see [where the cycles go](#where-the-cycles-go)). The fix was mechanical: 32-bit and fractional forms in
`sdk/include/opz_engine.h` / `opz_dsp.h` first, then the engines' loops
(see [optimisations](#optimisations-macro-v3-kit-v4-swirl-v3)).

## Method

[`tools/opz_cycles.py`](../tools/opz_cycles.py) and the bare-metal driver in [`tools/cycles/`](../tools/cycles/):

1. Build the engine with `tools/opz_build.py` (same GCC 15.3, same flags `-O2 -mcpu=bf592-none …`, same linker
   script and slot window). The engine is packed and checked by `opz_verify.py` as for the device.
2. Copy the engine's loadable bytes **unchanged** to its slot base (`0x81BD…`) in a driver program. The driver finds
   `initModule`, `init`, `onNoteOn`, `onNoteOff` and `render`/`renderFX` through the export table, as the OS loader
   does, and zero-fills the engine's `.bss` (the loader's FILL block).
3. Per case: `initModule` once, `init` and `onNoteOn` per voice, then N blocks of `render` per voice into one shared
   track buffer (or `renderFX` on a −12 dBFS saw-plus-noise input). The `CYCLES` register is read around every
   call (overhead of one cycle, subtracted). Voices whose envelope reaches stage 0 are skipped, as the host does.
4. The host side is our model: `updateParametersADSR` is an integer port of opzsim's (tables from
   `sim/src/callbacks.c`). It runs inside `render`, as TE's callback does, and costs ~66 cycles per call
   (0.2 % of a `macro` voice). Its rate tables don't change any engine's cost.

Run it:

```bash
python3 tools/opz_cycles.py --all
```

`--markdown` prints tables, `--json FILE` writes everything. `--profile "CASE"` traces one case (one voice) and
counts instructions per function and per source line. The `-g1` build it uses for line numbers is checked to be
byte-identical to the device build. `--run` picks another simulator binary. `--dump DIR` also saves every rendered
block of every case (the device build's output, printed by the driver outside the timed calls), and
`--compare DIR_A DIR_B` null-tests two dumps case by case (peak and RMS of the difference in dBFS). Tests:
`python3 -m unittest discover -s tools/tests` (skipped without the simulator), `make -C sim cycles` for the
full table.

### The simulator, and what it does not model

GNU sim 18.1 (`sim/bfin` from the GDB 18.1 release), built into `~/opt/bfin-elf` with two fixes needed on
macOS/arm64; see [toolchain-setup.md](toolchain-setup.md#instruction-set-simulator-bfin-elf-run). It runs the
classic Blackfin ISA, which is exactly what `-mcpu=bf592-none` emits and the BF70x runs. Its cycle count is
**one cycle per instruction** (a parallel 64-bit bundle counts once), plus fixed penalties:

| Event | Cycles in the simulator |
|---|---|
| conditional branch: taken / not taken, predicted (`(BP)`) | 5 / 9 |
| conditional branch: taken / not taken, not predicted | 9 / 1 |
| `JUMP`, `CALL`, `RTS`, `RTI` | 5 |
| `[--SP] = (R7:x, P5:y)` / pop multiple | 14 / 11 |
| `R0 *= R1` (32-bit integer multiply) | 3 |
| `LINK` / `UNLINK` | 3 / 2 |

Not modelled:

- **Pipeline stalls**: data hazards (a loaded pointer used at once, a multiply result used by the next
  instruction, register moves into DAG registers). These add cycles on real silicon.
- **Caches and memory**: the engine slots live in DDR at `0x81BD…`. A cold instruction-cache line fill from DDR
  costs tens of cycles. For one engine type a block costs at most roughly its code size in lines, ~150–300 lines for
  4–9 KB, so on the order of 10,000 cycles if the code was evicted since the last block. The code then stays cached
  for the other voices of the same engine in that block. Data (voice state, the track buffer, `swirl`'s delay lines)
  is small or sequential.
- **Blackfin+ differences**: the BF70x core has a branch target buffer, so taken branches in hot code can cost
  less than the classic penalties above. The harness also runs a second simulator build that charges one cycle per
  instruction and nothing else (`--run`, an analysis build). Its counts are a **hard floor**: no single-issue core
  executes more than one instruction per cycle.
- **The OS's own share** of the instrument window: per-track filter, DC blocker, LFOs and mixing. It comes on top of
  all numbers here and still needs the device (runbook step 11).

**Plausible margin:** device cycles ≈ simulator cycles × 0.7 … 1.3. The low end is the instruction floor (25–37 %
below the simulator for our engines before the optimisation pass, if a BTB removed every branch penalty; the
optimised loops are hardware loops with few branches, so that gap is smaller now and the low end moves up). The
high end allows for stalls and cache refills: the optimised code puts multiply results to use at once, which is
where stalls would show. The before numbers were over budget anywhere in that range; the after numbers are under it
for most of the range, which is why step 11 on the device still decides (see the budget).

## Results

Per call = one voice (instrument) or one FX track (effect) for one 64-frame block. *Mean* leaves out `swirl`'s
first 8 blocks, while it clears its delay line. *Max* is the worst single block. Note-on is one `onNoteOn` call.
The "before" columns are the first measurement (swirl v2, macro v2, kit v3); its instruction-only floor is in the
git history of this file (the analysis simulator build was not rerun after the pass).

### `macro` v3 (4 voices, notes 48/55/60/64, velocity 100, 100 blocks)

| P1 shape | P2 | Before: cycles/block/voice | per sample | **After: cycles/block/voice** | max | **per sample** | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| saw (0) | 0 | 30,618 | 478 | **7,872** | 8,384 | **123** | 131 |
| saw (0) | 128, 255 | 44,554–44,576 | 697 | **13,530** | 14,103 | **211** | 220 |
| square (128) | 0 | 27,983 | 437 | **7,049** | 7,557 | **110** | 118 |
| square (128) | 128, 255 | 39,258–39,309 | 614 | **12,528** | 13,097 | **196** | 205 |
| pulse (255) | 0 | 28,081 | 439 | **7,048** | 7,719 | **110** | 121 |
| pulse (255) | 128, 255 | 39,337–39,429 | 616 | **12,526** | 13,431 | **196** | 210 |
| pulse, P2 255, released from block 20 | 255 | 39,339 | 615 | **11,584** | 12,836 | **181** | 201 |
| pulse, P2 255, **19 voices** | 255 | 39,333 | 615 | **11,847** | 13,431 | **185** | 210 |

The 19-voice run takes **225,000 cycles per block** in total (before: 747,000). P2 > 0 adds the second oscillator
and the sub, about +5,500. The saw is dearer than square/pulse at P2 > 0 because of the crossfade (`opz_lerp`) in
both oscillators. Note-on: 408 cycles (before 631).

### `kit` v4 (one voice per key, velocity 127, P1 128, P2 255, up to 200 blocks)

| Keys | Model | Before: cycles/block | per sample | **After: cycles/block** | max | **per sample** | max |
|---|---|---:|---:|---:|---:|---:|---:|
| 1, 3 | kick deep, boom (sine + pitch sweep, click) | 42,420–42,450 | 663 | **9,119** | 15,238 | **143** | 238 |
| 20 | zap (sine + pitch sweep) | 42,419 | 663 | **9,034** | 9,034 | **141** | 141 |
| 2, 4 | kick punchy, hard (+ noise, click) | 75,960–79,950 | 1,187–1,249 | **12,757–13,225** | 16,208 | **199–207** | 253 |
| 5 | snare 808 (two sines, sweep, noise, high-pass) | 81,095 | 1,267 | **16,986** | 16,986 | **265** | 265 |
| 6 | snare tight (+ click, band-pass) | 83,226 | 1,300 | **16,254** | 19,237 | **254** | 301 |
| 7 | clap | 53,614 | 838 | **7,072** | 10,939 | **111** | 171 |
| 8 | rim (two triangles, noise) | 59,716 | 933 | **10,383** | 10,424 | **162** | 163 |
| 9–12 | closed, pedal, open hat, cymbal (6 squares + noise + filter) | 58,617–58,642 | 916 | **10,305–10,318** | 10,318 | **161** | 161 |
| 13–15, 16, 23 | toms, conga, hi-q | 79,325–81,985 | 1,240–1,281 | **12,417–12,455** | 15,438 | **194** | 241 |
| 17 | cowbell (2 squares) | 18,881 | 295 | **6,927** | 7,110 | **108** | 111 |
| 18 | clave | 26,683 | 417 | **6,465** | 6,465 | **101** | 101 |
| 19 | shaker | 51,410 | 803 | **7,390** | 7,390 | **115** | 115 |
| 21 | blip | 18,339 | 286 | **5,608** | 5,699 | **88** | 89 |
| 22 | noise | 50,846 | 794 | **6,608** | 6,609 | **103** | 103 |
| 24 | metal (two sines; no square bank) | 27,899 | 436 | **8,653** | 8,653 | **135** | 135 |
| 11, 12 × **8 voices** | open hat / cymbal | 58,618 / 58,602 | 916 | **10,318** | | **161** | |

The worst block of a kick, tom or snare is its first: the click runs for ~1 ms. P2 (decay) still changes how long a
hit lasts, not what a block costs (P2 128 and 255 agree within a cycle). Eight hat voices together take
**82,500 cycles** per block (before: 469,000). Note-on: 1,755–1,800 cycles (before 5,174–7,430); the first hit of a
filtered model at a new P1 (tune) costs ~4,200, as it computes the noise filter, which is then cached per model.

### `swirl` v3 (one FX track, 60 blocks, mean after the 8 clearing blocks)

| Mode | P1 | P2 | Before: cycles/block | per frame | **After: cycles/block** | max | **per frame** |
|---|---:|---:|---:|---:|---:|---:|---:|
| phaser | 128 | 0, 64 | 79,258–79,288 | 1,239 | **21,032–21,039** | 22,925 | **329** |
| crossfade | 128 | 128 | 101,687 | 1,589 | **26,502** | 26,567 | **414** |
| flanger | 0 / 128 / 255 | 200, 255 | 101,681–101,790 | 1,590 | **26,489–26,506** | 26,580 | **414** |
| note comb (A3 on the FX track) | 128 | 200 | 101,758 | 1,590 | **26,478** | 26,543 | **414** |

P1 (rate) doesn't change the cost. "Per frame" is per stereo frame (both channels). The phaser's max is a
clearing block. The note-on that tunes the comb costs 1,141 cycles (before 2,108); the first block after a pan change
costs two divisions more (~700 cycles), cached until pan moves again.

`filter` (plug 61002) and `resonator` (61003) came in with a later merge and are not in these tables. They use only
`opz_mul_q31`, `opz_add_q31` and `opz_sat32`, whose definitions this pass left alone: their device code is
byte-identical with the new SDK, so their costs (in their READMEs) and goldens are unchanged. `resonator`'s
4-note case (~710 per frame) is the one over the 600 effect target; the same treatment would apply there.

### Test engines

| Engine | Case | Before: cycles/block/voice | **After** | max | **per sample** |
|---|---|---:|---:|---:|---:|
| `opz_test_sine` v6 | P1 128/255 (triangle/saw), P2 0 | 11,433 | **9,195** | 10,491 | **144** |
| `opz_test_sine` v6 | P1 0 (sine), P2 0 | 17,553 | **15,315** | 18,169 | **239** |
| `opz_test_sine` v6 | P1 0, P2 255 (drive) | 24,373 | **22,135** | 25,903 | **346** |
| `opz_test_passthrough` v2 | — | 284 | **284** | 284 | **4.4** |

`opz_test_sine` was not reworked; it only picked up the cheaper SDK helpers (its output is bit-identical).
`initModule` takes 19–23 cycles for every engine.

## Budget and headroom

> **Update 2026-10-23 ([cpu-budget.md](cpu-budget.md)):** the OS's own share of the timed instrument stage is now
> counted from the device code: ~61,500 cycles per block with all 8 instrument tracks active and 19 voices (~6,600
> per track, ~350 per voice) [code, estimated floor]. The per-voice budget is therefore **~167 cycles/sample** for the
> steady state and ~188 for a single block, not 218, and `tools/opz_cycles.py` now judges against those two lines.
> With it the step 11 layout below comes to ~274,000 cycles (1.03× the 265,000 line, under 290,000): expect the OS to
> fade decaying notes when it is sustained, not crackles. The tables below keep the 218 of the time.

From [Q-04](ahd-drum-notes-cpu.md#q-04--cpu-clock-and-cycle-budget): a block is 580,499 cycles. The OS kills
a voice when the instrument stage takes **> 265,000 cycles three blocks running, or > 290,000 once**. That is
~13,900 cycles per voice per block (**218 per sample**) with 19 voices, *before* the OS's own share. The ABI's
target is 150 per sample for instruments and 600 for effects.

| Engine | Worst cycles/sample/voice, before → **after** | vs 218 | Voices that fit in 265,000 (no OS share), after |
|---|---:|---:|---|
| `macro` P2 0 | 478 → **123** | 0.56× | 33 |
| `macro` P2 ≥ 128 | 697 (saw), 615 (pulse) → **211, 196** | 0.97×, 0.90× | 19–21 |
| `kit` snares | 1,300 → **254–265** | 1.2× | 15–16 |
| `kit` toms, conga, hi-q, kicks 2/4 | 1,240–1,281 → **194–207** | 0.9× | 20–21 |
| `kit` hats/cymbal | 916 → **161** | 0.74× | 25 |
| `kit` cowbell, clave, blip, noise, shaker, clap | 286–838 → **88–115** | 0.4–0.5× | 35+ |
| `opz_test_sine` | 179–381 → **144–346** | 0.7–1.6× | 12–28 |
| `swirl` (effect, target 600) | 1,590 → **414** | 0.69× of its target | — (FX stage, outside the instrument window) |

**The 19-voice worst case** of the device (runbook step 11: 11 `macro` voices on bass/lead/arp/chord, 8 `kit` voices
on the drum tracks):

| Layout | Before | **After** | vs 265,000 |
|---|---:|---:|---:|
| 11 × `macro` P2 255 pulse + 8 × `kit` hats | 433,000 + 469,000 = 902,000 | 137,800 + 82,500 = **220,000** | **0.83×** (45,000 left) |
| 11 × `macro` P2 255 saw + 8 × `kit` hats | 490,000 + 469,000 = 959,000 | 148,800 + 82,500 = **231,000** | 0.87× |
| 11 × `macro` P2 0 + 8 × `kit` hats | 309,000 + 469,000 = 778,000 | 77,500 + 82,500 = **160,000** | 0.60× |
| 11 × `macro` P2 255 + 8 × `kit` snare tight | 433,000 + 666,000 = 1,099,000 | 137,800 + 130,000 = **268,000** | 1.01× |
| 19 × `macro` P2 255 (if all tracks could take it) | 747,000 | **225,000** (measured) | 0.85× |

That's still before the OS's per-track filter, DC blocker, LFOs and mixing, which only the device can show
(runbook step 11), and before `swirl` (21,000–26,500 per FX track) in the FX stage of the same 580,499-cycle
block. Hits add their note-on (~1,800 cycles each, ~14,000 if all eight drum voices start in one block) wherever
the OS runs it. Within the simulator's margin (×0.7–1.3, [below](#the-simulator-and-what-it-does-not-model)), the
hats layout lands between ~155,000 and ~290,000: the top of that range is where the OS's share would have to
fit, so step 11 on the device is the real test. Eight snares at once with 11 thick `macro` voices sits on the line.
The OS's kill rule only removes voices in release, fade or drum hold/decay; held `macro` notes and `kit` hits are
never candidates, so a block that runs long mostly can't be rescued: if it overloads, expect crackles rather than
shortened releases.

## Where the cycles go

**Before** the pass, `--profile` on the worst cases (share of engine instructions):

| Hot spot | `macro` pulse P2 255 | `kit` snare tight | `kit` open hat | `swirl` flanger |
|---|---:|---:|---:|---:|
| `opz_mul_q31` (`__builtin_bfin_mult_fr1x32x32`, 6 instructions) | 13 % | 14 % | 11 % | 16 % |
| `opz_sat32((int64_t)…)`: 64-bit compare chains, 10–15 instructions with 2–4 conditional branches | 9 % | 13 % | 15 % | 19 % |
| `___muldi3` (64-bit multiply, a libgcc call; calls per block) | 11 % (400) | 28 % (2,300) | 21 % (1,170) | 11 % (990) |
| 32/64-bit divisions in the per-block setup (`___udivsi3`, `___divdi3`) | 8 % | 2 % | 3 % | 11 % (8 all-pass coefficients per block) |
| `opz_svf` itself (4 int64 products per sample, besides the rows above) | — | 18 % | 26 % | — |
| per-sample `opz_exp2_q16` (pitch sweep) | — | 5 % | — | — |

For comparison, `__builtin_bfin_add_fr1x32` (saturating add) is **1 instruction** with no branch, and a 16 × 16
fractional multiply is 1. The 64-bit forms the C code used instead cost 10–40×.

**After** the pass there are no libgcc calls on the render path (the only ones left are per note or per pan change)
and no `opz_sat32` on an int64 per sample. What remains is mostly the exact 32 × 32 fractional multiply itself, six
instructions (`A1 = lo*lo; A1 >>= 16; A1 += hi*lo, A0 = hi*hi; A1 += lo*hi; A1 >>>= 15; R = A0 + A1`):

| Share of engine instructions | `macro` pulse P2 255 | `kit` snare tight | `kit` open hat | `swirl` flanger |
|---|---:|---:|---:|---:|
| `opz_mul_q31` (32 × 32 fractional multiply) | 31 % | 50 % | 50 % | 61 % |
| `opz_mul_q31_rnd` (the envelope step, same multiply) | 8 % | — | — | — |
| saturating add/subtract (1 instruction each) | 7 % | 10 % | 17 % | 19 % |
| libgcc | 0.1 % (note-on) | 0.1 % (note-on) | 0.3 % (note-on) | 0.3 % (first block's pan divisions) |

## Optimisations (macro v3, kit v4, swirl v3)

The first pass followed the plan of the first measurement, steps 1–2 fully, 3–5 where they kept the bits.

**SDK** (`opz_engine.h`, `opz_dsp.h`, `opz_adsr.h`):

- Saturating `opz_sub_q31` and `opz_shl_q31` next to `opz_add_q31` (one instruction each, `__builtin_bfin_*`), a
  3-instruction 32 × 16 `opz_mul_q31_q15` (inline asm: `A1 = c*x.lo (M), A0 = c*x.hi; A1 >>>= 15; R = A0 + A1`),
  `opz_norm32` (`SIGNBITS`), `opz_mul_u16` and an exact 32-bit `opz_mul_u32_shr16` for `(uint64_t)a * b >> 16`.
  Every host version gives the Blackfin's bits; `tools/cycles/sdk_check.c` checks that in the simulator
  (`HostMatchesDevice` in `tools/tests/test_opz_cycles.py`). The simulator also confirms that
  `__builtin_bfin_mult_fr1x32x32` is exactly `floor(a * b / 2^31)`, the host's `opz_mul_q31`.
- `opz_clip`, `opz_soft`, `opz_lerp`, `opz_tri`, `opz_sine`, `opz_saw_blep`, `opz_svf`, `opz_env_tick` and
  `opz_ahd_tick` without `opz_sat32((int64_t)…)`. Where saturation of a multi-term sum matters, the terms are
  ordered so that only the last add can saturate (same bits). `opz_exp2_q16` uses 16 × 16 multiplies (every
  partial result fits 16 bits: same bits). `opz_env_tick0` is the envelope step for step 0 (decay, release): one
  multiply instead of four, same bits.
- `opz_recip_q31`: 1 / (2m) by a linear first guess and three Newton steps (7 multiplies, within 2 LSB).
  `opz_blep_recip` uses it instead of a 64-bit division per block. `opz_decay_coef` and `swirl`'s all-pass
  coefficient correct its estimate with a 32-bit remainder to the exact quotient. `opz_udiv64_32` (restoring
  division, ~80 bytes) replaces libgcc's `___divdi3` (1.4 KB with its table) for per-note 64-bit divisions.
- The BLEP's rare edge case is one out-of-line call; the common case is one unsigned compare for both sides of
  the wrap.

**Engines**: each render runs in short passes over the block (oscillators, then mix, then tone/clip/output), with
the block's constant choices as compile-time constants in each copy, so the loops are hardware loops with
everything in registers and without per-sample branches:

- `macro`: oscillator 1 → oscillator 2 + sub + normalisation → tone, clip, level, envelope. The 64-bit detune
  `inc * ratio >> 16` per sample became two 16 × 16 products (exact); the normalisation and both BLEP reciprocals
  use `opz_recip_q31` (no divisions per block). The envelope step uses `opz_env_tick0` when the host's step is 0.
  Scratch buffer in the voice state (280 of 308 bytes).
- `kit`: tone (pitch-sweep increments, oscillator a, oscillator b, envelope), metal (the six squares counted in
  registers, three at a time, and summed from a 7-entry table), noise and click (same draw order as before), then
  filter, clip and output, one copy per filter mode. Block buffers in `.bss` (768 bytes, shared by all voices:
  render runs one voice at a time). Note-on without 64-bit divisions and with the noise filter's coefficients
  cached per model and tune; image 9.7 of 10 KB.
- `swirl`: the six all-pass stages keep 7 memories per channel instead of 12 (stage k's last output *is* stage
  k + 1's last input: same values), the 8 coefficients per block come from `opz_recip_q31` corrected to the exact
  quotient, the per-16-sample delay maths is exact 32-bit, wet/dry are recomputed only when pan moves (with
  `opz_udiv64_32`, like the note-on's period: no libgcc 64-bit code left, image 5.1 → 3.8 KB), and the phaser-only
  and flanger loops are separate copies.

What is left, if more headroom is needed: GCC still reloads and spills in `macro`'s oscillator passes (no hardware
loop there, because of the BLEP edge call: about 10 cycles per sample and oscillator pass), and 16-bit
coefficients (`opz_mul_q31_q15`, 3 instead of 6 instructions) for gains, `macro`'s tone and `kit`'s envelopes would
save another 10–20 % at the cost of LSB-level (≈ −100 dBFS) changes. `kit`'s snares (254–265) are the only
models above 218.

## Sound: null tests against the old builds

The pass was meant to keep every bit where that was cheap, and to stay far below audibility where not:

| Engine | Device build (`--dump`/`--compare`, every benchmark case) | Host golden scenarios (opzsim) | Where it differs |
|---|---|---|---|
| `macro` v3 | P2 0 bit-identical; P2 > 0 peak **−171 dBFS** | peak −175 dBFS or lower | normalisation and BLEP reciprocal (a few LSB) |
| `kit` v4 | peak **−152 dBFS** (8 hats summed to full scale), single hits −157 dBFS or lower | peak −154 dBFS | the filter's products rounded one by one, trim (≤ 3 LSB) |
| `swirl` v3 | peak **−187 dBFS** (1 LSB) | peak −187 dBFS (1 LSB); `extremes` bit-identical | output mix: two products rounded one by one |
| `opz_test_sine` v6 | bit-identical | bit-identical | — |

All golden renders are within 0.00 dB of their old octave-band spectra; the goldens were re-recorded. 16-bit
audio has its LSB at −96 dBFS, so every difference is at least 55 dB below it. One case taught a lesson: at
`swirl`'s extremes (flanger feedback 0.9, full wet, a full-scale input), a few-LSB difference in the all-pass
coefficient grew through the saturating feedback into a full-scale difference, so that coefficient is now exact;
anything inside a feedback loop should be.

## Reproducing

```bash
python3 tools/opz_cycles.py --all --json build/cycles/report.json
```

```bash
python3 tools/opz_cycles.py engines/kit --profile "key 6 snare tight, P2 255"
```

Null test of two builds (here: an older checkout in `../old` against this one):

```bash
python3 ../old/tools/opz_cycles.py --all --dump /tmp/old && python3 tools/opz_cycles.py --all --dump /tmp/new
```

```bash
python3 tools/opz_cycles.py --compare /tmp/old /tmp/new
```

The older checkout needs this version of `tools/opz_cycles.py` and `tools/cycles/driver.c` for `--dump`.

The simulator is deterministic: the same build gives the same counts (a test checks it). Counts change only when
an engine, the SDK headers, the compiler flags or the driver's host model change.
