# Drum envelope, drum-key notes and CPU budget

Findings for three open questions: **Q-17** (`updateParametersAHD`), **Q-14** (the note numbers drum keys send)
and **Q-04** (CPU clock and the cycle budget). Offline work: the desktop app's arm64 code and the device images,
no hardware. Decompiled TE code stays in `build/` (git-ignored); this page gives our description and the
addresses to check it.

Evidence labels: **[code, app]** = `SnakePlugin` arm64 (symbols present; `lipo -thin arm64`, then
`objdump -d -C --arch=arm64` or Ghidra). **[code, device]** = `opz.ldr`, `bootloader.ldr` or a `.engine` from
firmware 1.2.45, read with the Blackfin+ extension in Ghidra (`tools/ghidra/`). Addresses are as Ghidra loads
them.

## Summary

| Question | Answer |
|---|---|
| Q-17 `updateParametersAHD` | `(env, pos, attack_end, hold_end, decay_end, coef, shape)`: the arguments are **play positions in frames**, not knob words. Stages 1 attack → 7 hold → 4 decay → 0. The engine steps the level **linearly**: `level = coef·level + step`. No rate tables; note-off is ignored. TE's drum player derives the landmarks from params[2..5] and the sample length. Implemented as `opz_ahd_layout` / `opz_ahd_tick` in `opz_adsr.h` and in the simulator |
| Q-14 drum notes | The 24 keys send **MIDI 53–76** (F3…E5), lowest key first, at octave 0, so `key = (note − 53) mod 24` is right. On drum tracks the octave buttons move **2 octaves per press**, i.e. **±24 notes**: the same key index, one sample-octave up or down (`octave = floor((note − 53) / 24)`) |
| Q-04 CPU | **CCLK = 400 MHz** (CLKIN 25 MHz × 32 / 2; SYSCLK 200 MHz, DCLK 200 MHz), set by the bootloader's init block. The OS kills a note when the **instrument stage** of a block takes **> 290,000 cycles**, or **> 265,000 cycles three blocks in a row** (a block is 580,499 cycles). It kills one voice per block: the quietest one in stage ≥ 4 |

## Q-17 — the drum envelope (`updateParametersAHD`)

### The function [code, app `updateParametersAHD` @ `0x2be3c`]

A switch on `env->stage` (`+16`), through a 7-entry jump table:

| Stage | When | Writes |
|---|---|---|
| 1 attack | `attack_end > pos` | `coef = 1`, `step = 0x7fffffff / (attack_end − pos)` (unsigned divide) |
| | `attack_end ≤ pos` | stage = 7, fall into hold |
| 7 hold | `pos < hold_end` | **`level = 1`**, `coef = 1`, `step = 0` |
| | `pos ≥ hold_end` | stage = 4, fall into decay |
| 4 decay | `decay_end > pos` | `coef = coef` argument; `step = −(level / max(decay_end − pos, 64)) · (1 − f·shape³)⁴` with `f = (decay_end − pos) / (decay_end − hold_end)` (Q31, divisor 1 if zero) |
| | `decay_end ≤ pos` | **stage = 0**, `coef = 0x70000000`, `step = 0` (voice freed) |
| 5 fade | | unchanged `coef`; `step = −min(|step|, level·0x4D7A7880 >> 37)`; level < `0x10624E` → stage 0 (as the ADSR) |
| 6 fade | | `coef = 1`, `step = −(level·0x4D7A7880 >> 37)`; level < `0x10624E` → stage 0 |
| 0, 2, 3 | | nothing |

Q31 products are saturating; the three `·shape` products truncate, the `u⁴` chain is unsigned. The release flag
(`+21`) is never read and `+0` (attack step) is never written: **drums ignore note-off**. Unlike
`updateParametersADSR` there are **no rate tables**: every rate comes from positions.

