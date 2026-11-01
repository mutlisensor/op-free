# The host's shared DSP tables (Q-15) and plug `flags` (Q-08)

Offline research, 2026-10-19, OS 1.2.45 (`opz.ldr`, Ghidra 12.1.4 + Blackfin+), the desktop app's `SnakePlugin`
(arm64, symbols) and TE's engines in the 1.2.45 package. No hardware. Builds on the P0 sessions' uncommitted notes
of 2026-10-18 (`p0-dsp-tables`, `p0-loading-flags`): their table formulas, flag readers and voice findings were
re-checked here against the image and the device code, and extended (accessors, simulator support, voicing, cycle
cost). No TE data, tables or decompiled code is reproduced; the tables are described by formulas, which we
regenerate ourselves. Evidence labels as in [CONTRIBUTING.md](../CONTRIBUTING.md); **[proven, offline]** = our
formula was regenerated and compared word by word with the device table.

## Summary

- **Every shared table is a formula.** Our regenerated tables match the 1.2.45 device tables to ±1 LSB
  (`g_sine`, `g_sineTable`, `rFilterCoeffs`, `releaseTable`) and ±11 LSB (`k_lp_table`, 5·10⁻⁹) [proven, offline;
  `tools/tests/test_opz_tables.py` `Device` test, local image only].
- **Engines reach them only through the callback table** passed to `initModule`, as TE's engines do. The
  addresses are 1.2.45-specific; only one OS version is available here, so stability across versions is unknown —
  never hard-code them. `opz_host_check_*()` refuses an OS whose tables changed.
- **Two ABI corrections:** `g_sineTable` is **256 × int16** (not 128 × Q31) and `rFilterCoeffs` is **5 pointers**
  to `[9][65]` Q31 biquad tables (not 10 × Q31).
- **Accessors:** [`sdk/include/opz_tables.h`](../sdk/include/opz_tables.h). opzsim and the cycle driver now
  pass formula-generated tables in the device formats; on the Blackfin simulator the accessors give the same bits
  as on the host [proven, offline].
- **Cycle cost:** a table sine is ~22 cycles/sample cheaper than our computed sine; the OS's `g_sine` costs the
  same cycles as an own table in the slot and **0 bytes** (an own 256-entry table is 512 B). Memory timing is not
  simulated (below).
- **Q-08:** `flags` is one byte with six live bits; **none changes voicing**. Voicing comes from the note style
  (`params[17]`: poly / mono / legato), portamento (`params[12]`) and fixed per-track voice counts. In legato the OS
  **does not call `onNoteOn`** for the new note: it glides the sounding voice's `pitch`.

## 1. Q-15 — the shared tables

### Where they are (OS 1.2.45)

The device's `ModuleCallbackInterface` is a static 13-entry table at `0x81003850` (a string follows it) [code,
device]. Its table entries, and who reads them:

| Entry | Table | 1.2.45 address (memory) | OS readers | TE engines that read it |
|---|---|---|---|---|
| `+0x08` | `k_lp_table` | `0x81128264` (DDR) | none | `delay`, `chorus`, `dist`, `reverb` |
| `+0x0C` | `g_sine` | `0x0800BB44` (L2 SRAM) | LFO, IIR peak, two more | `ep` |
| `+0x10` | `g_sineTable` | `0x11B019E8` (on-chip, L1 range) | none found | none of the 13 local engines |
| `+0x14` | `rFilterCoeffs` | `0x815C8070` → 5 tables at `0x81128468 + k·0x924` (DDR) | the track filter | `analoga` |
| `+0x18` | `releaseTable` | `0x815C7E70` (DDR) | `updateParametersADSR` | none (they use the callback) |

[code, device; memory types from the BF70x address map, inferred]. All five are initialised data in the image (LDR
data blocks; `g_sine` entries 0 and 8,192 lie in zero-filled memory and are 0).

**How TE's engines reach them** [code, device]: each engine stores the `cb` argument of `initModule` in a global
and reads the table pointer from it when it needs it — `ep` loads `cb->g_sine` (`[cb + 0x0C]`) and indexes it with
its oscillator phase `>> 19` every sample; `delay` loads `cb->k_lp_table` (`[cb + 8]`) once per block; `analoga`
loads `cb->rFilterCoeffs` (`[cb + 0x14]`) and passes the five pointers to its lookup. No TE engine uses a fixed
table address. **Stability across OS versions:** unknown (only 1.2.45 is available locally); the callback table is
the ABI, the addresses are not.

