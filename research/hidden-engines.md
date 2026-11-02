# TE's unreleased engine classes (Q-11)

What the desktop app's `Lead`, `DxFm`, `Fm` and `Swirl` classes are, how the app chooses them, and whether the
device knows them. Offline only: the desktop app `OP-Z.app` 1.3.1 (`SnakePlugin`, arm64 slice, symbols present;
addresses are offsets in that slice), its Unity assembly and `StreamingAssets/factory-plug-cache`, `opz.ldr` 1.2.45
and its `system/content` (device addresses are load addresses), and the user's own `plugs.json` from the restore
backups (read only). No device command was sent.

This note builds on the uncommitted P0 note `p0-loading-flags.md` (session `p0-loading-flags`, 2026-10-18, main
checkout only). Its Q-11 claims were re-checked here against the binary; the new findings are the factory preset
cache's orphan plug records (§3) and the comparison with our engines (§4). Its Q-08 part is superseded by
[host-tables-flags.md](host-tables-flags.md).

Behaviour is described in our own words. No TE code, tables or patch data are reproduced (CONTRIBUTING); the
decompiled listings stay in the git-ignored `build/ghidra/p0-loading-flags/`.

## Summary

| Question | Answer |
|---|---|
| How does the app choose `Lead`/`DxFm`/`Fm`/`Swirl`? | **It doesn't.** The app's engine registry is filled for `engines/tapetwo.engine` only, and its instrument note-on is an empty function, so no TE instrument (shipped or not) ever plays in the app. The four classes come in with every other engine through one `AllEngines.cpp` unit; nothing calls them [code] |
| Which classes have no device engine? | Exactly these four. The app has 25 engine classes; 21 match TE's shipped engines (13 in the 1.2.45 package, 8 from the catalogue) [code, data] |
| Engine paths, plug ids, parameter names, track masks, presets? | **None** for the four classes, anywhere: app strings, Unity assembly, factory plug cache, firmware content, `opz.ldr`, the user's device [code, data] |
| Hidden ids that do exist | TE's factory preset cache has empty records for plug ids **15, 16, 42, 66, 67**, which no shipped plug uses; the user's device lists TE plug **66 → `engines/fmedit.engine`**, an engine that was never released. Nothing ties them to the four classes [data] |
| Can the device enable them? | **No.** No `.engine` exists for them, the OS has no name, id or slot for them, and no flag or setting in the OS points at them. There is nothing to switch on [code, data] |
| What are they? | `Lead`, `DxFm`: **4-operator** FM, 11 algorithms, 8 built-in patches picked by P1, P2 unused — `ep`'s engine without its P2. `Fm`: a three-sine FM voice, an early form of the shipped `uranus` (`fm003b`). `Swirl`: **not a phaser** but a pitch-shifting, modulated delay ambience whose interval is set by P1 or by keys on its FX track [code] |

## 1. The app's engine path

| Step | Address | What it does [code, app] |
|---|---|---|
| `makeHash(path)` | `0x43790` | zlib CRC-32 of the path string (0 for an empty string) |
| `PlugManager::loadRequestedEngines` | `0x45744` | walks the set of requested engine paths, inserts `hash → LoadedEngine` into the map at manager `+8`. Only the key is set: no function pointers are looked up |
| `PlugManager::init` | `0x44adc` | the **only** place that fills a `LoadedEngine`: key = hash of `engines/tapetwo.engine`, pointers to `TapeTwo::init`, `renderFX`, `onNoteOn`, `onNoteOff` (PC-relative `adr` at `0x44b04`–`0x44b20`), then `TapeTwo::initModule` (`0x44b4c`) |
| `PlugManager::getPlugEngine` | `0x46fec` | plug id → plug record → engine hash (record `+8`) → `LoadedEngine`. Single caller: `FxManager::noteOn` |
| `InstrumentManager::noteOn` / `noteOff` / `killNote` | `0x3bfa8` / `0x3bfac` / `0x3bfb0` | each a bare `ret` |
| `Voice::renderVoice` | `0x70dd0` | calls the voice's render pointer (`voice+8`, `blr` at `0x70ef0`); never set, since no voice starts |

Checks that nothing else selects the classes [code, app]:

- No PC-relative address computation (`adrp`+`add`/`ldr`, `adr`) in `__text` resolves to any engine class's
  `initModule`, `init`, `render`/`renderFX`, `onNoteOn` or `onNoteOff`, except the four `TapeTwo` ones above.
- No 8-byte word in `__DATA_CONST` or `__DATA` points at any of those functions (scan of both segments, chained
  fixup targets).
- No direct `bl`/`b` into those functions from outside their own class.
- The per-class `::interface` symbols are not a registry: each is the class's saved pointer to the host callback
  table, written by its own `initModule` (e.g. `Swirl::initModule` `0x26268`).
