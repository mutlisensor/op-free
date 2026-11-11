# opzsim — OP-Z engine host simulator

`opzsim` runs an engine compiled for macOS with the call pattern the OP-Z uses, so DSP can be developed and
tested before a Blackfin toolchain and hardware runs exist (ROADMAP P1). It is our own model of the host in
[engine-abi.md](../sdk/docs/engine-abi.md) and [02 — The sound engine host](../research/architecture/02-sound-engine-host.md).
It contains no TE code, tables or binaries. The same host also runs in the browser (see [Web simulator](#web-simulator)).

```bash
make -C sim test
```

That builds `sim/build/opzsim`, builds every `engines/<name>/` that has a `plug.json` as `sim/build/engines/<name>.dylib`,
runs the host checks and compares every golden render. `make -C sim golden` re-renders and overwrites the golden
files. Review that diff like code.

## Running an engine

```bash
sim/build/opzsim -e sim/build/engines/opz_test_sine.dylib --slot 57 --trackmask 240 -o sine.wav engines/opz_test_sine/tests/basic.scn
```

```bash
sim/build/opzsim -e sim/build/engines/opz_test_sine.dylib --fx sim/build/engines/opz_test_passthrough.dylib --fx-slot 87 -o chain.wav engines/opz_test_sine/tests/basic.scn
```

| Option | Meaning |
|---|---|
| `-e PATH` | engine dylib, instrument (`render`) or effect (`renderFX`) |
| `--fx PATH` | effect after the instrument; its input is the instrument mix |
| `-o PATH`, `--bits 16\|32` | stereo WAV, 44.1 kHz (default 32-bit; goldens use 32-bit) |
| `--stats PATH` | JSON: peak, RMS, DC, clipped samples, octave-band spectrum, voice use, budgets, host time |
| `--slot N`, `--trackmask M` | from `plug.json`; enable the static-size and track checks |
| `--generous-chords` | 6 voices on the chord track instead of 4 |
| `--no-check-add` | skip the "render adds into out" probe |
| `-v` | log events and callback calls |

Exit codes: `0` ok, `1` usage or I/O, `2` engine refused (missing export, both or neither of `render`/`renderFX`,
`initModule` returned 0), `3` a scenario expectation failed, `4` an ABI or budget rule was broken.

## What the host does

| Behaviour | Source |
|---|---|
| 44,100 Hz, 64-frame blocks | [code, app] |
| Parameters: byte `b` → `b·0x808080 + (b >> 1)`, then `cur += (int32)(target − cur) >> 6` every block before rendering | [code, app] `Synth::interpolateParameters`; the `>> 6` form is ours [inferred] |
| Values set at step 0 (`preset`, `param … 0`, a sweep's start) are loaded unsmoothed, as with a project load | [idea] |
| `pitch` = Q32 phase increment, `f · 2³² / 44100`, MIDI note numbers with A4 = 69 = 440 Hz (C0 = 12 → `0x184CBB`, matching the ABI doc) | [code, app]; note numbering [inferred] |
| `pitchSlope = (int32)(pitchAtEnd − pitch) >> 6`; pitch bend ±200 cents moves pitch at the next block | [code, app] |
| Voices per track 2, 2, 2, 2, 2, 3, 2, 4 (6) | [code, app] `voicePolyphony` |
| Allocation: free voice, else the oldest releasing voice, else the oldest | [idea] |
| Instruments: `init(state, track, packId, packId, scratch)` when a voice first plays this plug. `packId` is the plug's sample-pack id, crc32 of its `samples` path (`arg2`, and the same number as `mem`: never a pointer), so **0 for synth plugs**; opzsim passes the id of the pack a scenario attaches with `sample`, else 0. Instruments get **no private large buffer**: a non-zero `memRequest` from an instrument is ignored with a warning (use the static voice pool, engine-abi.md §7) | [code, device] `FUN_8012d278`; [samples.md](../research/samples.md) |
| FX: `init(state, track, 0, mem, scratch)` at start | [code, device] `FUN_8012ced0` |
| All voices of a track add into one zeroed mono buffer; the host frees a voice when `env->stage` is 0 after `render` | [code, app] |
| Effects: `renderFX` in place on stereo buffers; `pitchBend` = cents / 200 in Q31; `gain` from the scenario | in place [code, app]; bend scale [idea] |
| FX `mem`: the engine's `memRequest`, capped at the track's pool (FX1 2 MB, FX2 256 KB, tape 2 MB, master 16 KB, from `opz_engine.h`). It starts filled with `0x5a`, not zeros: the pool is reused between plugs | pool sizes [code, device]; fill [idea] |
| Events are applied at the first block boundary at or after their time | [idea] |
| The host applies no level or pan to an instrument track, as the OS doesn't: the engine applies `params[11]`/`[10]` itself (`opz_mix.h`; golden.py checks every instrument engine: level 128 = half, pan 0/255 = one side silent, level 0 = silent) [code, app + device; engine-abi.md §4]. The mixer's track and group gains are not modelled, and `--fx` feeds the effect the whole instrument mix (a 100 % send, no dry path). LFO, portamento and vibrato are not modelled. The OS smooths only params 0–11 (with its own rounding), lets 12–17 jump, latches all 18 at every note-on and passes 1024 on drum tracks: opzsim differs in all four ([param-paths.md](../research/param-paths.md); the cycle driver's streams, `tools/opz_params.py`, model them) | — |

### Fake `ModuleCallbackInterface`

| Entry | opzsim |
|---|---|
| `sequencerGetValue` | id 2 → 0, id 3 → the flag set by id 3, **id 4 → tempo in whole BPM**, **5–7 → a sequencer clock** (playing, global step, tick 0–23: 24 ticks per step at the scenario tempo, run between blocks, one tick ahead of the position played, the step wrapping at 16; playing from the start, `transport` to stop and play), 8 → 1, others −1; any id can be overridden with `seq` |
| `sequencerSetValue` | stores ids 0–3 (flag, tape ducker, FX timing, track flag); `-v` logs them |
| `g_sine`, `g_sineTable`, `k_lp_table`, `rFilterCoeffs`, `releaseTable` | the device formats (8,193 × Q31 sine, 256 × int16 sine, 129 one-pole coefficients, 5 pointers to `[9][65]` biquad tables, 128 decay multipliers), generated from our formulas by `sdk/include/opz_tables_gen.h` (engine-abi.md §5, Q-15) |
| `updateParametersADSR` | linear segments, see below |
| `updateParametersAHD` | the decoded drum envelope (positions in, linear step), see below |
| `getSample` | `getSample(packId, note)` as on the device: the attached pack with that id, slice `(note + 19) mod 24` clamped to its keys (a synth sample has one), as an `OpzSampleSlice` with every word as the OS loader builds it (data, frames, knobs, rate, loop points, pitch, editable; `tools/opz_sampleplay.py`, Q-26); **NULL** for id 0 or an unknown id. It ignores `seq 8` (on the device that key only marks live sample editing). A note on a track whose pack is `unloaded` is **dropped before `onNoteOn`**, as the OS does when it can't lock the pack (printed once per track). `sample builtin` serves the old test buffer (44,100 × int16, decaying 440 Hz sine at −6 dBFS) as a one-key pack |
| `getPresetData` | reads up to 4,096 bytes from the scenario's `preset_file`; false without one |
| `IIRFilter_*` | pass-through (warning) |

The tables follow the device formats (Q-15 answered): our formulas regenerate OS 1.2.45's tables to within 1 LSB
(`k_lp_table` 11), so an engine that reads them through `opz_tables.h` sounds the same here as on the device. They
are computed, never copied from TE. The host check `tables_probe` compares them with `tools/opz_tables.py`.

Tables and pitch are computed with our own libm-free `sin`/`exp2`, and everything is built with
`-ffp-contract=off`, so renders are bit-identical on Apple silicon and Intel (checked: x86_64 under Rosetta gives
the same hashes).

### Envelope model (decoded)

opzsim's `updateParametersADSR`, note-on and note-off follow the device model in
[sdk/docs/engine-abi.md §3](../sdk/docs/engine-abi.md) exactly (stage machine, thresholds, sustain step, release
flag), and engines step it with `opz_env_tick()` from [sdk/include/opz_adsr.h](../sdk/include/opz_adsr.h). The
attack and decay/release rate curves are our own fits to the shape of TE's tables, so times are close but not
bit-identical. A note on a busy voice fades it first (stage 5) and starts when it is idle, as on the device.
`updateParametersAHD` follows the decoded drum envelope exactly (stage 1 attack, 7 hold, 4 decay, 0 done; same
arithmetic as the app): its arguments are the engine's play position and the landmarks from `opz_ahd_layout()`,
and engines step the level with `opz_ahd_tick()`. The host check `ahd_probe` compares it sample by sample with an
independent model of the ABI document.

Real release times are long: release byte 40 ≈ 0.9 s, 90 ≈ 2 s, 128 ≈ 3 s, 200 ≈ 10 s to −66 dB. Give scenarios
enough tail before `expect idle`.

## Budgets and ABI checks

- **State**: each voice gets 280 bytes and each FX record 200 bytes, followed by a 256-byte guard. Any write into
  the guard by `init`, `onNoteOn`, `onNoteOff`, `render` or `renderFX` is an error (exit 4). State starts filled
  with `0x5a`, not zero: the host does not clear it. Pointers are 8 bytes on the Mac and 4 on the device, so a
  state struct holding pointers is larger here.
- **Adds, never overwrites**: each voice is first rendered into a probe buffer pre-filled with a constant, then
  its state and envelope are restored and it renders for real into the shared track buffer. The two
  contributions must match (exit 4 otherwise). This also catches engines that keep per-voice data in globals.
- **Static size**: opzsim reads the dylib's Mach-O sections (`__text`, constants, strings, `__data`, `__bss`) plus
  an export-table allowance and warns when the sum exceeds the slot class (50/10/5 KB). It is an estimate:
  arm64 code is not Blackfin code (the SDK's fractional helpers are single Blackfin instructions but longer arm64
  sequences, so it now overshoots `kit`). It also warns about static constructors and about imports such as
  `memset` (there is no libc on the device). `make -C sim test` builds each engine for the device too when the
  Blackfin toolchain is installed; an engine whose device build fits its slot does not fail on the estimate.
- **FX init**: the OS masks interrupts around an effect's `init` [code, device]. opzsim warns when `init` changes
  more than 4 KB of `mem` or takes over 1 ms of host time; clear delay lines progressively in `renderFX`.
- **CPU**: host nanoseconds per sample per voice (instruments) or per sample (effects). Not Blackfin cycles: for
  those, `make -C sim cycles` (or `tools/opz_cycles.py`) runs the device build in the GNU Blackfin simulator
  ([cycle-model.md](../research/cycle-model.md)).
- **Voices**: report of voices used per track, notes that stole a voice, and voices still active at the end
  (an envelope that never reaches stage 0 holds its voice forever).

## Scenario format (`.scn`)

One directive per line; `#` starts a comment. Steps are sixteenth notes at the scenario tempo and may be
fractional. Defaults: tempo 120, 16 steps, 1 s tail, track `lead` for instruments and `fx1` for effects,
parameters 0 except sustain 255, pan 128, level 200, filter 255.

| Directive | Meaning |
|---|---|
| `tempo BPM` | 20–400; also what `sequencerGetValue(4)` returns |
| `steps N`, `tail SECONDS` | render length: N steps plus the tail |
| `track NAME\|1-16` | track for the following lines: `kick snare perc sample bass lead arp chord fx1 fx2 tape master` |
| `preset b0 … b17` | the 18 parameter bytes, loaded unsmoothed |
| `param NAME VALUE [STEP]` | set one byte; names `p1 p2 attack decay sustain release fx1 fx2 filter resonance pan level portamento lfo_depth lfo_speed lfo_target lfo_shape note_style` or 0–17 |
| `sweep NAME FROM TO START END` | linear byte ramp between two steps, applied per block |
| `note STEP NOTE LENGTH [VELOCITY]` | `NOTE` is a MIDI number or a name (`C4` = 60, `A#3`, `Eb2`); velocity 1–127, default 100. On an FX track it calls the effect's `onNoteOn`/`onNoteOff` |
| `bend STEP CENTS` | track pitch bend, −200…200 |
| `gain VALUE` | FX `gain` argument, 0–1 |
| `input KIND …` | effect input: `silence`, `sine F AMP`, `saw F AMP`, `noise AMP`, `sweep F0 F1 AMP` (linear), `impulse EVERY_STEPS AMP`, `instrument` (default with `--fx`) |
| `preset_file PATH` | file served by `getPresetData`, relative to the scenario |
| `sample FILE\|builtin\|unloaded [SAMPLES_PATH]` | attach a pack to the current instrument track: an OP-1 format `.aif` (relative to the scenario; parsed like `tools/opz_samples.py`: drum packs with 24 start/end positions, 2³¹ = 529,200 frames, synth samples (`"type": "sampler"`, 8 knobs, `base_freq`) as one slice, or a plain AIFF as one slice up to 6 s), `builtin`, or `unloaded` (the plug names a pack the loader has not loaded: `init` gets its id, then every note is dropped before `onNoteOn`). `SAMPLES_PATH` is the plug's `samples` path, default `samplepacks/user/` + the file name; its crc32 is the pack id passed to `init` and expected by `getSample`. `make -C sim` writes test packs to `sim/build/samples/` (`tests/testkit.py`) |
| `seq ID VALUE` | override `sequencerGetValue` for id 0–15 |
| `transport STEP play\|stop` | sequencer clock (keys 5–7): stop as the OS does (the step jumps to 0 or 15, the counter runs on), or play from (0, 0). Notes stay where the scenario puts them |
| `expect sound FROM TO` | RMS above −60 dBFS between two steps |
| `expect silence FROM TO` | peak below −90 dBFS between two steps |
| `expect idle STEP` | every voice freed by that step |
| `expect unchanged` | effect output bit-identical to its input |
| `expect peak_below DBFS` | whole render |

## Golden tests

Each engine keeps `tests/*.scn` and, per scenario, `tests/golden/<scenario>.wav.sha256` (exact) and
`tests/golden/<scenario>.spectrum.json` (octave bands and RMS). `make -C sim test` fails on any hash change or on
a warning from opzsim. On a hash change it prints the spectral distance;
`python3 sim/tests/golden.py --build sim/build --spectral --tolerance 0.5` accepts renders within that many dB,
for refactors that only change rounding. WAVs go to `sim/build/renders/` (git-ignored).

The host checks in the same run use fixture engines in [tests/fixtures](tests/fixtures): an engine with a
missing export is refused, a state overflow and a buffer overwrite are errors, and parameter smoothing and pitch
match a model of the ABI document sample for sample, an effect on master is capped at 16 KB with its heavy
`init` flagged, `getSample` serves every key of four test packs (`sample_probe`: pack id, key mapping and clamping,
and each slice word against `tools/opz_sampleplay.py`, synth sample included), and the sequencer clock (keys 4–7) and `OpzClock` ([opz_tempo.h](../sdk/include/opz_tempo.h)) match
the model of the ABI document's "Sequencer keys" block for block through play, stop and play.

## Web simulator

`sim/web` plays engines in the browser with this host: **eight instrument tracks** (kick … chord, OP-Z tracks 1–8,
each its own engine or none) and an **effect on FX1** and one **on FX2**, each with presets and knobs (P1/P2 with the
plug's names); a section lists only the engines whose trackmask has its track. A strip of track tabs shows each
track's engine, MIDI channel, activity and level; the keyboard (computer keys, on-screen) plays the selected track.
**Web MIDI plays every track on its own channel** (OP-Z default: track + 1), so an OP-Z drives the page
multitimbrally, and its dial CCs move the matching knobs (`sim/web/src/midimap.ts`; MIDI learn for any other
control; a virtual OP-Z for trying it offline; evidence and the device check in
opz-as-controller.md). Each track's FX1 send (byte 6) and FX2 send (byte 7)
feed the two effects, shown as each effect section's *Send* knob (the selected track's) next to that effect's *Dry
level* (byte 12). An effect can run on a generator instead (saw, sine, noise, sweep, impulses), as both do with no
instrument. Below: scope and meter, the host checks for the selected track and the effects, and their scenarios.
Setup:
[QUICKSTART.md](../sdk/docs/QUICKSTART.md#play-it-in-the-browser).

```bash
make -C sim wasm                      # sim/build/wasm/engines/NAME.wasm, templates/{synth,drum,fx}.wasm
npm --prefix sim/web run dev          # http://localhost:5174 (the `websim` entry in .claude/launch.json)
npm --prefix sim/web test             # every scenario in wasm against the golden hashes, live = scenario
npm --prefix sim/web run test:e2e     # headless Chrome: templates, swirl, fm2, kit by keyboard; tracks through FX1/FX2; MIDI routing and learn
```

How it is built:

- **One WebAssembly module per engine**, the engine linked with the host sources in `src/` (built with WASI's libc
  for the host's `printf`, `strtod` and file reading; engines still get `-ffreestanding` and no libc). The same
  `host.c` block loop runs in both builds: opzsim's `main` drives it from a scenario, `wasm/websim.c` from the page.
  `engine.c` takes the five exports from the link (weak symbols) instead of `dlopen`.
- **Scenarios** run opzsim's own `main` (`-Dmain=opzsim_main`) on a fresh instance over an in-memory file system
  (`sim/web/src/wasi.ts`): same arguments, same log, same WAV. `sim/web/test/golden.test.ts` renders every
  `engines/*/tests/*.scn` in wasm and compares the SHA-256 with the golden file: all 190 scenarios of the 33 engines are identical [proven,
  2026-10-27], so the browser sounds bit-identical to opzsim. `-ffp-contract=off` and the libm-free `sim_sin` /
  `sim_exp2` keep the double maths the same; wasm has no fused multiply-add.
- **Live play** (`wasm/websim.c`): the page opens one track, keys and knobs arrive between 64-frame blocks as
  `host_event` / `host_set_param` (a preset loads unsmoothed, a knob is smoothed per block), and each block runs
  `host_block` with the add check and the state guards on. `sim/web/test/live.test.ts` replays scenarios through
  this path and gets the same samples as the scenario render.
- **Tracks through FX1 and FX2**: up to ten modules, each with its own host. Per block every instrument track
  renders; each track's sends are summed into each effect's input (`IN_EXTERNAL`), in parallel, and the tracks' dry
  paths are added to both returns
  (`sim/web/src/mixer.ts`). The mixer is the web simulator's model, not opzsim's: a send **crossfades** (raising it
  moves the synth from the dry path into that effect [proven, engine-abi.md]), an effect's byte 12 at 255 keeps the
  dry path (send/return) [code, app; plug-format.md], the returns get no pan or level [code, app]; the curves are
  linear and two sends' shares of the dry path multiply [inferred], and the OS's track filter (bytes 8/9) and the
  returns' DC blocker are not modelled. Gains ramp over a few blocks, so a knob does not
  click.
- **Audio**: an AudioWorklet at 44.1 kHz (the context is created at that rate) cuts each render quantum into 64-frame
  blocks; Q31 out of the host, float to the speakers, nothing in between (no pan or level, as opzsim).

Differences from the CLI:

| | opzsim CLI | web simulator |
|---|---|---|
| Pointer size | 8 bytes (state structs with pointers are larger) | 4 bytes, as on the device |
| Static size check | Mach-O estimate | not measured (prints so); `tools/opz_build.py` measures the device build |
| Host CPU | `clock_gettime` | WASI clock: `performance.now()` on the page; Chrome's AudioWorklet has only `Date.now()`, so live figures are averages since load. Neither is Blackfin cycles (`tools/opz_cycles.py`) |
| `input sweep` (live) | once over the scenario | repeats every 4 s |
| Engines per run | `-e` plus `--fx` (the instrument mix straight into the effect) | one per module; live, up to eight instruments and effects on FX1 and FX2 through the send model above (`--fx` scenarios are not run in the page; none of the goldens use it) |
| Tracks (live) | every track a scenario names | tracks 1–8 (one engine each), FX1 and FX2 |

## Adding an engine

Quickest: `python3 tools/opz.py new <name> --kind synth|drum|fx` starts one from a template, with its slot and plug
id ([QUICKSTART.md](../sdk/docs/QUICKSTART.md)). By hand:

1. `engines/<name>/plug.json` ([plug-format.md](../sdk/docs/plug-format.md)) and `<name>.c` against
   `sdk/include/opz_engine.h` (and `opz_adsr.h` for instruments). Add a
   `_Static_assert(sizeof(State) <= OPZ_VOICE_STATE_BYTES)` (or `OPZ_FX_STATE_BYTES`).
2. `engines/<name>/tests/*.scn` covering the knobs at 0 and 255, sweeps, and `expect` lines.
3. `make -C sim golden ENGINE=<name>` (only that engine; without `ENGINE` every engine is re-rendered), listen to
   `sim/build/renders/<name>/*.wav`, commit the golden files.