### Formats and indexing

| Table | Format | Formula (ours) | TE's indexing |
|---|---|---|---|
| `k_lp_table` | 129 × Q31 in (0, 1) | coefficient `c` of `y += c·(x − y)` such that **two** such poles in series are −3 dB at `fᵢ = 10 Hz · 2000^(i/128)` (10 Hz … 20 kHz, 11.67 steps per octave): `w = 2πfᵢ/44100`, `A = (1 − g²cos w)/(1 − g²)` with `g² = 2^(−1/2)`, `c = 1 − (A − √(A² − 1))` | integer index, no interpolation: `delay` `34 + (p >> 25) + (p >> 26)`, `dist` `min(36 + (p >> 24), 100)`, `chorus` fixed 64 (447 Hz), `reverb` several fixed/derived indices [code, device] |
| `g_sine` | 8,193 × Q31 | `round(0x7FFF0000 · sin(2πi/8192))`; entry 8,192 = entry 0 (guard) | `g_sine[phase >> 19]`, no interpolation (`ep`, the OS LFO); cosine = `+2048` [code, device + app] |
| `g_sineTable` | 256 × int16 | `min(32767, round(32768 · sin(2πk/256)))` | no reader; natural index `phase >> 24` |
| `rFilterCoeffs` | 5 pointers → `[9][65]` Q31 each: `b0, b1/2, b2, a1/2, a2` (`a0` = 1) | RBJ cookbook biquads: columns 0–29 low-pass at the ISO third-octaves 25 Hz … 20 kHz, 30–34 flat (31–33 `b0` = 1; 30 and 34 unity filters shaped like their neighbours), 35–64 high-pass at the same frequencies; rows `Q_LP` 0.2, 0.4, 0.6, 0.8, 1, 4, 8, 16, 16 and `Q_HP` 0.2, 0.4, 0.6, 0.8, 1, 2, 3, 4, 4; values rounded, 1.0 → `0x7FFFFFFF` | bilinear in coefficient value: column `c >> 25`, fraction `(c & 0x1FFFFFF) << 6`; rows `q >> 28`, fraction `(q & 0xFFFFFFF) << 3` (`analoga`) or `min(q >> 27, 7)`, fraction `(min(q, 0x3FFFFFFF) & 0x7FFFFFF) << 4` (OS track filter); truncating products, saturating sums [code, app + device] |
| `releaseTable` | 128 × Q31 | `e^(−1/τᵢ)`, τ in samples: `τ₀ = 33.64711`, `τᵢ = 0.9581 + 248.8227·i + 11.72398·e^(0.079528·i)` (our least-squares fit) | `releaseTable[param >> 24]`; decay uses `max(…, releaseTable[1])` [code, app + device] |

Comparison with the device (word by word, `tools/opz_tables.py`): `g_sine` 57 of 8,193 words 1 LSB off,
`g_sineTable` 2 of 256, `rFilterCoeffs` 3 of 2,925, `releaseTable` 9 of 128 (all 1 LSB), `k_lp_table` 121 of 129
within 11 LSB [proven, offline]. `p0-device-abi`'s "semitone-spaced" `k_lp_table` is wrong (the step is
`2000^(1/128)` ≈ 1.0611).