### What drives it: TE's sample player [code, app `SamplePlayerGain::onNoteOn` @ `0xc7c8`, `::render` @ `0xb7f4`; device `sampleplayergain.engine` `onNoteOn` @ `0x81b69f20`, layout helper `FUN_81b690be`, call @ `0x81b69e5a`]

Each block (`render` @ `0xbd2c` app; `0x81b69e28` device):

```text
updateParametersAHD(env, max(play_pos >> 10, 0), attack_end, hold_end, decay_end, coef, shape)
```

`play_pos` is the sample read position in frames (Q22.10), so the envelope follows **the sample's own timeline**:
pitching a drum up shortens its envelope. On the device the four stack arguments go out in one parallel bundle
(`0x81b69e38`) that stores `[state+0x34]` before clearing the register, which the decompiler shows as a 0.

**Per sample** (app `0xc73c`; device `render`, `[env+8]·[env+4] + [env+12]`):

```text
level = sat(coef · level + step)        // linear — not the ADSR's r + (1 − r³)·s
```

so the attack, with `step = 1/(frames left)` recomputed every block, rises along `−ln(1 − t/T)` and clips at 1.0
after about 63 % of the attack time; the hold then pins it at exactly 1.0. With `shape = 0` the decay is a straight
line that reaches 0 at `decay_end` (the per-block `level/left` steps telescope), times `coef^t`; `shape = 1` keeps
the level up and drops it near the end (it is cut at `decay_end`, possibly from a few percent).

**Landmarks** from the knobs, for a sample of `len` frames (computed at note-on and again every block from the
live knobs; app `0xb8dc`–`0xba04`):

| Knob (instrument name) | Drum meaning | Arithmetic |
|---|---|---|
| params[2] (attack) | **start** point | `start = min(p·len, len − 512)` |
| params[3] (decay) | **attack** | `attack = p'·(len − start)/2`, `p' = max(p, 0x400000)` when `start > 0` |
| params[4] (sustain) | **hold** | `hold = max(p·rest, 512)`, `rest = len − start − attack` |
| params[5] (release) | **decay** length and curve | below 0.5: `coef = 0x7ffe5c92` (device; app `0x7ffe5c80`) ≈ e^(−1/20,000), `shape = 0`, length `2p·(rest − hold)`; from 0.5: `coef = 1`, `shape = 2p − 1`, full length |

`attack_end = min(start + attack, len − 192)`, `hold_end = min(start + attack + hold, len − 64)`,
`decay_end = start + attack + hold + length`. App and device agree term by term.

**Note style (params[17] >> 29)** picks the play mode [code, app `onNoteOn`/`render`]: 2 = **gate** (on key
release the engine itself sets stage 5, `coef = 0x7F000000`, `step = −level·0x7FFFFF`, the host's steal fade), 3 =
loop while held (the update is skipped in hold); 0 and 1 are one-shots (0 can take the mode from the sample's
header). `onNoteOff` only clears a gate byte.

### What it means for drum engines

- A synthesized drum engine can **follow the OS drum envelope** by choosing a sound length `len`, keeping a
  position counter, and calling the host once per block — `opz_ahd_layout`, `opz_ahd_tick` and the recipe in
  [`opz_adsr.h`](../sdk/include/opz_adsr.h). The knobs then behave as on TE's drum tracks (start, attack, hold,
  decay), the voice is freed at `decay_end`, and voice steals fade through stage 5.
- Stage 7 (hold) and 4 (decay) are ≥ 4, so drum voices can be **chosen by the overload killer** (below) during
  their hold and decay, unlike ADSR voices before release.

## Q-14 — what the drum keys send

**Keys → notes** [code, app `Sequencer::handleKeyMessage` @ `0x7be40`, `shared::KeyToMidi` @ `0x31624`]: key
messages `0x10`–`0x27` are the 24 note keys; `KeyToMidi` maps them through a 51-entry table to **53, 55, 57, 59,
60, 62, 64, 65, 67, 69, 71, 72, 74, 76** (white keys, left to right) and **54, 56, 58, 61, 63, 66, 68, 70, 73, 75**
(black keys): MIDI 53–76 chromatically, F3 first. Other key codes become CCs.

