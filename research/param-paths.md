# Parameter paths: what reaches an engine, in what range, and when (Q-23)

Offline research, 2026-10-23, OS 1.2.45 (`opz.ldr`) and the desktop app's `SnakePlugin` (arm64, with symbols); no
command was sent to the OP-Z. Addresses: `0x80…`/`0x11a0…` are device code in `opz.ldr`, `0x0…` (six hex digits)
are app functions. Paraphrased, no TE code. Model and simulator cases: [`tools/opz_params.py`](../tools/opz_params.py).
Engine rules that follow from it: [engine-abi.md §4 and §6](../sdk/docs/engine-abi.md#4-parameters) (rules 20–22).

## Answer to Q-23

**No.** Every `params` word the OS hands an engine (instrument `render`/`onNoteOn`, effect `renderFX`) is in
**0 … 0x7FFFFFFF** [code]. Every source sets a parameter *byte*; bytes become words `b·0x808080 + (b >> 1)`
(≤ 0x7FFFFFFF); the per-block smoother only moves between two such words; the track LFO saturates its sum and then
clamps it at 0. So P1/P2 can neither go negative nor wrap. What they can do: take **any** value in that range (the
smoother and the LFO leave the byte lattice), reach exactly 0 and 0x7FFFFFFF, and **jump across the whole range from
one block to the next** (a note-on latches all 18 words unsmoothed; the LFO's fastest free rate is 8 blocks per
cycle). Within one `render`/`renderFX` call the words never change.

`pitch` is different: it is a `uint32_t` that can exceed 0x7FFFFFFF (up to ~0xB66E2BE5 with the tuning at +12
semitones), and `pitchSlope` wraps with it. The FX `pitchBend` spans the whole `int32_t` range, 0x80000000 included.

## 1. The path of a parameter word

```text
knob / MIDI CC / step lock / preset / $0E / punch-in / arpeggio / step component
        │  a byte 0..255 per track and parameter (sequencer +0x3460d0 in the app: 16 tracks × 18 bytes)
        ▼
sequencer tick (96 per beat; runs while stopped)          Synth::setTargetParameters 0x052820 · opz.ldr 0x80117808
        │  target word = b·0x808080 + (b >> 1); two layers per track (L1 0x11900004, +0x480 layer 1)
        ▼
every audio block, before rendering                       interpolateParameters 0x0529c8 · opz.ldr 0x801183d8
        │  params[0..11]: w ← 63·(w >> 6) + (t >> 6) + (t >> 25)     params[12..17]: = target (no smoothing)
        │  smoothed words at L1 0x11900904 + track·0x48 (+0x480 layer 1)
        │      ▲
        │      └── note-on: all 18 bytes written here directly    latchParameters 0x052ab4 · opz.ldr 0x801179d8
        ▼                                                         (called from playNote 0x04c06c · 0x8010ca1a)
per track, per block: the track LFO                       Lfo::render 0x042fb0 · opz.ldr 0x8012d994
        │  copies the 18 words + a pitch word (0x3FFFFFFF) to L2 0x0800B8AC (layer 1: 0x0800B8F8),
        │  rewrites ONE destination word, clamps it to 0..0x7FFFFFFF
        ▼
render(out, pitch, pitchSlope, state, params = 0x0800B8AC, env)   for each voice of the track
                                                          InstrumentManager::render 0x03be60 · opz.ldr 0x8012d204
```

Device Synth render: `0x801184c0` (loop over the instrument tracks; LFO calls at `0x8011885c`/`0x801188a0`, voice
render at `0x8011894e`/`0x8011896e`); FX1/FX2 get the same LFO (`0x801196aa`, `0x801197ae`) before `renderFX`; the tape
track's effect gets its smoothed words without an LFO (`0x80119536`). The tables the device uses are the app's: the
LFO's target groups (`0x815C7BA4`), destinations (`0x815C7BC4`) and shapes (`0x815C7BE4`) match the app's
`shapeGroups`/`destinations`/`shapes` word for word, and the MIDI CC map and parameter→CC tables are in `opz.ldr` too
[code].

### Smoothing (params 0–11)

`w ← 63·(w >> 6) + (t >> 6) + (t >> 25)` once per block while `w ≠ t` [code, `0x801183d8`; app `0x0529c8`]. This is
a 1/64 one-pole (63 % in 64 blocks ≈ 93 ms; most of the way in ~0.7 s). Checked exhaustively over all 65,536
(start, target) byte pairs [offline, `opz_params.smooth_step`]: it **never** leaves [min(start, target),
max(start, target) + 1], never exceeds 0x7FFFFFFF, and settles up to ~4,000 LSB *below* most targets (no overshoot
beyond 1 LSB). Only P1, P2, ADSR, sends, filter, resonance, pan and level are smoothed; **portamento, LFO depth/speed/
target/shape and note style (12–17) jump** to their new byte at the tick. Tracks 12–15 get all 18 unsmoothed.

### Note-on latch

`Sequencer::playNote` copies the step's bytes (its parameter locks, else the track's current bytes) and calls
`latchParameters`, which writes all 18 words of that track and layer **straight into the smoothed array**
[code, app `0x052ab4` from `0x04c308`; device `0x801179d8` from `0x8010ca1a`]. So at every note, P1/P2 (and the rest)
jump to the step's value with no smoothing, for **every voice of the track**, including voices already sounding.
The latch runs in the sequencer task without a lock; the audio interrupt can preempt it, so a block can see some words
latched and others not [code: no masking in `0x801179d8`; effect inferred]. Each word is still in range.