- `__GLOBAL__sub_I_AllEngines.cpp` (`0x27140`) only initialises the classes' constant tables.
- Strings: the only engine paths in `SnakePlugin` are `tapetwo`, `sampleplayergain`, `synthsampler`; the Unity
  assembly adds `engines/sampleplayer.engine`. Its `lead` is the track name and `swirlAmplitude` a visual
  animation property.

So in this build the app's audio side is a sequencer and mixer with the tape effect; the engine classes ride along
because the symbols are exported [code]. Why TE ships them is unknown: probably one shared source tree for device
engines and desktop builds [inferred].

### Class inventory

| | Classes [code, app symbols] |
|---|---|
| Shipped, in the 1.2.45 package (13) | AnalogA, Chorus, Chorus2, Cluster, Crush, Delay, Dist, EP, FdnReverb, Organ, Reverb, SamplePlayerGain, SynthSampler |
| Shipped through TE's catalogue (8) | Chord (`shade`), Digital, Electric, Fm003b (`uranus`), MiniStringShort (`bow`), Saw013a (`saw`), TapeTwo (`tape`), Volt |
| **No device engine (4)** | **Lead, DxFm, Fm, Swirl** |

Catalogue names from the app's factory plug cache [data]. TE's live DLC list (`/dlcs`) is fetched from TE's server
and is not on this Mac; the factory cache (22 bundles, 35 plugs) and the user's device list are the local evidence.

## 2. The device side

- `opz.ldr` has no class or engine name for the four (`lead` and `6-lead` are track names); its engine paths are
  `sampleplayergain`, `synthsampler` and the content importer's `%d.engine` / `%s%d.engine` [code, strings].
- The OS's fallback slot table (`0x800ff150`, used when `slotConfiguration.json` fails to load) writes one plug id
  per track: 13, 14, 15, 17, 21, 25, 28, 31 (tracks 0–7), 34, 36, 37, 39 (FX1, FX2, tape, master), 60, 70, 80, 100.
  15, 21, 28, 36, 37, 60, 70, 80 and 100 match no plug in use today; they sit on drum, FX and control tracks, so
  they read as early-firmware defaults, not as the four classes [code; meaning inferred].
- Hidden switches: `g_renderToneMode` (device byte `0x81059d21`) only selects `rendertone.preset` in
  `getPresetData`, skips the CPU-overload voice kill and skips a save; nothing writes it [code, P0 note]. The
  `general.json` / `midi.json` keys and the SysEx text commands ([04](architecture/04-hidden-and-unreleased.md))
  have nothing to do with engines. **No flag enables an unreleased engine**: there is no device code for one.

## 3. Ids that exist without a released engine

| Id | Where | What we know |
|---|---|---|
| 15, 16 | factory preset cache `settings/plugPresets.dat` (1.2.45) | Empty records (no names, values or masks) between organ 14 and ep 17. 15 is also in the OS's fallback table, on track 2 (perc) [data, code] |
| 42 | same | Empty record between chorus 39 and fdnReverb 43 [data] |
| 66 | same, and the user's device `plugs.json` | Empty preset record; the device's plug list has **`{"id": 66, "engine": "engines/fmedit.engine", "flags": 0, "version": 1}`** with no matching `engines[]` entry, so it never loads (present in the first restore backup, 2026-10-17, before any install of ours) [data] |
| 67 | factory preset cache | Empty record [data] |
| 146–149 | factory cache, device | 146 `dmx basic`, 147 `performance basic`: plugs without an engine for the control tracks; 148, 149 the same shape [data] |

The cache's records are in id order with one slot per plug the build knew, so TE's own plug list had these ids when
the cache was written [data; reading inferred]. Which engines they were is not recorded. Plug 66's path, next to the
`fmedit.preset` hook (callback `+0x28`), points to an FM patch-editing build of the FM family (EP/Organ/Lead/DxFm),
never released [inferred].

**Caution for us:** never name an engine of ours `engines/fmedit.engine`. The user's device already has TE plug 66
pointing at that path; installing a file there would bring that plug to life with parameters and a track mask we do
not control [inferred]. Our ids (60000+) do not collide with any of the ids above.

## 4. The four classes, behaviour only

No plug exists for any of them, so TE never gave them knob names. P1/P2 below are the parameter words the classes
read.

### `Lead` and `DxFm` (`0x1d65c…`, `0x12f54…`)

- **Four operators** (operator loops of 4; 460-byte patches), each with a five-stage envelope (attack, decay 1,
  decay 2, sustain, release, then idle), plus a pitch envelope [code].
- **P1's top three bits pick one of 8 built-in patches** (`lsr #28` of P1 at `0x1d754`). **P2 is never read**.
  Attack/decay/sustain/release go to the host ADSR, pan and level are applied by the engine, filter and resonance
  stay the OS's [code: loads from the params pointer in `Lead::render`, `DxFm::render`].
- Each patch: an algorithm 0–10 (11 algorithms), a transpose of a few octaves, feedback, a sine LFO (rate, fade-in,
  pitch depth, per-operator amplitude depth), and per operator: ratio (coarse/fine/detune or fixed frequency),
  level, velocity sensitivity, rate scaling and DX-style level scaling around a break point [code; grouping partly
  inferred, P0 note].
