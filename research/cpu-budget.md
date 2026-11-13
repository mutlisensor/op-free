# CPU budget: one audio block, the overload rule, and what is left for our voices

Research of 2026-10-23, offline only (no command was sent to the OP-Z). It replaces the estimate in
[ahd-drum-notes-cpu.md](ahd-drum-notes-cpu.md#budget-for-engine-authors) and [cycle-model.md](cycle-model.md#budget-and-headroom),
"265,000 cycles for 19 voices = 218 cycles/sample/voice before the OS's own share", with the OS's own share counted
from the device code, and it answers **Q-22** (what a hang does). Sources: `opz.ldr` and `bootloader.ldr` 1.2.45 in
Ghidra (Blackfin+ extension), the desktop app's arm64 `SnakePlugin` (same C++ source, with symbols) to name the
device functions, and TE's `.engine` files for comparison. Evidence labels as in [CONTRIBUTING.md](../CONTRIBUTING.md);
addresses are device addresses in `opz.ldr` unless marked. No TE code is quoted.

**What "estimated" means here.** The OS's cycle counts are *issue-slot counts*: one cycle per instruction or parallel
bundle in the Ghidra listing, times each loop's trip count. They leave out pipeline stalls, branch penalties and
multi-cycle instructions (push/pop multiple, calls), so they are a floor; the device is likely 0–30 % slower. Our
engines' numbers are measured in the GNU Blackfin simulator ([cycle-model.md](cycle-model.md), margin ×0.7–1.3).
Nothing here is measured on the device yet: hardware check **H-C1** below does that.

## The short version

| Question | Answer |
|---|---|
| What the OS times | The **instrument stage** only: from `CYCLES` at `0x80118670` to `CYCLES` at `0x80118dbe` in the Synth render `0x801184c0`. It holds, per instrument track (8): two LFOs, vibrato, both voice layers' `render` calls, the OS track filter, a DC blocker and the mix into the drum/synth buses and the FX/tape sends; then the drum + synth bus soft clipper [code] |
| What it does not time | Input track, tape, FX1, FX2, master (plug, filter, compressor, output), the audio callback around the Synth render, the interrupt entry [code] |
| OS share of the timed stage | **~61,500 cycles per block** with all 8 instrument tracks active and 19 voices: ~6,600 per active track, ~350 per sounding voice, ~1,300 fixed, ~770 for the bass track's second filter stage [code, estimated floor] |
| Overload rule | After the stage: `elapsed − 8 > 290,000` → kill now; `> 265,000` → count, kill once the count reaches 3, and again every further block while it stays above; otherwise reset the count. One voice per kill: the quietest of the 44 voice records with stage ≥ 4 (release, fade, drum hold/decay). Checked every block (1.45 ms); skipped during "render song" [code] |
| What a kill sounds like | The voice gets the host fade (stage 5, coefficient `0x7F000000`, step −level/256 per sample): it fades out in ~10 ms, no click. The engine's `onNoteOff` is called **inside the audio interrupt**, and usually again in the next blocks while the overload lasts, because the fading voice is still the quietest [code; fade time inferred from the stage-5 formula]. Held notes, attacks and FX are never killed [code] |
| Voice limit | **19 per layer, fixed**: kick, snare, perc, sample, bass 2 each, lead 3, arp 2, chord 4; chord 6 (21) with `generous_chords` in `general.json`. Layer 0 is sequenced notes, layer 1 live-played notes, each with its own 22 records, so live playing over a pattern can sound up to 19 more voices. No plug flag or note style raises it [code] |
| **Per-voice budget** | **≈ 10,700 cycles per block = 167 cycles/sample/voice** (steady, 19 voices, 8 active tracks), **188** for any single block. Plausible range 152–175 if the OS is up to 25 % slower or a little faster than counted. The ABI's 150 target is safe under the whole range |
| Q-22: a hang | Nothing on the Blackfin recovers [code]: the timer tick still interrupts, but no task runs again (UI, sequencer, USB, the PSoC's power-off request), no watchdog is armed, the DMA replays the last two blocks [inferred]. Whether the power knob still cuts power is up to the PSoC firmware (**Q-33**, open) |

## 1. One audio block, start to finish

`SPORT0` DMA interrupt `0x80030fb8` (IVG11 through the SEC; dispatcher `0x11a02766`) flips the ping-pong halves of the
output buffer (`0x11b004e8`, 2 × 0x600 bytes) and the input buffer (`0x11b010e8`, 2 × 0x400 bytes), then calls the
audio callback `[0x11b004c8]` = `0x8002325c` [code]. The callback reads USB audio in (`0x8002f7e4`, a 512-frame ring
at `0x81967628`), runs the Synth render `0x801184c0` unless the "render song" flag `0x8110e51d` is set (then it
outputs silence), hands the mix on (`0x80117cd4`), applies the output gain ramp (64 frames), writes USB audio out
(`0x8002f944`) and posts a semaphore (`0x80134064`) [code; names of the USB helpers from their log strings].

Inside the Synth render (names from the app's `Synth::render` `0x52ce4`, whose calls match the device's one for one):

| Step | Device code | What | When | Cycles per block (estimate) | Timed? |
|---|---|---|---|---:|:-:|
| Clear six buses | `0x801184f8`–`0x80118586` | 128 words each: drum bus `0x11901204`, synth bus `0x11901404`, FX1 send `0x11901604`, FX2 send `0x11901804`, tape send `0x11901c04`, output | always | ~900 | no |
| Window start | `0x80118670` | 64-bit `CYCLES` read under CLI/STI | | | — |
| FX flags, module sends | `0x800fc198` ×2 (`getPlugFlags` of FX1/FX2: flag 0x20), `0x80109fee` (`getModuleSends`) | | always | ~100 | yes |
| **Per instrument track 0–7** | loop `0x80118814`–`0x80118d5a` | | | | |
| · LFOs | `0x8012d994` ×2 (`Lfo::render`) | copies the track's 18 parameters and applies two LFOs (one value per block) | always | ~330 | yes |
| · vibrato | `0x8012d818` ×2 (layers 0, 1) | writes the vibrato value into each voice of the track | always | ~160 | yes |
| · voices | `0x8012d204` ×2 (`InstrumentManager::render`, layers 0 and 1) → `Voice::render` `0x8012e2c0` → engine `render` | glide, pitch bend/vibrato in cents (`0x8011aa1c` ×2, a ROM division each), the engine call, idle check | per sounding voice | **~350 + the engine** | yes |
| · track filter | `0x11a00e18` (`trackFilterProcess`, L1) | stereo biquad from `rFilterCoeffs`, 35 slots × 64 frames; the bass track (4) gets a second 64-frame stage | voices rendered **and** the plug lacks flag 0x04 (`0x80118986`, byte `0x1190576c + track` written by `InstrumentManager::noteOn` `0x8012d278`) | ~2,400 (+770 bass) | yes |
| · DC blocker | `0x11a01548` (`trackFilterDcBlock`, L1) | 10 slots × 64 frames | until the track has been silent for 1,000 blocks (1.45 s; counter at `+0x5444`) | ~670 | yes |
| · mix | loop `0x80118c24`, 128 values | gain, pan and send ramps; adds into the drum bus (tracks 0–3) or synth bus (4–7) and the FX1, FX2 and tape sends; peak | same as the DC blocker | ~2,950 with set-up | yes |
| · VU | `0x801183a4` | | always | ~20 | yes |
| Bus sum + soft clip | loop `0x80118d84` | drum + synth bus, `y = 1.5x − 0.5x³` | always | ~1,100 | yes |
| Window end, check | `0x80118dbe`–`0x80118e38`, `0x80119bd0` | see §2 | always | ~35 | — |
| Input (I/O) track | `0x80118e40`–`0x80119474` | LFO, input selector `0x80109ab0`, a 4-channel input mix, optional track filter, DC blocker, into the buses | unless byte `0x81059d50` is set | ~5,000–10,000 | no |
| Tape | `0x8012ce7a` (`FxManager::render`, track 10) → tape plug; filter, DC, VU; mixed back into the dry bus and both FX sends | rendered **before** FX1/FX2, so tape feeds the effects | always | ~4,000 + the tape plug | no |
| FX1, FX2 | `0x8012ce7a` (tracks 8, 9) → `renderFX`; LFO, filter `0x11a00e18`, DC `0x11a01548`, VU, add to the master bus | `renderFX` only if the record's active word (`+0xdc`) is set | ~3,700 each + the plug | no |
| Master | `0x8012ce7a` (track 11) → master plug (TE's `chorus`); filter, DC, bounce hook `0x80120d58`, compressor `0x11a01254` (~2,000), output gain + soft clip + peak `0x80119a42`, metronome click | always | ~7,000 + the plug | no |

So **per active instrument track the OS spends ~6,600 cycles** in the timed stage (filter on), ~4,200 when the track
filter is skipped (flag 0x04, or no voice sounding), ~1,000 once the track has been silent for 1.45 s; and **~350 per
sounding voice** around the engine call. With all 8 tracks active and 19 voices:
1,330 + 8 × 6,600 + 770 + 19 × 350 ≈ **61,500 cycles** [code, estimated floor; ~23 % of the 265,000 line].

None of our plugs sets flag 0x04 today (all 31 `plug.json` files), so every track pays for the OS filter. A drum
engine that has its own filter could set it and save ~2,400 cycles per block on its track, at the cost of the
track's filter knob [idea].

## 2. The overload rule, exactly

[code, `0x80118dbe`–`0x80118e38`, `0x80119bc4`–`0x80119c1a`; constants: u64 `290,000` at `0x8110f5fc`, u64 `265,000` at
`0x8110f604`, count byte `0x8110f5f8`; the app has the same rule at `0x53868`–`0x538a8`]

```text
elapsed = CYCLES(end) − CYCLES(start) − 8                 (64-bit)
if the "render song" byte 0x8110e51d is set: skip
if elapsed > 290,000:           kill one voice now          (count unchanged)
elif elapsed > 265,000:         count += 1; kill one voice if count >= 3
else:                           count = 0
```

- **How it measures:** the core cycle counter (wall-clock cycles at 400 MHz), read twice per block under CLI/STI. It
  therefore also counts anything that interrupts the stage: the core-timer tick (IVG6, `0x801322d4`) and any
  higher-priority system interrupt [inferred from how `CYCLES` works; the SEC priorities were not read].
- **How often:** every block, 689 times a second. After three blocks over 265,000 it kills one voice in **every**
  further block that stays over (the count is not reset by a kill).
- **Which voice** (`handleOverload` `0x8012d142`): of the 44 voice records (stride `0x17c`, both layers), skip free
  ones (`+0x10` = 0xff) and those with envelope stage (`+0x40`) below 4; take the smallest |level| (`+0x34`); log
  `cpu overload: kill notes %d` to `system.log`. If no voice is in release, fade or drum hold/decay, nothing happens.
- **The kill** (`Voice::killNote` `0x8012dfae`): stage 5, coefficient `0x7F000000`, step −level × `0x7FFFFF`/2³¹, gate
  bytes cleared, then the engine's `onNoteOff(note, state)`. Through `updateParametersADSR`'s stage 5 the level falls
  below `0x10624E` (−66 dB) in roughly 400–450 samples (~10 ms) [inferred from the stage-5 formula in
  [ahd-drum-notes-cpu.md](ahd-drum-notes-cpu.md#q-17--the-drum-envelope-updateparametersahd)]. Until then the voice still renders,
  so a kill frees its CPU only after ~7 blocks; meanwhile it is usually still the quietest candidate and is "killed"
  again each block (another `onNoteOff`, from the interrupt) [code for the calls; inferred that it repeats].
- **FX are never killed**, and FX cost never counts toward the rule [code].
- **What the user hears:** the quietest decaying notes and drum tails stop early with a short fade, one at a time.
  When nothing is decaying, an overloaded stage just runs long. If the *whole* block (timed stage plus FX, tape,
  master and everything else in the interrupt) passes 580,499 cycles, the next DMA interrupt is already pending:
  audio glitches and the tasks (UI, sequencer) starve [inferred]. The 265,000 line keeps the instrument stage under
  half the CPU, which is what leaves room for the rest (§5).

## 3. Voices

[code: tables `0x815C7ACC` (counts, default), `0x815C7AEC` (counts, `generous_chords`), `0x815C7B0C` (first record
per track); `getVoiceStartIndex`/`EndIndex` `0x8012d0ac`/`0x8012d1c4` add 22 for layer 1; the setting is read by
`SnakeSystem::loadSettings` (app `0x71958`, key string at app `0x8eaa3`, device string present)]

| Track | kick | snare | perc | sample | bass | lead | arp | chord | total |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Voices per layer | 2 | 2 | 2 | 2 | 2 | 3 | 2 | 4 (6) | **19 (21)** |

- **Layers:** `Sequencer::playNote` passes a `TrackLayer` (app `0x4c06c`): `triggerNote` (pattern playback) uses 0;
  live keys, step-press previews, `_PlayNote` and `MasterEcho` use 1 [code, app]. The device renders both layers every
  block (`0x8011894e`, `0x8011896e`) [code]. So the hard maximum is 38 (42) sounding voices; the realistic worst case is
  19 sequenced voices plus what the user plays live.
- **Allocation** inside a track (`0x8012d278`): a free record first, else the oldest released voice, else the
  oldest; note style mono uses the first record only; legato reuses the sounding voice without `onNoteOn`
  ([host-tables-flags.md](host-tables-flags.md#voicing-what-the-flags-do-not-do)).
- Drum engines only ever see the 8 drum-track voices, synth engines the 11 (13) synth-track voices; the budget below
  is shared by all of them.

## 4. The budget per voice

Per-voice budget (cycles per block) = (265,000 − OS) / V, with OS ≈ 1,330 + 770 + 6,600 · T + 350 · V for T active
instrument tracks and V sounding voices. Divide by 64 for cycles/sample. For a single block use 290,000.

| Sounding voices (all 8 tracks charged as active: conservative) | V | OS share | **Budget per voice** | per sample | single block |
|---|---:|---:|---:|---:|---:|
| 8 | 8 | 57,700 | 25,900 | 405 | 454 |
| 11 (e.g. the 4 synth tracks full) | 11 | 58,750 | 18,750 | 293 | 329 |
| 15 (e.g. synth tracks full, drums 1 voice each) | 15 | 60,150 | 13,660 | 213 | 239 |
| **Full: 4 synth tracks + 4 drum tracks, every voice sounding** (FX1/FX2 don't count) | **19** | **61,550** | **10,710** | **167** | **188** |
| Full with `generous_chords` | 21 | 62,250 | 9,650 | 151 | 170 |
| Full + 4 live chord notes on layer 1 | 23 | 62,950 | 8,780 | 137 | 154 |

**Example layouts with our engines** (steady means from the simulator run of 2026-10-23, `opz_cycles.py --all`):

| Layout | Engines | + OS | Timed stage | vs 265,000 |
|---|---:|---:|---:|---:|
| Runbook step 11: 11 × `macro` pulse P2 255 (11,836) + 8 × `kit` open hat (10,309) | 212,700 | 61,550 | **274,200** | **1.03×**: kills of decaying notes when sustained; under 290,000 |
| Same with `macro` P2 0 (7,872) | 169,100 | 61,550 | 230,700 | 0.87× |
| Mixed: bass `macro` P2 0 ×2, lead `super` ×3 (8,572), arp `fm2` ×2 (7,038), chord `table` ×4 (8,361); kick `kit` ×2 (13,216), snare `kit` snare tight ×2 (16,245), perc `hats` ×2 (8,414), sample `snap` ×2 (10,402) | 185,500 | 61,550 | **247,000** | 0.93× |
| 11 × `dx` BRASS (11,057) + 8 × `kit` open hat | 204,100 | 61,550 | 265,700 | 1.00× |
| TE for comparison: 19 × `analoga` (~9,500, §5) | ~180,500 | 61,550 | ~242,000 | 0.91× |

**Constants for `tools/opz_cycles.py`** (implemented): `OS_WINDOW_FIXED, OS_PER_TRACK, OS_BASS_EXTRA,
OS_PER_VOICE = 1_330, 6_600, 770, 350`, `OS_WINDOW_CYCLES` ≈ 61,550, `BUDGET_PER_SAMPLE` = (265,000 − OS) / 19 / 64
≈ 167.3 (steady), new `BUDGET_PER_SAMPLE_HARD` = (290,000 − OS) / 19 / 64 ≈ 187.9 (one block). The instrument verdict
is now "ok" when the worst block is ≤ 150 (unchanged), "tight" when the steady mean is ≤ 167 **and** the worst block
≤ 188, else "over budget", mirroring the OS's two lines. Why: the old 218 ignored a cost that is now counted (~23 % of
the line), and compared the worst block against the three-block line. Effect on the 2026-10-23 run: cases that move
from "tight" to "over budget" — `dx` BRASS/STRINGS (mean 168–173), `macro` square/pulse at P2 > 0 (196), `kit` hi-q
(194), `ensemble`/`super`/`sync`/`vowel` at their extreme top notes, first-block spikes of `mallet` (193–200),
`modal` (201–210), `swarm` knob sweeps (205) and one `8bit` case (213). Engine READMEs that quote "218" were left
alone (other sessions own them).

**How sure:** medium. The OS share is a floor from instruction counts; stalls (the OS code puts multiply results to use
at once), cache refills for voice records and engine code, and nested interrupts add to it. If the true share is
~80,000 the budget is ~152/sample; if TE's parallel bundles hide less than assumed it could be ~175. TE's own engines
land at 7,500–10,000 cycles per voice (§5), which fits 19 voices under the line with this OS share and would not fit
with a much larger one; that is the best offline evidence that the estimate is in the right place.

## 5. Cross-checks and the rest of the block

**TE's engines, same method** (issue slots per 64-frame block from the Ghidra listing of each export; [code],
estimated): `analoga` ~9,500 per voice; `sampleplayergain` ~7,600 (plus its sample-fetch loops); `cluster` ~9,000;
`ep` and `organ` 9,000–16,000 depending on which of their alternative 64-frame loops runs. Effects: `reverb` ~24,000,
`dist` ~30,000 (four 128-value passes), `fdnreverb` ~13,000, `chorus` (the master plug) ~7,500, `chorus2` ~4,800, `crush`
~4,100, `delay` ~2,200 plus cache misses on its delay line. TE's instruments thus sit at 120–155 cycles/sample, the
same place as the ABI's 150 target.

**The whole block** (580,499 cycles), full layout with the timed stage at the 265,000 line and two of our effects at
the 600-cycle target:

| Part | Cycles | |
|---|---:|---|
| Interrupt entry/exit, audio callback, bus clearing | ~4,500 | estimated |
| Timed instrument stage (OS + engines) | 265,000 | the line |
| Input, tape, FX1, FX2, master: OS part | ~28,000 | estimated |
| Tape plug (built into the OS, not counted) + master `chorus` | ~10,000 + ~7,500 | tape assumed |
| FX1 + FX2 plugs at 600 cycles/frame | 76,800 | target |
| **Total** | **~392,000 (68 %)** | leaves ~190,000 for other interrupts and every task |

So the binding constraint for instruments is the 265,000 line, not the block. Effects never trigger kills, but they
spend the same CPU: an effect over its 600 target is affordable alone, two heavy ones next to a full instrument stage
start to eat the tasks' time (UI lag, sequencer jitter) [inferred].

## 6. Q-22: what happens when an engine never returns

**A `render`/`renderFX` that never returns** (the audio interrupt, IVG11) [code unless marked]:

1. The **core-timer tick (IVG6) still runs**: it outranks IVG11. Its handler `0x801322d4` only calls the uC/OS tick
   (`0x80141ad4`) and acknowledges the timer (`TCNTL` `0x1fc03000` = 7); it checks nothing.
2. **No task runs again.** Task switches happen in IVG14 (`0x8013230a`), below IVG11, so `main_task` (UI, LEDs,
   saving, the PSoC messages), `sequencer_task`, USB, BLE and file tasks all stop.
3. **No watchdog.** WDOG0 is written only by the reboot function `0x80028076` ([crash-safety.md](crash-safety.md#1-what-the-os-does-on-a-fault));
   the listing has no reference to WDOG1 (`0x20047000`); the SEC fault reset is enabled only inside that reboot
   function. Nothing else resets the Blackfin.
4. **The power knob is a request.** The PSoC reads the knob and sends a power-mode message; the OS's handler only
   records it (`0x80131d08`: logs "PSOC requested SER_MSG_POWER_MODE_REQUEST_POWER_OFF", sets `0x81003e80` = 1,
   `0x81004750` = 0, `0x815c94ac` = 0x14) for `main_task` to act on. In a hang the OS never answers.
5. **Sound:** the SPORT DMA keeps reading the two output halves that nobody refills, so the last two blocks repeat:
   a ~2.9 ms loop, a buzz near 344 Hz, or silence if the hang came at a quiet moment [inferred: the DMA's
   ping-pong descriptors were not read].

**A hang in `onNoteOn` or `init`** (task context): audio keeps going, since `render` runs in the interrupt; the task
that called it stops (pattern playback for `sequencer_task`, the UI and power messages for `main_task`), and lower
priority tasks starve if it spins [inferred from uC/OS scheduling]. **A hang in `initModule` at boot** stops boot
before audio starts; update mode stays reachable through the bootloader ([crash-safety.md](crash-safety.md)).

**Other reset paths:** none on the Blackfin. "blackfin reset was requested" in `bootloader.ldr` is a command handler
inside the *bootloader* (`block8::0x11a03212`: log, then reboot with reason 0x14 or 10 depending on a byte of the
message) [code], so it needs a running bootloader. **Whether the PSoC cuts power or resets the Blackfin on its own**
when the OS stops answering is in the PSoC firmware (`decrypted/firmware/psoc_keyboard/bin/*.cyacd`: a Cortex-M0 image,
application at `0x1900`, which uses its own watchdog block at `0x400B02xx` [code, literal-pool references]); not
analysed: **Q-33**. Indirect evidence: on 2026-10-19 `system.log` stops at 21:56:00 without a power-off line and the
OP-Z booted again at 21:57:18 with no crash log (the user's `system.log`, read by the crash-safety session on
2026-10-20; not in git, see Q-24), so it came back without the OS's cooperation [inferred]. What the user did then (turned the knob? how long?) would settle most of Q-33.

## 7. Hardware check H-C1 (run 2026-11-01: A, B, C)

Goal: measure the OS's share of the timed stage and the overload line on the device, with no risk of a hang.

**Built 2026-10-27:** engines/opz_test_load (slot 67, id 60907) implements
step 1 with a tone code decoded by `tools/opz_loadtest.py`, and adds a run C for Q-46 (uC/OS-III's idle counter at
`0x81AEEAFC`, read once per block) and an overload-kill counter for run B. The README holds the device procedure.

1. **A test engine `opz_test_load`** (to be written by an engine session; instrument, all eight tracks in
   `trackmask`, 5 KB slot, flags 0). `render` reads `CYCLES` on entry and exit and keeps, in one static block shared
   by all voices, the entry/exit stamps of every call of the current block. From them it gets: the gap between the
   last voice of track k and the first voice of track k + 1 (the OS's per-track work), the span from the first to
   the last call (the timed stage minus its first and last parts), and its own cost. It outputs a quiet sine whose
   pitch encodes one number (e.g. track 0 plays the median per-track gap / 10 as Hz, held 2 s), or a beep code;
   the user records the line out and we decode offline, as with the 2026-10-17 recordings. P1 adds a bounded busy
   loop of 0–16,000 cycles per call in steps of 500 (bounded so the whole block stays below ~70 %); P2 picks which
   number is reported.
2. **Run A, the share:** P1 = 0; one held note on each of the 8 instrument tracks (a pattern with long notes),
   then 2 notes per track. Record 10 s each. Expected: per-track gap ~6,600 (track filter on), per-voice overhead
   ~350.
3. **Run B, the line:** chord track with 4 long-release notes repeating, others as in A; raise P1 one step per bar
   until the release tails audibly shorten. Then switch off, update mode (hold SCREEN), copy `systeminfo/`
   read-only (as H-10), and look for "cpu overload: kill notes" lines in `system.log`. The step where they start gives
   265,000 = OS + V × (engine + busy), a second unknown-free check of the share.
4. **Safety:** the busy loop is capped; the overload only fades decaying notes; nothing persists across a restart
   (no boot file). Do not install it on an FX track. Roll back by switching the tracks to their old plugs.

### H-C1 run A/C results (2026-11-01, the user's OP-Z, firmware 1.2.45)

Set-up: `opz_test_load` (60907) in slot 10 of all eight instrument tracks; project 9 patterns 4–16 built on a copy
of the user's pattern 1 (so they carried the in-use bit and appeared at all; see [patterns.md §9](patterns.md),
Q-50 answered 2026-11-13), each pattern one P2 zone; one pattern looped at a
time at 40 BPM, its tone recorded from USB audio for 10 s (`tools/opz_freqtrack.py`, git-ignored
`device-tests/20261101T17*-freqtrack-hc1-p*`), the steady median decoded with the engine's table. FX1/FX2 played
`echo`/`plate` (pattern 1's), sends 0. Every tone was steady to ±1–3 Hz over 8 s [proven].

| Pattern | Zone | Voices | Tone | Value | Offline estimate |
|---|---|---|---|---|---|
| 4 | 0 stage span | 8 (1 per track) | 838 Hz | **63,800 cycles/block** [proven] | — |
| 5 | 1 OS work between tracks | 8 | 2,270 Hz | **8,280 cycles** [proven] | ~6,600 |
| 6 | 2 OS work between voices of a track | 8 | 200 Hz | 0 (one voice per track: nothing to measure) | — |
| 7 | 3 engine cost per call (P1 0) | 8 | 285 Hz | **850 cycles** [proven] | ~1,400 (simulator) |
| 8 | 4 idle-task loops per block | 8 | 200 Hz | **0**: see below | unknown |
| 9 | 5 render calls per block | 8 | 1,000 Hz | **8** [proven] (confirms the zone mapping) | 8 |
| 10 | 6 block period | 8 | 3,102.8 Hz | **580,566 cycles** [proven] (±60 by tone resolution) | 580,499 (400 MHz, 64 frames) |
| 11 | 0 stage span | 18 notes written | 879 Hz | **67,900 cycles** [proven] | — |
| 12 | 1 OS work between tracks | 18 written | 2,270 Hz | **8,280 cycles** [proven] | ~6,600 |
| 13 | 2 OS work between voices of a track | 18 written | 554 Hz | **354 cycles** [proven] | ~350 |
| 14 | 5 render calls per block | 18 written | 1,800 Hz | **16** [proven]: two written notes get no voice (lead and arp one at a time? [inferred]) | 18 |
| 15 | 4 idle loops, P1 128 | 8 | 200 Hz | 0 | — |
| 16 | 4 idle loops, P1 255 | 8 | 200 Hz | 0 | — |

What it means:

- **Clock and block confirmed**: 580,566 ≈ 580,499 cycles per 64-frame block, so CCLK 400 MHz and 64 frames
  [proven] (Q-04's code answer holds on the device).
- **The OS's per-track work is 8,280 cycles, ~25 % above the instruction count (6,600)** [proven]; the per-voice work
  (354) matches the count. Q-34: about 1,700 cycles per track land in the timed stage beyond the counted
  instructions (interrupts, cache refills or code not counted) [inferred for the cause].
- **The stage span is mostly OS**: with 8 voices, 7 × 8,280 = 58,000 of the 63,800 cycles are track gaps and
  8 × 850 = 6,800 are the engine [arithmetic on proven values].
- **Budget, recomputed with the measured OS share** [inferred]: 265,000 − 7 × 8,280 − 11 × 354 ≈ 203,000 cycles for 19
  voices ≈ **167 cycles/sample/voice**, the figure §4 estimated. It does not include OS work before the first and after
  the last voice of the stage, which the span does not see.
- **Open:** pattern 11's span rose only ~4,100 cycles for 8 more render calls, where 8 × (850 + 354) ≈ 9,600 was
  expected. The busy-loop steps and run B (the overload line) were not run yet.
- **The OP-Z hung under pattern 16's load** [proven, user]: P1 255 (busy loop 15,500 cycles per call) on all eight
  voices, one per track, FX1 `echo` and FX2 `plate` running (sends 0), left playing for several minutes after its
  10 s recording. First symptom: the USB file service stopped answering (`no $34 reply` twice, ports still listed);
  the user found the unit unresponsive while playing and power-cycled it; it then worked normally. Estimated
  timed stage ≈ 58,000 + 8 × (850 + 15,500) ≈ 190,000 cycles — **below the 265,000 overload line**, so the overload rule
  never acted [inferred]. Most likely the audio work (stage + FX + mixer) left too little of the 580,499-cycle block for
  the OS's other tasks, which then starved [inferred; Q-46, Q-33]. The README's "≈ 61 % of the block" safety estimate
  for the busy loop does not hold with our FX running. Pattern 15 (P1 128, busy 8,000) and the 10 s recording of
  pattern 16 itself showed no problem.
- **Spare CPU (Q-46) not measured**: the idle-task counter the engine reads (`0x81AEEAFC`) did not move in any case,
  with P1 0, 128 or 255 [proven]. Either the address is not the counter on this build, or the idle task never runs
  during playback [inferred; both open]. Zone 4 gives no information until that is resolved offline.

### H-C1 run B results (2026-11-01): the overload line

Set-up as run A, rebuilt on pattern 1 (`build/hc1/hb09.opz`, written with project 10 open): bass … arp two long notes
each, kick two, chord four one-step notes on every second step with release 200 (tails pile up), 40 BPM; P2 zone 8
(overload kills per second, counted as `onNoteOff` on a voice already in its tail) on patterns 4–15 with P1 rising,
zone 0 on pattern 16. Each pattern played alone for 10 s, stopped before the next; files moved only while stopped.

| Pattern | Busy loop per call | Kills per second (mean of 21 half-second windows) |
|---|---|---|
| 4 | 0 | ≈ 6 (2–10, even counts only) |
| 5–7 | 8,500–10,500 | ≈ 6, even counts |
| 8–10 | 11,000–12,000 | ≈ 6; single odd counts in ~¼ of the windows |
| 11 | 12,500 | ≈ 6, even counts only |
| 12 | 13,000 | ≈ 6.8; odd counts in 9 of 21 windows |
| 13 | 14,000 | ≈ 8.5 (peaks 16–17) |
| 14 | 15,000 | ≈ 8.2 |
| 15 | 15,500 | ≈ 8.0 |
| 16 (zone 0) | 12,000 | stage span 204,000–230,000 cycles per block (varies with chord's voices) |

[proven, recordings `device-tests/20261101T1*-freqtrack-hc1-pB*`] What it means:

- **The zone-8 counter also counts voice stealing**: with no load, chord's retriggers (4 notes every 0.75 s) end
  older tails ≈ 6 times per second [proven baseline; cause inferred]. Overload shows as the rise above that.
- **The line: tails start to be cut by load between a busy loop of 12,500 and 13,000 cycles per call** (~16 voices),
  where the stage averages ≈ 220,000–246,000 cycles per block (pattern 16 + 16 × 1,000) [inferred from the measured
  span at 12,000]; single blocks pass 265,000 there, which the OS rule (265,000 three blocks running, 290,000 once)
  turns into kills [code rule; consistent]. The rate then levels off at ≈ 8 per second, most likely because only a few
  chord tails exist at any time to kill [inferred].
- **No hang** in run B (each pattern ≤ ~30 s, sequencer stopped between patterns, no file transfer while playing),
  although patterns 13–15 load the stage far more than run A's pattern 16, which hung after minutes of play with a
  file transfer attempted [proven both]. Whether the hang came from the long sustained load, from the USB file
  service under load, or both is open (Q-46).
- Not done: the update-mode `system.log` check for "cpu overload: kill notes" lines (§7 step 3).

## 8. Open, and follow-ups

- **Q-33** (new): does the PSoC cut power or reset the Blackfin when the OS stops answering? Needed before any test
  that could hang the unit; first, ask the user what they did on 2026-10-19 at 21:56.
- **Q-34** (new): does anything else land inside the timed stage on the device — the timer tick, USB or MIDI
  interrupts at higher SEC priority — and how much? H-C1 run A measures the total.
- Engine authors: `onNoteOff` can arrive from the overload killer inside the audio interrupt, between two `render`
  calls, and repeatedly for the same voice ([engine-abi.md §6](../sdk/docs/engine-abi.md#6-rules-for-engine-authors) rule
  20). `tools/opz_crashcheck.py` could add a "repeated `onNoteOff` between renders" pass.
- [idea] Our engines are built for the classic Blackfin ISA (`-mcpu=bf592`); TE's code uses the BF70x's single
  32 × 32 multiply (`Mult32` in the listings) where ours needs six instructions. Hand-encoded Blackfin+ multiplies in
  the inner loops would cut our engines' cost, if the GNU toolchain can be taught to emit them. **Measured
  2026-10-27:** multiply-family instructions are 9–46 % of our render paths, so the saving is 7–35 % per engine, not
  "most" (Q-47).
- How to get a bigger budget at all (pool use, voice caps, flag 0x04, L2, raising the line by a firmware patch):
  [cpu-headroom.md](cpu-headroom.md).
- Not read: the SEC priority of the SPORT interrupt, the DMA descriptor mode, the tape plug's cost.