### The track LFO

Per track and block [code, `0x8012d994`; app `0x042fb0`]:

| | |
|---|---|
| Destination | LFO target byte `>> 5`: P1, P2, filter, resonance, attack, **pitch** (an extra word, not in `params`), pan, level |
| Shapes | shape byte → 12 user shapes (`(word >> 16) · 3 >> 13`): free sine, triangle, clipped sine ("square"), ramp, random (sample and hold), **tilt** (the accelerometer); then the same five note-triggered plus a one-shot ramp. P1/P2/filter/resonance/attack/level get the unipolar forms, pitch/pan the bipolar ones (pitch: a true square) |
| Depth | bipolar around 128: bytes 124–130 do nothing; up to 255 the amount rises to 0.977, squared 0.954 of the distance to 0x7FFFFFFF; down to 0 it is 0.969, squared 0.938 of the distance to 0 (unipolar shapes); bipolar shapes add `±amount² · wave` (pitch: half the amount) |
| Output | `max(0, saturate(base + modulation))`: **0 … 0x7FFFFFFF** |
| Speed | bytes 0–127: synced, `(256 … 2) · BPM · 6464` per block in 8 steps (1/64 note … 2 bars); 128–255: free, `2·x⁴ + 100,000` with `x = (word − 0x3FFFFFFF) >> 23`: 62 s per cycle at 128, **8 blocks (86 Hz) at 255** |
| When | once per block, from the block's smoothed words; sample and hold at block rate |

The **arp track** (track 6) has no LFO: its third page drives the arpeggiator. The device copies only words 0–13 of
its own track into the buffer, so `params[14..17]` on the arp track are **the previous track's (lead's) words**
[code, `0x80119cd4`; app `Synth::render` copies `0x38` bytes]. In range, but not the arp track's own LFO speed,
target, shape and note style. The arpeggiator itself moves one parameter byte per arp step by an offset, clamped
0..255 [code, app `applyArpeggioParameters` `0x0746b8`].

## 2. Every source, its range and timing

