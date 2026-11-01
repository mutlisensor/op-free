# Engine ABI (draft)

The contract between the OP-Z OS (1.2.x) and a `.engine` module. Reconstructed from the desktop app's symbols
(`SnakePlugin`, arm64), decompiled device code and the 21 official engines. **Not yet verified by running a
custom-built engine** — see ROADMAP phase 3. The matching C header is
[sdk/include/opz_engine.h](../include/opz_engine.h).

## 1. File format

An engine file is an ADI **LDR boot stream**: a sequence of 16-byte block headers, each optionally followed by data.

```text
offset size field
0x00   u16  flags        (0x5000 FIRST|IGNORE on the first block, 0x0100 FILL, 0x8000 FINAL; ADI LDR bits)
0x02   u8   hdrchk       XOR of all 16 header bytes must be 0
0x03   u8   0xAD         marker
0x04   u32  target       load address
0x08   u32  count        bytes of payload (or fill count)
0x0C   u32  argument     first block: offset of the FINAL header (file length − 16); FILL: fill value (must be 0)
```

| Block | Target | Content |
|---|---|---|
| first (FIRST \| IGNORE) | `0x11a00000` | no payload; argument = offset of the FINAL header |
| 1 | **slot base** | **export table** |
| 2… | inside the slot | `.text`, `.rodata`, `.data` (including the export name strings) |
| FILL | inside the slot | `.bss`, value 0 |
| FINAL | `0x11a00000` | end |

Every block must lie entirely inside the engine's slot window ([memory-slots.md](memory-slots.md)). The loader
copies blocks, zero-fills FILL blocks and flushes the instruction cache. There is no signature. [code]

[`tools/opz_verify.py`](../../tools/opz_verify.py) re-implements these checks; [`tools/opz_pack.py`](../../tools/opz_pack.py) writes this layout (both checked against the 13 engines in 1.2.45).

### Export table (at the slot base)

```text
u32 0xfeed0ad1
u32 0xad1facef
{ u32 name_ptr; u32 target; } × N      ; name_ptr → NUL-terminated ASCII inside the slot
u32 0, u32 0                           ; terminator
```

Required names: `initModule`, `init`, `onNoteOn`, `onNoteOff`, and **either** `render` (instrument) **or**
`renderFX` (effect). The loader looks names up by string; order does not matter. [code]

## 2. Calling convention

Blackfin C ABI (CCES / GCC `bfin-elf`): first three arguments in `R0`, `R1`, `R2`, the rest on the stack;
return value in `R0`; `R4–R7`, `P3–P5`, `FP` are callee-saved; return with `RTS`. Pointers and `int` are 32-bit.
Exported names are plain C symbols (no C++ mangling). [code; GCC compatibility to verify, Q-01]

## 3. Entry points

### `initModule` — once, when the engine is loaded

```c
uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest);
```

- `version`: interface version; official engines compare it against an embedded constant (0 in the app).
- `cb`: keep this pointer in a global; it is the only way to reach host services.
- `*memRequest`: bytes of large buffer the engine wants (effects: delay lines). 0 if none.
- Return non-zero for success. Zero means "Failed to initialize module". [code/notes]

### `init` — when a voice or FX slot switches to this plug

```c
void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch);
```