Not settled: whether TE's FX run two `k_lp_table` poles in series (the table is designed for it; the pseudocode of
`delay` is too noisy to tell) and the exact constants behind `releaseTable` (ours reproduce it, they are not TE's).

### Accessors (`sdk/include/opz_tables.h`)

| Accessor | Does | Cost |
|---|---|---|
| `opz_host_sine(g_sine, phase)` | TE's lookup, `phase >> 19` | 1 load |
| `opz_host_sine_lerp(g_sine, phase)` | linear interpolation with the guard point, error < 1e-7 | 2 loads + 3-instruction multiply |
| `opz_host_cos(g_sine, phase)` | `phase + 2^30` | 1 load |
| `opz_host_sine16(_lerp)(g_sineTable, phase)` | the int16 table, plain or interpolated (−80 dB) | 1–2 loads |
| `opz_host_klp(k, i)`, `opz_host_onepole(y, x, c)` | clamped coefficient; one pole (run two for the table's −3 dB point) | — |
| `opz_host_rf_lookup(rf, cutoff, res, coef)` / `_os` | bilinear lookup with `analoga`'s / the OS filter's row rule, per block | ~20 multiplies |
| `opz_host_biquad(state, coef, x)` | TDF-II step, the OS filter's form without its soft-clip | 5 multiplies |
| `opz_host_release_coef(rel, param)` | the OS's decay multiplier for a knob word | 1 load |
| `opz_host_check_klp/sine/sine16/rf/release(cb)` | present and matching our formulas at 3–5 points (64 LSB; 1 for int16) | ~96 B of code each, once |

The tables come from the device at run time; nothing is copied into the slot. Simulators stand in with
[`opz_tables_gen.h`](../sdk/include/opz_tables_gen.h) (C, libm-free, deterministic) and
[`tools/opz_tables.py`](../tools/opz_tables.py) (Python); the two produce identical words, checked by `make -C sim
test` (fixture `tables_probe`).

### How a 5 KB engine should use them

1. In `initModule`, keep `cb` in a global (TE does the same) and call the checks for the tables you read, e.g.
   `if (!opz_host_check_sine(cb)) return 0;` — an OS with different tables then fails to load the engine instead of
   playing garbage.
2. In `render`, load the pointer once per block (`const fract32 *sine = s_cb->g_sine;`), then
   `opz_host_sine(sine, ph)` per sample (TE's quality), or `opz_host_sine_lerp` for a clean sine (+13 cycles).
3. For a smoothing or damping pole, pick a `k_lp_table` index per block (`OPZ_KLP_100HZ` 39, `OPZ_KLP_1KHZ` 78,
   `OPZ_KLP_10KHZ` 116) and run `opz_host_onepole`. For a resonant LP/HP, `opz_host_rf_lookup` per block and
   `opz_host_biquad` per sample; with plug flag `0x04` (below) the engine owns the filter/resonance knobs.
4. To follow the OS's decay/release curve in an engine-owned envelope, `opz_host_release_coef(rel, params[5])`.
5. Never hard-code a table address and never copy a table into the slot.

### Cycle cost against our own tables

A probe voice (one sine per voice, host ADSR, stereo add; built with the project's flags and run by
`tools/opz_cycles.py` in bfin-elf-run, 4 voices; scratch code, not committed):

| Oscillator | cycles/sample/voice | slot bytes for the sine |
|---|---:|---:|
| computed (`opz_sine` in `opz_dsp.h`, parabola + correction) | 79.5 | 0 |
| own 256 × int16 table in the slot, no interpolation | 57.6 | 512 (+ init loop) |
| own 256 × int16 table, interpolated | 68.5 | 512 |
| **OS `g_sine`, `opz_host_sine`** | **57.6** | **0** (+96 B for the check) |
| OS `g_sine`, `opz_host_sine_lerp` | 70.5 | 0 |
| OS `g_sineTable`, `opz_host_sine16_lerp` | 75.5 | 0 |

Reading through `s_cb` every sample instead of a local cost nothing extra here (GCC hoisted it). The simulator
charges every load one cycle, so these numbers say nothing about memory: on the device `g_sine` is in L2 SRAM
(on-chip, no cache misses, a few cycles per access on BF70x [inferred]), `g_sineTable` in the on-chip L1 range, and
an engine's own tables sit in DDR with the slot (fast when cached, slow on a miss; whether the OS caches the slot
area is unknown). A 32 KB table read at random phases is the worst case for a data cache, which favours the host's
L2 copy over an own table of that size [inferred]. Measure on the device with runbook step 11.

### Simulator and driver changes (shared files)

| File | Change |
|---|---|
| `sdk/include/opz_engine.h` | `g_sineTable` → `const int16_t *`, `rFilterCoeffs` → `const fract32 *const *`; comments with the formats |
| `sim/src/callbacks.c` | tables from `opz_tables_gen.h` instead of the earlier guessed shapes (no engine read them: goldens unchanged) |
| `sim/Makefile` | header dependencies |
| `sim/tests/golden.py`, `sim/tests/fixtures/tables_probe.{c,scn}` | host check: tables + accessor outputs against `tools/opz_tables.py` |
| `tools/opz_cycles.py` | `bench_header` appends `opz_tables.c_source()` (one line) |
| `tools/cycles/driver.c` | the callback table points at those arrays (was one zero-filled stand-in for all five) |

The driver's tables sit in its own memory, not at the OS's addresses; bfin-elf-run does not model memory timing, so
placement makes no difference to the counts. Existing engines read no table: their cycle numbers are unchanged.

## 2. Q-08 — plug `flags`

### The bits

One byte at plug record `+0x7A8` (higher bits are dropped when the plug is read) [code, app `readPlugSettings`;
device store `0x800FBD4A`]. `getPlugFlags` is device `0x800FC198`; the device has six calls to it and no other read of
`+0x7A8` outside the plug manager [code, device; re-checked: the call sites below].

| Bit | Device reader | Meaning | TE plugs |
|---|---|---|---|
| `0x01` | `0x80112340`/`0x80112352` (pattern load, FX1/FX2 plugs) | **insert-type effect**: when an old project is upgraded, FX1's/FX2's parameter byte 12 becomes 0 instead of 255 | tape, dist, crush |
| `0x02` | written `0x800FFAB2` (adding a user plug), tested `0x800FFCC2` (clean-up) | **user sample plug**, a clean-up candidate | user sample plugs |
| `0x04` | `0x8012D42E` in `InstrumentManager::noteOn`: per-track byte `Synth+0x576C` = 0 if set (plug 145 forced to 1); `Synth::render` runs the track filter only when it is 1 | **skip the OS track filter**: filter/resonance belong to the engine | analoga (20), UserSampler, plug 145 (overridden) |
| `0x08` | `0x8011F056` (`DefaultContext::onCC`) | **P1 is bipolar**: centre-detent LED cue on the green dial (LEDs only) | none |
| `0x10` | same | **P2 is bipolar** (blue dial) | ep (16), analoga (20) |
| `0x20` | `0x801186A8`/`0x801186B8` (FX1, FX2 plugs) | **send² dry ducking** for an FX plug (TE hard-codes the same for plug 34, `delay`) | organ (32), an instrument: no effect |
| `0x40`, `0x80` | none | unused | — |

[code, app + device; the LED-cue reading of 0x08/0x10 is from the app's `DefaultContext::onCC` and
`LedAnimation::centerClick`, the "dry level" name of byte 12 from the manual's 1.2.26 "dry mix" (p.6), inferred.]

**FX parameter byte 12** (portamento on instrument tracks) is the FX track's **dry level**: each sending track's dry
signal is scaled by `(1 − (1 − dry₁)·s₁)(1 − (1 − dry₂)·s₂)` with `s` the track's send (squared with 0x20) [code,
app; device stores the same `1 − dry` form]. Dry 255 = classic send/return, dry 0 = the insert-like crossfade we
heard on hardware. **All our FX presets ship `values[12] = 0`**, so every one of our effects crossfades today.

### Voicing: what the flags do not do

No flag reaches voice allocation, the voice count, legato or glide [code, device: the six readers above are all
there is]. What does:

- **Voice counts are fixed per track** [code, device tables `0x815C7ACC`/`0x815C7AEC`/`0x815C7B0C`]: kick, snare,
  perc, sample, bass 2 each, lead 3, arp 2, chord 4 (6 with the generous-chords setting), 22 voices per track layer
  (a second layer has its own 22). Nothing in the plug changes them.
- **Note style** (`params[17]`) picks the allocation mode [code, app `Sequencer::getNoteStyle` `0x4BF24` and device
  `InstrumentManager::noteOn` `0x8012D278`, whose modes 1 (reuse the sounding voice) and 2 (first voice only) are
  getNoteStyle's legato and mono codes; that the value is passed through unchanged is inferred]. Synth tracks: byte 0–85 **poly** (best free or oldest voice), 86–170
  **mono** (always the track's first voice; a sounding voice fades out, stage 5, then the engine gets `onNoteOn`),
  171–255 **legato**. Drum tracks: 64–127 mono, otherwise retrig (gate and loop are handled elsewhere).
- **Legato never calls the engine** [code, device `Voice::noteOn` `0x8012E0A4`–`0x8012E11C`; app `Voice::noteOn`
  `0x71014`]: when a voice of the same plug is sounding, the OS sets its new note and pitch target (glide step
  computed from the pitch difference) and returns. No `onNoteOn`, no envelope retrigger: the engine only sees
  `pitch`/`pitchSlope` move. If the voice is gated off it falls back to a normal note.
- **Portamento** (`params[12]`) is the OS's: it moves `pitch` per block in every style.

**For a mono lead with glide** an engine needs nothing new: the user sets note style to legato (or mono) and
portamento up. The engine must (1) read `pitch` and `pitchSlope` every block and never latch a frequency at
note-on, (2) not expect one `onNoteOn` per note — in legato the note number it saw stays the first one, so derive
key tracking from `pitch`, (3) keep its own per-note state (phase, filter, strike) running across a legato change.
Presets can ship a mono lead: `values[17]` in 171–255 (legato) or 86–170 (mono) and `values[12]` > 0.

### Recommended flags for our engines (applied 2026-10-20)

| Engine | Flags | Why |
|---|---|---|
| synths with unipolar P1/P2 (chip, dx, ensemble, fm2, fold, macro, modal, pd, pluck, super, swarm, sync, table, vowel, test sine) | `0` | as today |
| `kit` v5 (P1 tune ±1 octave, P2 decay ×¼…×2), `fmperc` v2 (ratio, sweep), `hats` v2 (tone, decay), `snap` v2 (spread, decay): both knobs nominal at 128 | `24` (0x08 + 0x10) | centre-detent cue on both dials, as TE's ep/analoga |
| `8bit` v2 (P1 tune −12…0…+12 semitones; P2 crunch is not centred) | `8` (0x08) | P1 cue only |
| `mallet` v2 (P2 ring ×1 at 128; P1 is a material morph) | `16` (0x10) | P2 cue only |
| a future engine with its own filter on `params[8]`/`[9]` (e.g. a 5 KB VA using `opz_host_rf_lookup_os`) | add `0x04` | the OS filter is skipped, so the knobs are the engine's; without it the sound is filtered twice |
| send-style FX: `echo`, `plate`, `shimmer`, `clouds` | `0`; preset `values[12] = 255` (applied, no version bump: `opz.py presets push`) | keep the source's dry signal (send/return); `echo` may add `0x20` (delay-style send²) once T-4 is heard |
| insert-style FX: `filter` v2, `glue` v2, `swirl` v4, `resonator` v2 | `0x01`; preset `values[12] = 0` (unchanged) | TE's convention; the flag itself only matters when an old project is upgraded |
| any | never `0x02` | marks user sample plugs for clean-up |

Changing `flags` needs a plug version bump (the OS re-imports only higher versions); the engines above bumped plug
and engine version together, as every release here does, so installing them means an OP-Z restart. Only plug.json,
presets.json and README plug rows changed; no engine code. On the device nothing changes until each is reinstalled
(flags) or its presets are pushed again (dry level).

### Hardware check (not run; one engine, dry run first, rollback)

`opz_test_sine` (60901, slot 57, already a test plug) with `flags: 4`, then `flags: 24`:

1. Offline: set `"flags": 4` and bump `engine_version`/`version` 6 → 7 in a scratch copy of
   `engines/opz_test_sine/plug.json`; `python3 tools/opz_build.py <copy>`; `.venv/bin/python tools/opz.py --fake
   install <copy> --track lead --yes` and read the generated patch entry (flags 4, version 7).
2. Dry run against the device: `.venv/bin/python tools/opz.py install <copy> --track lead --dry-run`.
3. With the user's approval: the same without `--dry-run` (the automatic backup comes first), restart the OP-Z
   (a loaded engine is replaced only at a restart). Expected: on lead, turning **filter** and **resonance** no
   longer changes the test sine (with flags 0 it did) — 0x04 proven.
4. Version 8 with `flags: 24`: on the first page P1 and P2 should show the centre animation passing 128.
5. Rollback: version 9 with `flags: 0` (versions only go up), or `opz.py restore <backup dir>` for the slot map.

Optional, same pattern: `echo` with preset `values[12] = 255` (dry kept at full send) vs 0, then `flags: 32` at
half send (less dry loss) — the T-3/T-4 checks of the P0 note.