- High-frequency fades per operator keep bright modulators from aliasing [code; exact use inferred].
- No buffer (`initModule` requests 0 bytes), ~278 bytes of state per voice, scratch used as work buffers
  [code: request checked; sizes from the P0 note].
- `DxFm` differs from `Lead` only in its patch bank, a per-algorithm carrier table and some scaling constants.
- **`EP` is the same engine plus a P2** (it reads `params[1]`; damp below 128, boost above), which is why `ep`
  carries the P2-bipolar flag `0x10` [code]. So `Lead`/`DxFm` are EP's siblings left without a second knob.

### `Fm` (`0x71b8…`)

- Three sines at **1×, 6× and ½×** the note. The ½× one modulates the 6× one; the 6× one, scaled by P2 and by the
  envelope level, modulates the carrier, so the tone darkens as the note decays. P1 sets a recursive smoothing on the
  carrier (warmth), with gain compensation [code, P0 note].
- No buffer (request 0 bytes), ~42 bytes of state, a per-octave loudness table shared in scheme with `Fm003b`
  [code: request checked; the rest from the P0 note].
- `Fm003b` (shipped as `uranus`, plug 129) is the larger relative (4.7 KB render vs 1.6 KB): `Fm` reads as its
  early form [inferred].

### `Swirl` (`0x26260…`)

- Requests **149,800 bytes** of FX memory (`0x26270`): fits FX1 (2 MB) and FX2 (256 KB), not master; ~96 bytes of
  state [code: request checked; state size from the P0 note].
- Signal flow: four long delay lines (~215, 74, 75, 215 ms) read with slow sine modulation (two LFOs, ~1.1 Hz and
  ~0.25 Hz, ±32 samples, depth from P2), mixed through a 4 × 4 sum/difference matrix with fixed one-pole damping;
  a 186 ms line used as a **two-tap crossfading pitch shifter** in the loop; two ~43 ms taps for width. Feedback
  per matrix term rises from 0.25 to 0.5 with P2. The output is the **pitch-shifted signal plus half of the short
  taps**, written (not added) to the buffer [code: delay lengths and memory request checked; P0 note for the rest].
- **Shift:** with no key held, P1 sets it on a cubic curve with no shift at 128 (small values detune, large ones
  move by intervals); the FX track's pitch bend adds to it. A held key on the FX track overrides P1 with an interval
  (lower 12 keys down, upper 12 keys +1…+12 semitones); releasing returns to P1. Changes are smoothed per block.
  Downward intervals come out at about half size (lowest key about −5 semitones) [code; whether intended unknown].
- So `Swirl` is a **pitch-shifted, moving ambience**: P1 = shift (detune → harmonize), P2 = size/feedback and
  motion, keys play intervals momentarily [code/inferred].

## 5. Compared with our engines

| TE class | Ours | Difference | Verdict |
|---|---|---|---|
| `DxFm` | `dx` (plug 61013, 6-op, 32 algorithms, 32 DX7-format patches, P1 patch, P2 brightness) | Ours has 2 more operators, the DX7's full algorithm set, 4× the patches and a P2. TE's has DX-style key scaling and a pitch envelope per patch, which DX7 patches also carry | **No new engine**: `dx` already exceeds it |
| `Lead` | `dx` | Same engine as `DxFm` with a lead bank | **No new engine.** At most a lead-oriented bank for `dx` (backlog S-20) |
| `Fm` | `fm2` (plug 61011, 2-op + feedback, P1 ratio, P2 index × envelope) | TE's is a fixed 1 : 6 : ½ three-sine voice with a warmth knob; its idea (index following the envelope) is already in `fm2`, and its successor shipped as `uranus` | **No new engine.** Optional: a 6× "tine" ratio step with a ½× sub-modulator in `fm2` (backlog S-21) |
| `Swirl` | `swirl` (plug 61001: phaser/flanger, designed from the name alone); `shimmer` (plug 61008: Dattorro plate with a shifter in the loop, latched intervals) | TE's `Swirl` is neither: short modulated delay network, output mostly the shifted signal, P1 a continuous detune → interval knob, keys momentary. Ours stay valid as what they are | **Worth a small clean-room effect** (backlog X-11 `harmonizer`, now specified in this spirit): a continuous shift knob and momentary key intervals are something neither `swirl` nor `shimmer` does |

## 6. What is still open

- TE's live DLC list (`/dlcs`) was not checked; it is fetched at run time and is not on this Mac. Any plug for the
  four classes would have to come from there, and the factory cache shows none.
- The engines behind plug ids 15, 16, 42 and 67 are unknown.
- Hearing the four classes would need a local harness that `dlopen`s `SnakePlugin`, calls the exported, mangled
  `initModule`/`init`/`onNoteOn`/`render` with a fake callback table and renders WAVs. Renders stay local
  (CONTRIBUTING). Not needed for the verdicts above.