- `state`: the per-voice (instrument, **≤ 280 bytes**) or per-FX-track (effect, **≤ 200 bytes**) state area.
  Clear and initialise it here. The voice record is 0x17c bytes with the state at +0x48, but `Voice::init` clears
  only 0x118 = 280 bytes there and the OS keeps its portamento fields at +0x160…+0x17b (`Voice::render`
  `0x8012e2c0`) [code, device; the macro-crash session's finding]. The FX slot is 0xe0 bytes with the state at +0x10, and the OS
  keeps the FX pitch bend at +0xd8 and the plug id at +0xdc, so an effect owns 200 bytes [code, device]. The header's
  `OPZ_VOICE_STATE_BYTES` (280) and `OPZ_FX_STATE_BYTES` (200), `opzsim`, `tools/opz_cycles.py` and
  `tools/opz_crashcheck.py` all use these limits.
- `track`: track index 0–15. Effects get 8 (FX1), 9 (FX2), 10 (tape) or 11 (master): the only way an effect knows
  where it runs (master and tape behave differently, [routing.md](../../research/routing.md)).
- `mem`: the large buffer for this track (effects; size = your `memRequest`, capped by the track's pool).
- `scratch`: a host scratch buffer. TE's `crush` keeps this pointer in a global and uses 256 bytes of it
  (`scratch` and `scratch + 0x100`) during `renderFX` [code, device]; its size and whether it is shared between
  tracks are open. Don't keep state in it across blocks.
- `arg2` and `mem` on **instruments**: both are the same value, field `+4` of the plug's definition record: **the
  plug's sample-pack id**, crc32 of its `samples` path, 0 for a plug without a pack [code, device `FUN_8012d278`,
  `FUN_800ff9e8`; app `SamplePlayerGain::init`]. Keep it to call `getSample` (§5). The OS calls `init` when a voice
  plays its first note on the plug, not when the plug is selected, and only then locks the pack for the note: a
  note on a plug whose pack is not loaded is dropped before `onNoteOn` ([samples.md §5](../../research/samples.md)).
  **Instruments get no private large buffer**; use the static voice pool in §7.
- `arg2` on **effects** is `0`; `mem` is `fxPool + offset[track − 8]` with `fxPool = 0x8112B21C` [code, device
  `FUN_8012ced0`].

For effects, `init` runs from `FxManager::noteOn` when the FX track's plug changes. **The OS masks interrupts
around an effect's `init`** (`IMASK = 0`) [code, device], so keep it short: don't clear megabytes of delay line
there — clear it progressively in `renderFX`. A note number of 255 means "plug selected, no note".

### `onNoteOn` / `onNoteOff`

```c
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params);
void onNoteOff(int note, void *state);
```

Instruments use these to start and release a voice; most official effects leave them as `return`. **Not every
note reaches `onNoteOn`:** with the **legato** note style (`params[17]` 171–255 on synth tracks) a new note on a
sounding voice only changes that voice's pitch target (the OS glides `pitch`); the engine gets no `onNoteOn` and the
envelope is not retriggered. With **mono** (86–170) every note uses the track's first voice, through the stage-5
fade. So read `pitch`/`pitchSlope` every block and never latch a frequency at note-on [code, device `Voice::noteOn`
`0x8012E0A4`, `InstrumentManager::noteOn` `0x8012D278`; app `Sequencer::getNoteStyle`; details in
[host-tables-flags.md](../../research/host-tables-flags.md#voicing-what-the-flags-do-not-do)]. `tapetwo` and
the unreleased `swirl` use them to follow notes sequenced on their FX track.

On FX tracks there is no voice allocation: `voicePolyphony` covers only the eight instrument tracks, and
`FxManager::noteOn`/`noteOff` pass every note straight to the effect's single state, so chords arrive as several
`onNoteOn` calls and the effect decides how many it honours. One exception: when a note is released while another
is still held and the sequencer asks for it (always on tape; on other tracks when a per-track mode value is 1,
likely a mono/legato note style, not yet identified), `FxManager::noteOff`
calls **`onNoteOn(heldNote, 0, state, NULL)`** instead of `onNoteOff` — velocity 0 and **no parameter pointer** —
so effects must not dereference `params` in `onNoteOn` [code, app `FxManager::noteOff`, `Sequencer::noteOff`].
**[manual p.39, 43, 72]:** notes played or sequenced on an FX track are control input to the effect, not audio notes: TE's
reverb takes its pre-delay from them, tape takes loop start (white keys) and loop length (black keys); holding shift on an
FX track instead plays the previously selected instrument track through the effect (so plain presses reach the plug).

**The master track** uses the same path: `Synth::noteOn` hands track indices 8–11 (FX1, FX2, tape, master) to
`FxManager::noteOn`, so a master plug's `onNoteOn` is reachable. **Sequenced** master steps are played as notes only
when master's note style `params[17]` is ≥ 86 (the manual's "free" style [inferred]); otherwise `Sequencer::playNotes`
skips track 11 and the steps only drive key changes [code, app `0x49e8c`: the byte is current parameter 17 of track
11]. Live keys reach it through `Sequencer::noteOn` → `playNote` unless the active context takes the key. Every master
note also runs `magicChordUpdate`, which transposes the tracks marked on master. Today only TE's chorus is ever on
master; ours would need the master mode below ([routing.md](../../research/routing.md#3-master-can-our-effects-run-there-q-19-q-20),
Q-19, Q-20, Q-30).

### `render` — instruments, once per voice per block

```c
void render(fract32 *out, uint32_t pitch, int32_t pitchSlope,
            void *state, const uint32_t *params, ADSR *env);
```

Recovered from `Voice::renderVoice` in the app [code]:

| Argument | Meaning |
|---|---|
| `out` | the track's buffer: **64 frames of interleaved stereo, 128 values (L, R, L, R…)** [code: TE's `synthsampler` on the device writes 2 values × 64; the app's `Fm::render` writes `out[2i]` and `out[2i+1]`; **proven on hardware**: a mono writer played an octave high with every second half-block silent]. **Add into it with saturation; never overwrite** — every voice of the track renders into the same buffer in turn |
| `pitch` | **phase increment per sample, Q32 of 44.1 kHz** (`f = pitch × 44100 / 2³²`; C0 = `0x184CBB`), at the start of the block. The host has already applied portamento, pitch bend (±200 cents), vibrato and detune (`Tuning::detuneCents`). **On drum tracks (1–4) `pitch` is a fixed unity rate, not a note frequency** — `Voice::getTargetFrequency` uses base 1024 for drum voices, so the word is **1024 at standard tuning** (408–2,565 with tuning, bend and vibrato) [code, app `0x70f84`, device `0x8012e058`; proven: a sine on the kick track is near-DC; this document said ≈2048 before, and opzsim and the cycle driver still pass 2048: hardware check H-23 step 6]. `pitch` is a `uint32_t` that can exceed 0x7FFFFFFF (up to ~0xB66E2BE5: note 127 with the tuning at +12 semitones, bend and vibrato), and the frequency table has no bound check for notes above 127 [code, [param-paths.md](../../research/param-paths.md#pitch-and-pitchslope)]. Drum engines take pitch from `onNoteOn`'s note number: the 24 keys send **MIDI 53–76** (F3 first), and each octave press on a drum track adds ±24, so `key = (note − 53) mod 24`, `octave = floor((note − 53) / 24)` [code, app `KeyToMidi`, `Sequencer::noteOn`, `trackOctaveUp`; TE's `sampleplayergain` splits the note the same way on the device] |
| `pitchSlope` | `(pitchAtEnd − pitch) >> 6` (arithmetic, wrapping): add it once per sample to glide smoothly across the 64-frame block. A pitch jump (legato without portamento) arrives as one block's slope; across 0x80000000 a rise can give a negative slope, so compute in `uint32_t` [code, device `0x8012e3a0`] |
| `state` | this voice's state |
| `params` | 18 smoothed parameters, Q31 (see §4) |
| `env` | the voice's amplitude envelope, owned by the host but **advanced by the engine** (see below) |

What an instrument's `render` does, from `Fm::render` in the app [code]:

1. If `env->stage == 0` (offset `+16`), return at once: the voice is idle.
2. Once per block, call `cb->updateParametersADSR(env, params[2], params[3], params[4], params[5])` (attack,
   decay, sustain, release). It moves the envelope between stages and sets its per-sample coefficients.
3. For each of the 64 frames: compute the sample, multiply by `env->level` (`+4`, Q31), add into `out[i]` with
   saturation, advance the phase by `pitch` (+ `pitchSlope`), and step the envelope (below).
4. **The host frees the voice when `env->stage` reaches 0** (`Voice::renderVoice` checks it after every
   `render`). An engine that never lets its envelope finish holds the voice forever.

### The envelope

Decoded from the app (`updateParametersADSR`, `Fm::render`, `Voice::noteOn/noteOff/killNote`) and confirmed on the
device: `synthsampler`, `cluster` and `organ` run the same per-sample arithmetic, and `analoga` carries the same
release-rate curve [code]. Implemented in [`sdk/include/opz_adsr.h`](../include/opz_adsr.h).

| Offset | Field |
|---|---|
| `+0` | attack step for the next note-on (written by `updateParametersADSR`) |
| `+4` | level, Q31 |
| `+8` | coefficient `c`, Q31 |
| `+12` | step `s`, Q31 |
| `+16` | stage: 0 idle (voice freed), 1 attack, 2 decay, 3 sustain, 4 release (drums: decay), 5/6 fade, 7 hold (drums) |
| `+20` | new-note byte (set by note-on) |
| `+21` | release flag (set by note-off, consumed by `updateParametersADSR`) |

Per sample: **`r = c·level; level = r + (1 − r³)·s`** (Q31, saturating; the Blackfin's fractional multiply
rounds). Once per block `updateParametersADSR(env, A, D, S, R)` sets `+0` from the attack parameter and picks
`(c, s)` by stage:

| Stage | `c` | `s` | Transition |
|---|---|---|---|
| 1 attack | 1 | attack step | level ≥ 0.99 → decay |
| 2 decay | decay rate | 0 | level ≤ S → sustain |
| 3 sustain | 1 | (S − level) / 256 | — |
| 4 release | release rate | 0 | level < `0x10624E` (≈ −66 dB) → idle |
| 5, 6 fade | unchanged | −min(\|s\|, 0.0095·level) | level < `0x10624E` → idle |

If the release flag is set and the stage is not idle, it first switches to release. Rates come from two 128-entry
tables indexed by `parameter >> 24`: a per-sample attack step that halves about every 8 entries (index 0 is
near-instant), and a decay/release coefficient `e^(−1/τ)` with τ ≈ 250·i + 11.5·2^(0.115·i) samples — linear up to
about 0.5 s, then exponential up to ~7 s; decay never goes below index 1 (~6 ms). These curves are our fits to
TE's tables, used only by the simulator: engines never need the tables, the host computes the rates.

Note-on resets a free voice to level 0, stage 1 (`c` = 1, `s` = attack step). A note on a busy voice, and voice
stealing (`killNote`), first fade it in stage 5 (`c` = 0.992, `s` ≈ −level/256) and start the new note when the
fade reaches idle. Note-off sets the release flag.

### The drum envelope (`updateParametersAHD`)

Decoded from the app (`updateParametersAHD`, `SamplePlayerGain::onNoteOn`/`render`) and confirmed on the device
(`sampleplayergain` makes the same call with the same layout and per-sample step) [code]. Details and addresses:
[ahd-drum-notes-cpu.md](../../research/ahd-drum-notes-cpu.md). Helpers: `opz_ahd_layout`, `opz_ahd_tick` in
[`opz_adsr.h`](../include/opz_adsr.h).

```c
cb->updateParametersAHD(env, pos, attack_end, hold_end, decay_end, coef, shape);   /* once per block */
```

The arguments are **positions in frames** along the drum sound, not knob words: `pos` is how far the engine has
played (TE: the sample read position, so a pitched-up drum has a shorter envelope), the three landmarks come from
the knobs (below). Per sample the engine applies **`level = coef·level + step`** (Q31, saturating) — linear, not
the ADSR's `r³` form.

| Stage | While | `coef`, `step` |
|---|---|---|
| 1 attack | `pos < attack_end` | 1, `1 / (attack_end − pos)` (level clips at 1.0) |
| 7 hold | `pos < hold_end` | level set to 1.0; 1, 0 |
| 4 decay | `pos < decay_end` | `coef`, `−level / max(decay_end − pos, 64) · (1 − f·shape³)⁴`, `f = (decay_end − pos) / (decay_end − hold_end)` |
| 0 | `pos ≥ decay_end` | voice freed |
| 5, 6 fade | voice steal | as the ADSR |

There are no rate tables and **note-off is ignored** (drums are one-shots; TE's "gate" note style fades the voice
itself through stage 5). TE's drum player derives the landmarks from params[2..5] and the sample length `len`:
params[2] = start point (`≤ len − 512`), [3] = attack (up to half of what follows the start), [4] = hold (≥ 512
frames), [5] = decay: below 50 % its length grows to the rest of the sound with `coef ≈ 0.99995`, above 50 % the
length is full, `coef = 1` and `shape = 2·p − 1` bends the line into "hold, then drop". A synthesized drum engine
picks a length, keeps a position counter and gets the same knobs and voice freeing as TE's drum tracks.

### `renderFX` — effects, once per FX track per block

```c
void renderFX(const uint32_t *params, fract32 pitchBend, void *state,
              const fract32 *in, fract32 *out, fract32 gain);
```

64 frames of **interleaved stereo in a separate input and output buffer** [code: TE's `crush` on the device reads
`in[0], in[1], in[2], …` and writes `out[0], out[1], out += 2`; the app's `Crush::renderFX` stores `L,R` pairs to
its 5th argument; **proven on hardware 2026-10-17**: a passthrough that left `out` untouched returned silence].

- `params`: FX1/FX2 get their LFO-modulated parameters, tape and master the plain ones (no LFO) [code, device].
- `pitchBend`: the FX track's current pitch bend (stored per FX track by `FxManager::pitchBend`).
- `in`: what is sent to this FX track, `L, R, L, R, …` (128 values): the sum of every track's send, the external
  input's send and (FX1/FX2) tape's send. Read-only on FX1, FX2 and tape.
- `out`: write **all** 128 values, interleaved. Whatever the engine does not write is not the input.
- **On master `out` is NULL**: the OS passes the main mix as `in` and expects the result written back into it, as
  TE's master `chorus` does [code, device call `0x801198b0`; app `FxManager::render(11, …, mix, 0, …)`]. An engine
  that writes `out` there writes to address 0 and crashes the OP-Z. Every engine of ours does; none may go on master
  until it handles `out == NULL` (`dst = out ? out : (fract32 *)in`, each frame read before it is written) and
  ignores P2 (master P2 is also the OS's drive) and pan/level. Details and the hardware check:
  [routing.md §3](../../research/routing.md#3-master-can-our-effects-run-there-q-19-q-20).
- After `renderFX` the OS runs the FX track's own filter (`params[8]`/`[9]`) and a DC blocker on `out` and adds it
  to the main mix; it applies **no pan or level** to an FX return [code, app `Synth::render` `0x52ce4`]. Tape's
  return also feeds FX1/FX2 by tape's own sends.
- The OP-Z's track sends crossfade: raising a track's FX send moves its signal from the dry path into the FX
  track, so an effect returning silence makes the sound fade out as the send goes up [proven].
- `gain`: the FX track's mixer level × mute (Q31; `0x7fffffff` on master) [code, device `0x801196ce`]. An engine
  that ignores it ignores the mixer's level and mute for its track. `crush` multiplies `params[11]` (level) by it for the output level, and uses
  `params[10]` (the "pan" slot) as a **dry/wet balance**: wet = `min(2·p, 1)`, dry = `min(2·(1 − p), 1)`.
  New effects should follow the same convention so the FX track's pan/level knobs behave like TE's. **[manual p.6, 21]:** 1.2.26
  added a "dry mix" setting to all FX plugs (our pan = dry/wet), but the manual's parameter chart draws only one page
  (P1, P2, filter, resonance) for FX tracks even though the text gives them an LFO and send editing: which page holds pan and
  level on an FX track is a hardware check ([manual-notes.md §3](../../research/manual-notes.md#3-parameter-pages-and-the-18-parameters)).

## 4. Parameters

`params[i]` starts as a Q31 value `b·0x808080 + (b >> 1)` for the parameter byte `b` that every source sets (knobs,
MIDI CC, step locks, presets, `$0E`, punch-in, arpeggio). The sequencer tick makes it the target; once per block
`params[0..11]` move 1/64 of the way to it (`w ← 63·(w >> 6) + (t >> 6) + (t >> 25)`), `params[12..17]` jump; at
every note-on all 18 are set to the step's bytes at once (no smoothing, every voice of the track); then the track
LFO rewrites one of P1, P2, filter, resonance, attack, pan, level per block and clamps it. **Every word is in
0..0x7FFFFFFF** but can be any value there and jump the whole range from one block to the next; it never changes
during a call [code, device `0x80117808`, `0x801183d8`, `0x801179d8`, `0x8012d994`; Q-23 answered,
[param-paths.md](../../research/param-paths.md)]. `params` points to a per-track copy that the next track reuses: read
it during the call only. On the arp track (no LFO: its LFO page drives the arpeggiator) `params[14..17]` are the
previous track's words [code, `0x80119cd4`].

| i | Parameter | i | Parameter |
|---|---|---|---|
| 0 | **P1** | 9 | resonance |
| 1 | **P2** | 10 | pan |
| 2 | attack | 11 | level |
| 3 | decay | 12 | portamento |
| 4 | sustain | 13 | LFO depth |
| 5 | release | 14 | LFO speed |
| 6 | FX1 send | 15 | LFO target |
| 7 | FX2 send | 16 | LFO shape |
| 8 | filter | 17 | note style |

**[manual p.21–22, 59]:** the user sees P1/P2 and filter/resonance on the first page (green, blue, yellow, red dials), the
envelope on the second, LFO depth/rate/destination/shape on the third (8 destinations: P1, P2, cutoff, resonance, attack,
pitch, pan, volume; shapes 6 free + 6 note-triggered) and FX1/FX2 send, pan, level on the fourth. MIDI CC 1…18 (absolute, 7-bit) and
32…49 (relative) set the parameters in a different order than `params[]`: P1, P2, filter, resonance, A, D, S, R, LFO
depth, speed, target, shape, FX1, FX2, pan, level, portamento, note style; CC 1, the mod wheel, is P1
[code, app `g_incomingCCToParameter`; [param-paths.md §3](../../research/param-paths.md#3-midi-an-engine-can-use-and-what-the-op-z-sends)]. Drum tracks label the green page attack/decay/sustain/release.

Engines own **only P1 and P2**. The filter knob is the OS's own bipolar track filter — neutral at the centre,
low-pass one way, high-pass the other, cutting to silence at the ends — on instrument **and FX** tracks; resonance
belongs to it too [proven: FX1 with `swirl`, and the test sine]. Don't reuse `params[8]`/`[9]`. **The OS filters every instrument track itself after the engine** (`trackFilterProcess`
and a DC blocker in the app; on hardware the filter knob changed our test sine, which ignores it) [code, proven], and
it also runs the envelope stages, LFO, sends, pan and level (see
[02 — The sound engine host](../../research/architecture/02-sound-engine-host.md#parameters)). An engine that also filters on
`params[8]`/`[9]` filters twice — unless its plug sets **flag `0x04`**, which turns the OS filter off for that
track so the knobs are the engine's (TE's `analoga`; [plug-format.md](plug-format.md#plug-fields)). The OS filter is
an RBJ biquad from the `rFilterCoeffs` tables (§5); `opz_host_rf_lookup_os` gives an engine the same response. The LFO can target P1/P2 (fastest: 8 blocks per cycle, depth up to 95 % of the range), so these two should respond smoothly at block rate.

## 5. Callback table (`ModuleCallbackInterface`)

Thirteen pointer-sized entries (offsets for the 32-bit device). Meanings come from the app [code]; the device
engines' use of each entry was checked with Ghidra on the 13 local engines [code, device]:

| Engine | Entries it calls | `initModule` memory request |
|---|---|---|
| `analoga` | `+0x14` rFilterCoeffs | 0 |
| `cluster`, `organ` | `+0x1c` updateParametersADSR | 0 |
| `ep` | `+0x0c` g_sine, `+0x1c` updateParametersADSR | 0 |
| `sampleplayergain` | `+0x04` get value, `+0x20` updateParametersAHD, `+0x24` getSample | 0 |
| `synthsampler` | `+0x04` get value, `+0x1c` updateParametersADSR, `+0x24` getSample | 0 |
| `delay` | `+0x04` get value (tempo sync), `+0x08` k_lp_table | 2 MB |
| `chorus`, `dist`, `reverb` | `+0x08` k_lp_table | 16 KB, —, **114 KB** |
| `chorus2`, `crush`, `fdnreverb` | none | 32 KB, —, 239 KB |

No shipped engine calls `+0x28` (`getPresetData`).

| Offset | Entry |
|---|---|
| `+0x00` | `void sequencerSetValue(int key, int value)` |
| `+0x04` | `int sequencerGetValue(int key)` |
| `+0x08` | `const fract32 *k_lp_table` — 129 one-pole coefficients (below) |
| `+0x0C` | `const fract32 *g_sine` — 8,193 × Q31 sine cycle |
| `+0x10` | `const int16_t *g_sineTable` — 256 × int16 sine cycle (not Q31) |
| `+0x14` | `const fract32 *const *rFilterCoeffs` — **5 pointers** to `[9][65]` Q31 biquad tables |
| `+0x18` | `const fract32 *releaseTable` — 128 per-sample decay multipliers |
| `+0x1C` | `void updateParametersADSR(ADSR*, uint32_t a, uint32_t d, uint32_t s, uint32_t r)` |
| `+0x20` | `void updateParametersAHD(ADSR*, uint32_t pos, uint32_t attack_end, uint32_t hold_end, uint32_t decay_end, fract32 coef, fract32 shape)` — the drum envelope, §3 |
| `+0x24` | **sample access**: `const OpzSampleSlice *getSample(int packId, uint8_t note)` — null in the app, filled on the device (`FUN_80100e64`). `packId` is `init`'s `arg2`; returns the loaded pack's slice for key `(note + 19) mod 24` (drum) or its one slice (synth), or NULL for an unknown id or a pack no longer requested. `sampleplayergain` and `synthsampler` call it in `onNoteOn` with the id they stored in `init` and play the result whatever `sequencerGetValue(8)` says (key 8 only decides whether they re-read the slice every block). By then the OS has locked the pack for the voice, so the slice is valid until the voice ends. Any engine can use it (Q-27). Slice layout, what each word does, memory: [samples.md](../../research/samples.md) §5, §8 |
| `+0x28` | `bool getPresetData(int, int, void *out)` — the app ignores both ints and **reads a file of up to 4,096 bytes into `out`**: `engines/fmedit.preset`, or `rendertone.preset` when a global mode flag is set. Returns true on success |
| `+0x2C` | `void IIRFilter_updateCoefficientsPeak(IIRFilter*, uint16_t, uint8_t)` |
| `+0x30` | `void IIRFilter_filter(IIRFilter*, fract16 *buf, uint8_t)` |

### Sequencer keys

`key = (index << 16) | id`. Both functions are a jump table on `id` [code, app `sequencerGetValue` `0x50e38`,
`sequencerSetValue` `0x50da8`; device `0x80109978`, `0x801098ce`: the same cases on the same sequencer offsets].
Other ids return −1 (get) or do nothing (set). Engines use them through
[`opz_tempo.h`](../include/opz_tempo.h) (Q-18).

| Call | id | Reads / writes | Units, range | TE engines |
|---|---|---|---|---|
| get | 2 | **FX timing** of track `index`: the pattern's track byte `+5` (`getTrackTiming & 0x1f`) | 0..23, a note value (below); saved with the pattern | `delay`, `tapetwo` (index 10) |
| get | 3 | "has FX timing" flag of track `index` (the byte `set 3` writes) | 0/1; cleared when an FX track's plug changes and by `resetEffects` | — |
| get | 4 | **tempo**: `Sequencer::getBPM(false)`, the tempo lock if set, else the project tempo | **whole BPM, 40..200** | `delay`, `tapetwo` |
| get | 5 | **playing**: byte `+0x1c`, set by `startPlayback`, cleared by `stopPlayback` | 0/1 | `tapetwo` |
| get | 6 | **global step**: int `+0x38` | 0 .. pattern length − 1 | `tapetwo` (`& 15`) |
| get | 7 | **tick within the step**: int `+0x40` | 0..23: 24 per sixteenth, **96 per beat** | `tapetwo` |
| get | 8 | byte `+0x61`, set by `DefaultContext::onCC` (`0x75fe4`) only while a user sample pack is active (sample mode editing it); while set, `Sequencer::tick` stops pushing track parameters | 0/1 | `sampleplayergain`, `synthsampler`: after `getSample`, to keep the slice pointer and re-read its rate/volume (and loop points) every block — **not** a "samples ready" gate; playback ignores it [code, device `0x81b69f6e`]. SDK: `OPZ_SEQ_GET_SAMPLE_EDIT` |
| set | 0 | sequencer byte `+0x2d` = `value != 0` | — | `tapetwo` writes `!playing` every block (meaning not named) |
| set | 1 | `Synth::setFXDucker(10, value)`, the **tape ducker** | Q31 | `tapetwo` |
| set | 2 | `Sequencer::setFxTiming(index, value)`: writes the track byte, keeping bit 7 if it was set | 0..23 | `delay` and `tapetwo` `onNoteOn` |
| set | 3 | "has FX timing" flag of track `index` = `value > 0` | — | `delay`, `tapetwo` `onNoteOn` |

All [code, app], with the device's switch matching case by case [code, device]. Semantics of key 0 (set) and key 8
are named from their writers and readers only [inferred].

**How the counters move** [code, app]. A sequencer thread (`update_sequencer`, `0x71dc4`) calls `Sequencer::tick`
every 60 × 10⁹ / (96 × BPM) ns; a song bounce uses 0x50BFE0 / (96 × BPM) interleaved values, i.e.
**44,100 × 60 / (96 × BPM) frames per tick**. `tick` (`0x4940c`) plays the notes of (step, tick), then increments the
tick; at 24 it returns to 0 and the step increments, wrapping at the pattern length (`+0x3c`: the longest track's
step count × step-length multiplier, set in `nextPattern`, up to 256) into the next pattern. **The counter runs
while stopped**; only playing notes and stepping tracks need key 5. So keys 6 and 7 read **one tick ahead** of the
position just played. Play (`startPlayback`, `0x4ee84`) zeroes both counters inside a tick, which then plays (0, 0):
the first block an engine sees reads (0, 1). Stop (`stopPlayback` → `resetPlaybackState(false)`) moves the step to 0
if the tick was below 8, else to the pattern's last step, and leaves the tick. MIDI clock out is every 4th tick
(24 PPQN). On the device the same offsets are read by the tempo screen's LEDs (`0x801248fc`: step `& 3`,
tick `% 24`); **where the device runs `tick`** (between audio blocks, as in the app's bounce, or in an interrupt
that splits a block) was not found [inferred: between blocks]; hardware check H-7.

**What key 4 leaves out.** The tempo dial moves 1 BPM per 4 detents (`TempoContext::onCC`, `0x2ef6c`) and
`setBPM` clamps to 40..200, so the tempo is always whole. The clock itself uses `getBPM(true)`, which adds the
nudge slip (`TempoContext::updateSlip`, `+0x39c764`) and a second offset (`+0x39c76c`), clamped to 40..200: while
the user nudges, the counters speed up or slow down but key 4 does not change. Synced to MIDI clock, ticks come
from the clock (nudge adds or drops ticks, `nudgeTick`) and `midiClickTempo` writes the measured tempo, rounded,
into the project tempo when it is 40..200, so key 4 follows. The tempo lock (`+0x39c768`, toggled on the tempo screen,
`TempoContext::onCC` CC 40) holds one tempo across projects.

**Not exposed.** Swing (a per-project byte, `+0x344ccd` of the project, 0..255 with 128 = none, applied to note
times by `ApplyQuantAndSwing`, `0x7b35c`), the metronome, per-track step counts, step lengths and track positions
(`getTrackStepAndTick`): no key reads them [code]. An engine knows the global grid, not its own track's.

**How TE's engines turn them into time** [code, app; `delay` also device]:

- `Delay::renderFX` (`0x65bc`; device `delay.engine` the same): BPM = clamp(key 4 + (pitchBend >> 25), 40, 220)
  (the pitch bend shifts the tempo by up to ±64 BPM), timing = key 2 of its own track; delay = num × 10,584,000 /
  (den × BPM) frames, capped at 2¹⁹. 10,584,000 = 44,100 × 240, a whole note at 1 BPM; (num, den) come from a
  24-entry table of note values (`k_delayPattern`, `0x7e808`): index 0 is 1/128 note, 18 a whole note, 23 four;
  in between sixteenth multiples n/16 (n = 1..10) interleave with straight and triplet values. `onNoteOn` sets
  key 3 and, for notes 53..76 (the 24 keys), key 2 = note − 53: the keys choose the delay time and the pattern
  remembers it.
- `TapeTwo` (`0xa9a4` and helpers): frames per step = 0x285FF0 / (4 × BPM) = 661,500 / BPM; key 5 every block
  (and set 0 = not playing); key 6 `& 15` tags per-step buffer positions; key 7 + 24 × step is the read position
  in ticks; loop lengths come from the tape track's key 2.
- `Swirl` reads no sequencer key. The OS's own track LFO in its synced rates (device, near `0x8012dbc0`) scales a
  per-rate constant (256 … 2 for 1/64 … 2/1) by 101 × 64 × BPM: a free-running phase at a tempo-derived rate.

**For engines** ([`opz_tempo.h`](../include/opz_tempo.h)): `opz_samples_per_note(bpm, num, den)` and
friends give delay times; `OpzClock` gives a block-accurate position: snapped to the tick just played whenever the
counter moves (at a pattern wrap, to the last step's last tick), interpolated from the tempo in between, never past
the next tick, `jumped` on start, stop, pattern change and nudge. Beat and bar phases are shifts of it. opzsim's
keys 5–7 follow the same model (`transport STEP play|stop` in scenarios; the global step wraps at 16).

### Shared tables

Every table is generated by a formula; ours regenerate the OS 1.2.45 tables to ±1 LSB (`k_lp_table` ±11) [proven,
offline]. Accessors, format checks and the 5 KB recipe: [`sdk/include/opz_tables.h`](../include/opz_tables.h);
evidence, addresses and cycle costs: [host-tables-flags.md](../../research/host-tables-flags.md) (Q-15).

| Table | Format | Formula | Index (TE) |
|---|---|---|---|
| `k_lp_table` | 129 × Q31 | `c` of `y += c·(x − y)`, **two poles in series** −3 dB at `10 Hz · 2000^(i/128)` | integer, no interpolation (`delay`, `dist`, `chorus`, `reverb`) |
| `g_sine` | 8,193 × Q31 | `round(0x7FFF0000 · sin(2πi/8192))`, guard point = entry 0 | `phase >> 19`, no interpolation (`ep`, OS LFO) |
| `g_sineTable` | 256 × int16 | `min(32767, round(32768 · sin(2πk/256)))` | no TE reader (`phase >> 24`) |
| `rFilterCoeffs` | 5 pointers → `[9][65]` Q31: `b0, b1/2, b2, a1/2, a2` | RBJ biquads: LP at the ISO third-octaves 25 Hz–20 kHz (columns 0–29), flat (30–34), HP (35–64); rows Q 0.2…16 (LP) / 0.2…4 (HP) | bilinear: column `c >> 25`; rows `q >> 28` (`analoga`) or `min(q >> 27, 7)` (OS track filter) |
| `releaseTable` | 128 × Q31 | `e^(−1/τᵢ)`, τ ≈ 0.96 + 248.8·i + 11.72·e^(0.0795·i) samples (i ≥ 1), τ₀ ≈ 33.6 | `param >> 24` (`updateParametersADSR`) |

Reach the tables **only through `cb`**, as TE's engines do (they keep `cb` from `initModule` and load the table
pointer from it). The addresses are OS-version specific (1.2.45: `g_sine` `0x0800BB44` in L2, `g_sineTable`
`0x11B019E8`, the others in DDR) [code, device]. opzsim and the cycle driver pass tables generated from the same
formulas ([`opz_tables_gen.h`](../include/opz_tables_gen.h), [`tools/opz_tables.py`](../../tools/opz_tables.py)).

## 6. Rules for engine authors

1. **Everything lives in the slot window**: code, constants, globals, the export table and the name strings.
   Budget: 50 KB, 10 KB or 5 KB.
2. **One copy of the code serves every voice and every track** that uses the engine. Globals are shared; put
   per-voice data in `state`. Only truly global data (the callback pointer, tables) may be global.
3. **No heap, no libc, no floating point.** Use Q31/Q15 integer math and the Blackfin's fractional builtins.
   Avoid division in the audio path. Compile without the C runtime (`-nostdlib`, no static constructors).
4. **No blocking, no I/O.** Engines run in the audio path.
5. **Bounded CPU.** The CPU runs at **400 MHz**: a 64-frame block is 580,499 cycles. The OS times the whole
   instrument stage (all voices plus its own per-track filter, LFO and mixing) and kills one note when it takes
   more than **290,000 cycles**, or more than 265,000 three blocks in a row — the quietest voice in release, fade
   or drum hold/decay (stage ≥ 4); voices in attack or sustain are never killed, the block just runs long [code,
   app + device]. The OS's own share of that stage is ~61,500 cycles with 8 active tracks and 19 voices, which leaves
   **~167 cycles/sample/voice** (188 for a single block) [code, estimated; [cpu-budget.md](../../research/cpu-budget.md)].
   Target ≤ 150 cycles/sample/voice for instruments and ≤ 600 cycles/sample for effects (effects run after the timed
   stage and are never killed, but share the same 580,499-cycle block).
6. **Saturate, never wrap.** A wrapped sample is a full-scale click on headphones.
7. Clear `state` in `init`. The OS clears 280 bytes when a voice's plug changes, but a voice that keeps its plug
   gets the next note on the previous note's state.
8. Keep `onNoteOn`/`onNoteOff` cheap; the sequencer calls them from its own timing.
9. **Instruments add into `out`** and must let `env->stage` reach 0 so the voice is freed.
10. Assume **44.1 kHz**, **64-frame** blocks and a **400 MHz** CPU (confirmed in app and device code).
11. **Peak level per voice: `OPZ_VOICE_PEAK` = −12 dBFS** at full velocity. Calibrated on hardware: the test sine at
    this level matches TE's saw at the same note and knob settings.
12. **Honour the host's fade (stage 5/6).** When a new note needs a busy voice, the OS sets that voice's stage to 5 and
    starts the new note only when the stage reaches 0. Engines that follow the host envelope fade automatically;
    engines with their own envelopes (drums) must fade out within a few milliseconds and set stage 0, or new notes
    wait for the old sound to finish [proven on hardware with `kit` v1].
13. **State limits: 280 bytes per voice, 200 per FX track.** Bytes 280–307 of a voice's area are the OS's portamento
    fields, and the next voice's record follows. Bytes 200–207 of an FX area are the OS's pitch bend
    and plug id. Keep `_Static_assert(sizeof(State) <= OPZ_VOICE_STATE_BYTES)` (or
    `OPZ_FX_STATE_BYTES`) in the engine.
14. **Never raise an exception; it restarts the OP-Z.** Every CPU exception goes to TE's handler, which logs it and
    reboots through the watchdog ([crash-safety.md](../../research/crash-safety.md)) [code]. That covers misaligned
    16/32-bit accesses (keep `int16_t`/`int32_t` data aligned; don't cast a byte pointer to a wider type), the NULL
    page and any address outside the mapped memory (FX `onNoteOn` may get `params == NULL`, §3), undefined
    instructions, and `EXCPT`/`__builtin_trap`. Division by zero does **not** trap on the Blackfin (it returns
    garbage): guard divisors anyway.
15. **Never run** `CLI`/`STI`, `RAISE`, `IDLE`, `RTI`/`RTX`/`RTN`, cache flushes or writes to system registers, and
    never touch L1 memory or MMRs: they are not memory-protected, so a wild write there is not caught [code].
16. **Registers:** hand back R4–R7, P3–P5, FP and SP (the C ABI); **L0–L3 must be 0** at every return and call
    (the OS clears them on interrupt entry and assumes 0 in its own code); LC0/LC1 may be left non-zero only as GCC
    leaves them (a counted loop left by `break`) [code + notes].
17. **Stack: at most 1 KB per call, no recursion, no variable-length arrays.** `render`/`renderFX` run on the 10 KB
    interrupt stack after ~0.5–1 KB of OS frames, and other interrupts nest on it; the other entry points run on
    8 KB task stacks (as far as traced). There is no guard: an overflow silently corrupts DDR [code]. Our engines use 0–348 bytes.
18. **`render` can run in the middle of `onNoteOn`.** The OS marks the voice as playing before it calls `onNoteOn`
    from the sequencer or UI task, and the audio interrupt may render the voice before `onNoteOn` returns
    [code; inferred that it happens]. Write `onNoteOn` so that every partly written state is still safe to render:
    compute into locals and store indices and pointers last, keep table indices in range when they are read (mask or
    clamp in `render`), and never leave a pointer half updated. The same holds for a re-triggered voice, whose
    state is the previous note's.
19. **Check before installing:** `tools/opz_crashcheck.py engines/<name> --full` (static checks; stack, register,
    guard and NULL-page checks in the simulator; every preset; renders on half-written states; the OS's worst
    parameter and pitch streams, `tools/opz_params.py`, which also reports their CPU and level). It must report no
    errors. On the device, a crash leaves `systeminfo/crash01.log` on the update-mode disk; decode it with
    `tools/opz_crashlog.py` ([recovery runbook](../../research/crash-recovery.md)).
20. **`onNoteOff` can come from the audio interrupt, more than once.** On CPU overload the OS fades the quietest
    decaying voice and calls its `onNoteOff` between two `render` calls, inside the interrupt, and usually again in
    the next blocks while the overload lasts [code: `0x8012d142` → `0x8012dfae`; inferred that it repeats]. Keep
    `onNoteOff` short and idempotent: a second call on a released voice must change nothing that matters
    ([cpu-budget.md §2](../../research/cpu-budget.md#2-the-overload-rule-exactly)).

20. **Accept every parameter word 0..0x7FFFFFFF, in any order.** Not only the knob values `b·0x808080 + (b >> 1)`:
    the smoother and the LFO give any word, 0 and 0x7FFFFFFF exactly, and a note-on or the LFO moves a word across the
    whole range in one block. Derive indices and counts with shifts (`>> 24`, `>> 27`) or clamps, never divide by a
    word that can be 0, and don't keep the `params` pointer past the call. `& 0x7FFFFFFF` is harmless insurance; the
    OS 1.2.45 never sets bit 31 (Q-23) [code, [param-paths.md](../../research/param-paths.md)].
21. **`pitch` is a `uint32_t`, `pitchSlope` a wrapping `int32_t`.** Expect words from ~0x4D8C7 up to ~0xB66E2BE5
    (above Nyquist, above 0x7FFFFFFF) and a full-range jump as one block's slope; drum tracks 408..2,565. Clamp
    whatever you derive from the pitch (delay lengths, filter coefficients, oversampling, per-note tables), and keep
    the cost bounded at the top of the range [code; `opz_params.py` streams].
22. **FX `pitchBend` is any `int32_t`, `INT32_MIN` included** (MIDI bend 0 gives 0x80000000; negating it
    overflows). `onNoteOn`'s note and velocity are bytes, 0–255 in the type (0–127 from the keys and MIDI)
    [code, app `Sequencer::pitchBend`; device `0x8012d47e`].

## 7. Pattern: per-voice buffers larger than 280 bytes

Plucked strings, comb filters and per-voice delay lines need kilobytes per voice, but `state` holds 280 bytes.
Unless instruments also get a `mem` buffer (Q-02), use a **static voice pool** inside the slot window:

```c
#define POOL_VOICES 6
static int16_t s_delay[POOL_VOICES][2048];   /* 24 KB of .bss → needs a 50 KB slot */
static uint8_t s_owner_used[POOL_VOICES];

/* in init(): claim a free pool entry and store its index in state */
```

The shipped `bow` (`ministringshort`, a 5 KB slot) shows that string synthesis fits the OP-Z; reading how it
holds its delay lines is part of P0.5. Remember that the pool is shared by **all tracks** using the engine.