**Octave** [code, app `Sequencer::noteOn` @ `0x4ce9c`, `DefaultContext::trackOctaveUp` @ `0x766b0` /
`trackOctaveDown` @ `0x76854`]: `note = KeyToMidi(key) + 12·octave[track]`, folded into 0–127 by whole octaves.
The octave buttons add **±2** to `octave[track]` on tracks 0–3 (kick, snare, perc, sample) and ±1 elsewhere,
within per-track limits (values not read). So **one press on a drum track moves the keys by 24 notes**: the same
`(note − 53) mod 24`, one octave of playback rate up or down.

**Engine side** [code, app `SamplePlayerGain::onNoteOn`; device `onNoteOn` @ `0x81b69f20`, same `·0x2aaaaaab`
divide]: TE's drum player computes `octave = floor((note + 19)/24) − 3 = floor((note − 53)/24)` and doubles or
halves its playback increment per octave; `getSample(packId, note)` receives the full note ([samples.md](samples.md)). `Voice::noteOn` passes
the note to the engine unchanged [code, app `0x71014`].

Consequences: `kit`'s `key = (note − 53) mod 24` is correct; an engine can read the drum track's octave as `floor((note − 53) / 24)` (0 at the default octave). Not traced:
whether incoming MIDI notes skip the octave offset (the external-MIDI branch of `Sequencer::noteOn`), and the
limits of `octave[track]`. `Sequencer::octaveShiftTrackNotes` moves recorded drum notes by ±1, not ±12 [code, app
`0x50c48`]; its UI meaning is not known. The device's key table is not stored the same way; the device evidence
for the 53 base is the engine-side formula above.

## Q-04 — CPU clock and cycle budget

### Clock [code, device `bootloader.ldr`]

`bootloader.ldr` starts with a small application: 0x5c bytes at `0x11800000` (DDR timing floats) and 0x1010 bytes
of code at `0x11a00000`, run by the boot ROM as an INIT block (`flags 0x0804`) before anything is loaded into
DDR. Its clock routine `0x11a00118` (disassembled linearly from the block loaded raw with the Blackfin+ language)
programs the register block at `0x20002000`, which behaves as the clock generation unit:

- If `CTL & 0x7f01 ≠ 0x2000`: write `DIV = 0x04042442`, `CTL = 0x2000` (MSEL 32, DF 0), wait for lock and alignment
  (`STAT` bits 0–3). Otherwise write `DIV = 0x44042442` (the same with the update bit). Then `CLKOUTSEL = 1`.
- Its own frequency helpers (`0x11a0056c`, `0x11a005a6`, `0x11a005fc`, `0x11a00694`) compute `PLL = 25,000,000 ×
  MSEL (0 → 128) >> DF` and divide by `DIV` fields: CSEL 2, SYSSEL 4, S0SEL 2, DSEL 4.

So **CLKIN 25 MHz, PLL 800 MHz, CCLK 400 MHz, SYSCLK 200 MHz, SCLK0 100 MHz, DCLK 200 MHz**. `opz.ldr` never touches `0x20002000`, and its time conversions hard-code 400 MHz
[code, device: 64-bit 400,000,000 at `0x81112ab4`, `0x81112abc`, `0x810030c8`, `0x810030d0`; used by
`FUN_8011b384` (cycles → µs), `FUN_8011b3ec` (cycles → ms) and the delay loop `FUN_11a00244`; an override word at
`0x815ca1d8` is −1 and nothing writes it]. The app emulates the same clock: `get_time_monotonic() ×
400,000,000` [code, app `0x52f40`, constant at `0x843c0`].

One 64-frame block is 64 / 44,100 s × 400 MHz = **580,499 cycles** (9,070 per frame).