| Source | Reaches the engine as | Range | When it changes | Evidence |
|---|---|---|---|---|
| Knob turn (encoder) | byte delta, clamped 0..255 → target → smoothed `params` (0–11) or jump (12–17) | 0..0x7FFFFFFF on the lattice, smoothed in between | byte at once (task), target at the next tick, word every block | app `setTrackParameterDelta` `0x03ea70`, `setStepParameterDelta` `0x03e954`; device above |
| Plug flags 0x08/0x10 | nothing: a centre-detent LED cue at bytes 124–132 | — | — | app `DefaultContext::onCC` `0x075aa8` |
| Smoothing | params 0–11 between two lattice words | ≤ max + 1 LSB, never wraps | every block | `0x801183d8` |
| Track LFO | one destination word (P1, P2, filter, res, attack, pan or level), or the pitch | 0..0x7FFFFFFF, any value | every block | `0x8012d994` |
| Step parameter locks | the step's bytes, latched at its note | lattice | at the note (jump), then target at each tick | `0x801179d8`; app `applyCurrentParameters` `0x04b9e0` |
| Preset (track + white key; preset 0 when a track switches plug) | bytes → current bytes | lattice | target at the next tick, smoothed (or latched at the next note) | [presets.md](presets.md) |
| `$0E` (track + 18 bytes) | bytes → current bytes | lattice | as presets [inferred: same path] | [presets.md](presets.md) |
| MIDI CC in (track's channel) | CC 1–18 **absolute** (value `v` → byte `2v + (v & 1)`, 7-bit), CC 32–49 **relative** (−64…+63 per message), in the order P1, P2, filter, resonance, A, D, S, R, LFO depth, speed, target, shape, FX1, FX2, pan, level, portamento, note style. **CC 1 = mod wheel = P1** | lattice (128 values) | as a knob | app `Sequencer::cc` `0x04fc60`, `g_incomingCCToParameter`; the same table in `opz.ldr` |
| MIDI pitch bend | instruments: `pitch` (±200 cents); master channel: all 8 instrument tracks; FX channels: `renderFX`'s `pitchBend` | bend word: full `int32_t`, **0x80000000 at MIDI bend 0** | next block | app `Sequencer::pitchBend` `0x04f948`, `Synth::pitchBend` `0x0527dc` |
| MIDI aftertouch (poly, channel) | nothing | — | — | app `Sequencer::handleMidiMessage` `0x07ba34` ignores 0xA0 and 0xD0 |
| MIDI notes | `onNoteOn`/`onNoteOff` as from the keys (no channel, no MIDI timing) | note 0–127, velocity 1–127 | at once (task) | `0x07ba34` |
| MIDI clock, start/stop | tempo and transport: sequencer keys 4–7 | — | — | [engine-abi.md, sequencer keys](../sdk/docs/engine-abi.md#sequencer-keys) |
| Portamento (`params[12]`) | the voice's glide towards the target pitch, in `pitch`/`pitchSlope` | rate 32 (byte 255: 2 s for any interval) … 1,663 (byte 1: 38 ms); byte 0 = jump | every block | `0x8012e324`; app `Voice::renderVoice` `0x070dd0` |
| Pitch bend, vibrato | `pitch` | bend ±200 cents; LFO pitch ±191 cents at full depth | every block | `0x8012e368` (detune `0x8011aa1c`) |
| Global tuning | `pitch` (and the drum unity rate) | ±1,200 cents in 1-cent steps | when changed | app `TuningContext::onCC` `0x077d94`, `initTuning` `0x028778` |
| Arpeggiator | notes, plus one parameter byte per step ± an offset | lattice (clamped) | per arp step (latched at its note) | app `0x0746b8` |
| Step components ("note styles") | pitch glide: the portamento byte per step; pitch ramp on drum tracks: **P1** per step (byte, wraps mod 256); velocity: 8–99 or a table; parameter sweep: a byte ramp per tick | bytes | per step / tick | app `PitchGlide` `0x0428f8`, `PitchRampUp` `0x04279c`, `Velocity` `0x0426fc`, `ParameterSweep` `0x042998`/`0x041948` |
| Punch-in effects (master effects) | override parameter bytes of tracks while held | bytes | per tick | app `MasterEffects::overrideParameters` `0x040b04`, `ApplyParameterSweeps` `0x041cac` |
| Tape-track effects | not in an instrument's or FX1/FX2's words (the tape plug's own notes and words; no LFO on tape) | — | — | `0x80119536` [inferred beyond the call] |
| `onNoteOn` arguments | note and velocity zero-extended **bytes** (0–255); `params` = the note path's words (instruments: never NULL as far as traced; FX re-send: NULL, §3 of the ABI) | | | `0x8012d47e`, `0x8012e280` |

Mid-block changes: none. `render`/`renderFX` run in the audio interrupt; every source above runs in a task (or is
computed in the same interrupt before the call), and the engine reads a per-track copy filled just before its
voices render [code; that no other interrupt writes it: inferred]. The copy is **reused by the next track**: an engine
must read `params` during the call and never keep the pointer.

### `pitch` and `pitchSlope`

`Voice::render` (`0x8012e2c0`; app `0x070dd0`): target = note frequency × tuning (`0x8012e058`: the 128-entry
frequency table at `0x815C808C`, **no bound check on the note**), drum tracks `1024 ×` tuning; portamento moves `cur` towards it
(`step = (target − start) / 44,100` set at note-on, × rate per block, stopped at the target with *signed* compares);
then `pitch_end = detune(detune(cur, bend · 200 / 2³¹ cents), vibrato · 400 / 2³¹ cents)`, and the engine is called
with `pitch = ` last block's `pitch_end` and `pitchSlope = (pitch_end − pitch) >>> 6` (arithmetic).

| | lowest | highest |
|---|---|---|
| Standard tuning, note 0 / 127, bend and vibrato at full | `0x9B18F` (5.3 Hz) | `0x5B4493E7` (15.7 kHz) |
| Tuning ±12 semitones as well | `0x4D8C7` (2.6 Hz) | `0xB66E2BE5` (31 kHz: **above Nyquist and above 0x7FFFFFFF**) |
| Drum tracks (`1024 ×` tuning, bend, vibrato) | ~408 | ~2,565 |

- **A pitch jump arrives as one block's slope**: legato without portamento, or a note jump on a voice, gives
  `pitch` = the old pitch and `pitchSlope` = (new − old) / 64. Across 0x80000000 the difference wraps: going *up*
  from note 0 to the top word gives a *negative* `int32_t` slope. `pitch + 64 · pitchSlope` in `uint32_t` arithmetic
  is right; treating `pitch` as `int32_t`, or comparing it signed, is not.
- **Drum unity rate: 1024, not 2048.** Both the app (`getTargetFrequency` `0x070f84`: `0x400 · cents >> 30`, cents
  `0x40000000` at the centre) and the device (`0x8012e058`: `Mult32 0x400 · 0x40000000`, then `<< 1`) give 1024
  at standard tuning [code]. engine-abi.md said ≈2048, and opzsim and the cycle driver pass 2048; the hardware check
  below settles it. Engines that use the drum `pitch` only as "about unity" are unaffected.
- Notes above 127 would read past the table (garbage pitch); whether any path makes one (transpose, octave, arp
  offsets) is open (Q-35). MIDI notes are 0–127.
- Above 0x7FFFFFFF the OS's own arithmetic is wrong too (signed multiplies in detune and signed glide compares), so
  such words are rare but real: only with the tuning far up and the top notes.

## 3. MIDI an engine can use, and what the OP-Z sends

An engine has **no MIDI access of its own** [code: no callback for it, §5 of the ABI]. MIDI reaches it only through
the OS: notes → `onNoteOn`/`onNoteOff` (track = MIDI channel per `midi.json`, no channel or timestamp passed), CC 1–18 /
32–49 → parameter bytes (7-bit), pitch bend → `pitch` / `pitchBend`, clock and transport → sequencer keys 4–7.
Aftertouch is dropped. So an engine can be "MIDI-learned" only through those 18 parameters: CC 1 (mod wheel) and CC 2
are P1 and P2 on the track's channel.

**MIDI out** [code, app `Midi::AddParameterCC` `0x037ec0` from `Sequencer::tick` `0x0497c0`; `AddNoteOn` `0x037d94`
from `playNote`; `AddPitchBend` `0x038154`]: every tick, for each track and each parameter whose `byte >> 1` changed
since the last tick, a CC with the 7-bit value, on the track's channel, with the CC number from a per-track table
(`g_defaultParameterToCC`, `midi.json`); **16 of the 18 parameters** (not portamento and note style); also note-ons
from the sequencer and keys, pitch bend, clock (24 PPQN), program change and song select. The LFO, the smoothing and
the latch happen later, in the audio path, and are **not** sent. So a web app with Web MIDI can record an engine's
parameter moves (knobs, locks, presets, `$0E`) at 7 bits and tick resolution, but not what the LFO does, and only
when the OP-Z's MIDI settings send CCs (defaults on the device: Q-37).

## 4. What engines must do (rules 20–22 of engine-abi.md §6)

1. Accept every `params` word in 0..0x7FFFFFFF, not only `b·0x808080 + (b >> 1)`, and any jump between blocks
   (0 → 0x7FFFFFFF in one block). Derive table indices and loop counts with shifts (`>> 24`, `>> 27`) or explicit
   clamps; no division by a word that can be 0. Masking with `& 0x7FFFFFFF` stays harmless and protects against a
   future OS. Don't keep the `params` pointer.
2. Treat `pitch` as `uint32_t` up to ~0xB7000000, `pitchSlope` as a wrapping `int32_t`; clamp anything derived from
   the pitch (delay lengths, filter coefficients, oversampling counts, note-dependent tables) to a sane range;
   expect a full-range jump in one block's slope. Drum tracks: `pitch` 408..2,565.
3. FX: `pitchBend` is any `int32_t`, `INT32_MIN` included (negating it overflows). `onNoteOn` notes and velocities
   are 0–255 in the type; MIDI and the keys give 0–127.
4. On the arp track, `params[14..17]` are not the arp track's own words.

## 5. Simulator cases and results (2026-10-23, main at `a30166b`)

`tools/opz_params.py` models the smoother, the LFO (all waves, depths, rates), the portamento and the pitch range,
and builds per-block word streams for the cycle driver (`BenchCase.stream`, 18 params + the pitch at the block's
start and end, or the FX pitch bend). `opz_crashcheck.py` now plays them on every engine (with the register, guard,
NULL-page and torn-state checks); `opz_params.py --all` also compares each stream's CPU and output level with the
engine's own benchmark cases.

| Stream (instruments: 4-voice chord or 4 drum keys; FX: one FX track) | |
|---|---|
| LFO on P1 from 0, depth 255, speed 255, each free shape | biggest per-block P1 swings the LFO can make |
| LFO on P2 at 255, depth 0 (full negative), speed 255, each free shape | the downward side |
| LFO on P1, tilt, the unit tilted through its full range | bipolar: saturates at 0 and 0x7FFFFFFF |
| LFO on attack, note-triggered shapes | `params[2]` into `updateParametersADSR` |
| every word at its extremes (0, 1, 0x7FFFFFFE, 0x7FFFFFFF, around 0x40000000), changing every block | all 18 |
| P1/P2 jumping 0 ↔ 0x7FFFFFFF every block, a new note every 2 blocks | note-on latches |
| step locks on all 18 parameters, a new step every 2 blocks | latches + smoothing + jumps of 12–17 |
| knob flicks through the OS smoother | off-lattice words |
| instruments: LFO on pitch; pitch jumps note 0 ↔ 127 at full bend and vibrato, standard tuning and ±12 semitones; fastest portamento 0 ↔ 127, both tunings | `pitch`/`pitchSlope` extremes |
| drums: LFO on pitch at the unity rate | |
| FX: pitch bend 0x80000000 / 0x7FFFFFFF / 0 every block | |

**Results on all 31 engines (quick and `--full`):** no crash, no exception, no write outside an engine's memory, no
register or stack finding, in any stream (`opz_crashcheck.py --all`: 0 errors; the one known note is `resonator`'s
harmless LC1). `macro` included: the streams don't reproduce its device crash. What misbehaves, against each engine's
own benchmark worst case (not fixed here; engines belong to their owners):

| Engine | Stream | What | How bad |
|---|---|---|---|
| `pluck` | pitch jumps to the top word (note 127, bend +2, vibrato +1.9: ~15.7 kHz), both tunings | the output **saturates at 0 dBFS for ~4 blocks** after the jump; the benchmark's loudest case is −1.4 dBFS (C8 held is fine) | audible click/buzz at the very top of the range |
| `macro` | pitch jumps and fastest portamento to the top | worst block **362–533 cycles/sample** (benchmark worst 220) | CPU, only with top-octave pitches |
| `super` | the same | **351–375** (benchmark worst 200–203) | CPU, top octave |
| `vowel` | pitch jumps / portamento, standard tuning | 236–237 (benchmark worst 196): just over the 218 budget line | marginal |
| `pd` | fastest portamento at +12 semitones tuning | 230–233 (benchmark worst 158) | marginal, extreme tuning only |
| `dx` | anything that changes P1 every block (LFO, jumps, locks, flicks) | 219.5–219.7 (benchmark worst 179; budget line 218) | marginal |
| `plate` | knob flicks through the smoother (quick lists only) | 605 cycles/sample (FX target 600, quick benchmark worst 528) | marginal; not flagged against the full benchmark |

The budget line is the soft overload limit shared by 19 voices (265,000 cycles / 19 / 64 ≈ 218 per sample): a few
voices above it are fine on their own; it matters when many voices hit the expensive path in the same block.
Louder FX outputs in the "extremes" stream (`clouds`, `plate`, `resonator`, `shimmer`: up to the −11.5 dBFS input)
are the dry signal at full level and pan, not a fault.

**opzsim does not model this path yet:** it smooths all 18 parameters with `cur += (target − cur) >> 6`, has no LFO,
no note-on latch and no portamento, and passes 2048 on drum tracks. Bringing it to the OS's model would change the
golden hashes of every engine whose scenarios move parameters; left as a follow-up.

## 6. Hardware check H-23: `engines/opz_test_param`

**Run 2026-11-01: passed** (result below). An instrument for any instrument track (plug 60904, 5 KB slot 69; was 72 until 2026-10-27) with two modes on P2 and an alarm:

- **P2 0–127, P1 meter:** a sine at 220 Hz (P1 word 0) … 880 Hz (0x7FFFFFFF), linear in Hz.
- **P2 128–255, pitch readout:** a sine at 440 Hz × `pitch` / 1024: on a drum track A4 (440 Hz) means the code's
  1024, A5 (880 Hz) the old 2048.
- **Alarm:** if any of the 18 words ever has bit 31 set, the voice plays a 2 kHz square, 50 ms on / 50 ms off, until
  its next note-on. Q-23 predicts it never sounds.

Steps (the README has the commands; `--fake`, then `--dry-run`, then for real with the automatic backup; normal mode,
TE's app closed, a disposable project, volume low):

1. Install on the lead track (`opz.py install engines/opz_test_param --track lead --slot 10`), select it (hold track + its
   black key). P2 at 0. Hold a note and turn P1: the tone glides 220 → 880 Hz (smoothed: it lags a fast turn).
2. LFO page: depth 255, speed 255 (fastest), destination P1, then each free shape. Expect a fast warble from the P1
   setting up towards 880 Hz (P1 at 0: 220 → ~850 Hz), clipped-sine ≈ square, ramp, random. **No 2 kHz alarm.**
   Then depth 0 (full negative) with P1 at 255: the warble goes down towards 220 Hz. Tilt shape: tilt the OP-Z both
   ways. Still no alarm.
3. Step locks: P1 at 0 on step 1 and at 255 on step 3, notes on both. Expect the tone to **jump** at each note (no
   glide): the note-on latch. Between notes with no lock it glides back.
4. Optional, with a MIDI controller on the lead's channel: the mod wheel (CC 1) moves the tone like P1.
5. Optional, pitch: P2 255 on the lead track: the readout clamps at 5.5 kHz (synth pitches are not unity) — ignore.
6. Move it to the **kick** track (it has every instrument track in its mask), P2 at 255, play a key at octave 0:
   **A4 or A5?** Compare with a tuner or a synth track playing A4. A4 = drum `pitch` 1024 (code), A5 = 2048.
7. Remove: `opz.py remove 60904 --unassign`.

Report: alarm heard (when, which shape), the warble's range and speed, jump vs glide at locked notes, A4 or A5.

### Result (2026-11-01, the user's OP-Z, firmware 1.2.45)

Measured from the OP-Z's USB audio with `tools/opz_freqtrack.py` (frequency of every cycle of the tone; recordings and
reports in the git-ignored `device-tests/20261101T14*-freqtrack-h23-r*`). Lead slot 10, project 10, the note held over
MIDI (note 69 on channel 6: **MIDI in reaches lead on channel track + 1** [proven]).

| Step | Result |
|---|---|
| 1, P1 meter | **exactly 220.0–880.0 Hz** at the knob's ends [proven]: P1 maps onto 0 … 0x7FFFFFFF, no word beyond |
| 2, LFO (depth 255, speed 255, → P1, P1 at 0) | **86 Hz** in sine, triangle, square and ramp (= the code's 8 blocks, 11.6 ms) [proven]; top **≈ 850 Hz**: full depth adds ≈ 95 % of P1's range, not 100 % [proven]; random: irregular steps, 41–50 changes/s; tilt: steady values from 220 to 849 Hz as the unit tilts [proven]. **No alarm** in any shape [proven] |
| 2, depth 0 at P1 255 | the same 86 Hz warble, downwards from 880 Hz [proven]; no alarm |
| 3, step locks (P1 low on step 1, 255 on step 3) | each note starts **at its locked value**: the step-3 note's first full cycles measure 879.9 Hz right after the step-1 note (225 Hz) is faded out in ~4 ms; no in-between pitch; the same every bar [proven]. The note-on latch holds. (225 Hz = P1 ≈ 2, most likely the lock set by hand [inferred]) |
| 6, kick track, P2 255 | **440.0 Hz = drum `pitch` 1024** [proven], as the code says; not 2048 |
| alarm | **never**, in five recordings (≈ 2 min of tone) [proven]: no `params` word with bit 31 set reached the engine |
| 7, remove | `opz.py remove 60904 --unassign`: done and verified; `engines/opz_test_param.engine` stays as an orphan file, slot 69 stays taken |

Limits: during the 86 Hz warble the per-cycle measurement under-reads the low end (one 220 Hz cycle is 40 % of an LFO
cycle), so the warble's floor is taken from the steady cases (tilt, square: 220 Hz). One square-shape stretch
(≈ 18.5–20.5 s of recording 2) sat at 850 Hz without measured modulation: unexplained, possibly the same effect.
Step 4 (mod wheel) and 5 were not run. Q-36 answered.