### The overload rule [code, app `Synth::render` `0x52f40`–`0x538e4`; device `0x80118670`–`0x80119c1a`]

`Synth::render` reads the cycle counter before the instrument tracks and again after they are mixed. In the app
the window holds the `InstrumentManager::render` calls (every voice's `render`), the LFOs, vibrato, and the OS's
per-track filter and DC blocker, and **not** the FX, tape or master stages, which follow. On the device the second
read (`0x80118dbe`) follows a 128-value saturating mix loop, the same place. Then (device: `elapsed = CYCLES − start
− 8`, 64-bit):

```text
if elapsed > 290,000 (u64 at 0x8110f5fc)           -> kill one note now
else if elapsed > 265,000 (u64 at 0x8110f604)      -> count (byte 0x8110f5f8); at 3 in a row -> kill one note
else                                               -> count = 0
```

(skipped while the byte at `0x8110e51d` is set; 0 in the image). The app has the same constants (290,000 and
"≥ 265,001").

**Which note** [code, app `InstrumentManager::handleOverload` @ `0x3bff4`; device `0x8012d142`]: of the 44 voice
records (stride 400 in the app, `0x17c` on the device), skip free ones (marker byte 0xff) and those with stage
< 4; kill (`Voice::killNote`, device `0x8012dfae`) the one with the smallest |level|, and log
`cpu overload: kill notes %d` (string at `0x815c7b6c`). One voice per block; if no voice is releasing, fading or
in a drum hold/decay, nothing is killed.

### Budget for engine authors

> **Superseded 2026-10-23:** [cpu-budget.md](cpu-budget.md) counts the OS's own share (~61,500 cycles per block),
> which leaves ~167 cycles/sample/voice with 19 voices, and gives the full per-block structure and overload details.

- All instrument voices together, plus the OS's per-track work, must stay under **265,000 cycles per block
  (≈ 4,140 per frame, 45.7 % of the CPU)**; 290,000 (50 %) is the hard line.
- With the simulator's voice counts (19 instrument voices at most) that is ≈ 13,900 cycles per voice per block, or
  **≈ 218 cycles per frame per voice before the OS's own share**. Measuring the OS's share (filters, LFOs, mixing)
  needs the device: that is runbook step 11. The ABI's "≤ 150 cycles/sample/voice" target stays a sensible
  default.
- Overload only ever removes voices that are already in release or fade (or a drum in hold/decay): an engine that
  is too slow in its attack/sustain phase is never relieved by it, the block simply runs long.

### Measured: our engines in the Blackfin simulator (2026-10-18)

The device builds of our engines, run in the GNU Blackfin simulator with the real exports (method, tables and
margins: [cycle-model.md](cycle-model.md)). Cycles per voice per 64-frame block, against ~13,900 per voice
(218 per sample) for 19 voices:

| Engine | Cycles/block/voice | Per sample | Voices in 265,000 |
|---|---:|---:|---:|
| `macro` P2 0 / P2 ≥ 128 | 28,000–31,000 / 39,000–45,000 | 440–480 / 610–700 | 8 / 5–6 |
| `kit` hats / pitched-noise hits / cowbell, blip | 58,600 / 76,000–87,000 / ~18,500 | 916 / 1,190–1,300 / 290 | 4 / 3 / 14 |
| `swirl` (per FX track) phaser / flanger | 79,000 / 102,000 | 1,240 / 1,590 | — |
| `opz_test_sine` | 11,400–24,400 | 180–380 | 11–23 |

The step 11 layout (11 `macro` + 8 `kit` voices) comes to ~900,000 cycles per block before the OS's own share.
That is over the whole 580,499-cycle block, and still ~590,000 on a pure instruction count. So the engines need
the optimisations listed in the note (64-bit C arithmetic in the inner loops) before the 19-voice case can work.
The OS's own share still has to be measured on the device.
