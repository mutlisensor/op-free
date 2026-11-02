# 04 — Hidden and unreleased

What exists in the firmware or the app but is not on the OP-Z's menus. Each item has an unlock route in
docs/roadmap/unlock-plan.md.

## Unreleased engines (in the desktop app, not on the device)

The desktop app's `SnakePlugin` contains every shipped engine as a C++ class, plus four that have **no `.engine`
file** in the firmware package or in TE's public catalogue [code]:

| Class | Kind | Size of render code (arm64) | What the code shows |
|---|---|---|---|
| `Lead` | instrument | 21.6 KB | **4-operator** FM, 11 algorithms, per-operator 5-stage envelopes (`ATTACK, DECAY1, DECAY2, SUSTAIN, RELEASE, IDLE`), 8 built-in patches (3,680 bytes) picked by P1; P2 not read |
| `DxFm` | instrument | 21.5 KB | Same engine as `Lead` with its own patch bank, plus `g_synthPresetTweaks` (written, never read) |
| `Fm` | instrument | 1.6 KB | Three sines at 1 : 6 : ½ of the note, modulation depth from P2 × envelope, P1 warmth; early form of the shipped `uranus` (`fm003b`) |
| `Swirl` | effect | 2.7 KB `renderFX` | **Not a phaser:** four modulated delay lines with a pitch shifter in the loop (~150 KB buffer); P1 or a key held on its FX track sets the shift, P2 feedback and motion |

**`Lead` and `DxFm` are siblings of the shipped `ep` engine.** `EP` has the same helper functions, the same
`phaseNames` list and a preset table of exactly the same size (3,680 bytes). `ep.engine` is 15 KB and lives in a
50 KB slot, so a `Lead` or `DxFm` build would fit the same slot class. [code/inferred]

**The app never runs them (Q-11, answered).** Its engine registry is filled for `engines/tapetwo.engine` only and
its instrument note-on is an empty function, so no TE instrument plays in the app; the four classes are linked in
with all the others (`AllEngines.cpp`) and nothing calls them. No engine path, plug id, knob names or presets exist
for them in the app, the firmware, `opz.ldr` or the user's device, and no device setting can enable them [code,
data]. TE's factory preset cache does hold empty records for plug ids 15, 16, 42, 66 and 67, and the device lists
TE plug 66 → `engines/fmedit.engine`, never released [data]. Details, behaviour and verdicts:
[hidden-engines.md](../hidden-engines.md).

## Hidden preset files: `fmedit.preset`, `rendertone.preset`

Both the device OS and the app contain, inside `PlugManager.cpp`'s strings [code]:

```text
rendertone.preset
%sengines/fmedit.preset
```

Next to them are `Failed to read all bytes: %s` and `Wrong file format: %s (%d != %d)`, so the OS reads these as
binary files with a format check. The app also has `g_referenceToneLevel`, `g_referenceToneNote`,
`g_referenceToneFrequency`. Most likely:

- **`fmedit.preset`** — a developer hook that loads an FM patch from `engines/fmedit.preset`, overriding the
  built-in preset tables of the FM family (EP, Organ, Lead, DxFm). That would be a **patch-editor entry point**.
- **`rendertone.preset`** — a reference/test tone patch.

**Answered from the app [code]:** both strings are used by `getPresetData(int, int, void *out)`, which is entry
`+0x28` of the callback table every engine receives. It reads up to 4,096 bytes from `engines/fmedit.preset`, or
from `rendertone.preset` when a global mode flag is set, into the engine's buffer. So any engine can ask the host
for an editable patch file. No engine calls it — not in the app, and not among the 13 device engines checked with Ghidra (including `ep`
and `organ`). So TE's patch-editing hook is dormant; **our own engines can use it** to load an editable patch
from `engines/fmedit.preset`, which makes an FM patch editor for a custom `dx` engine straightforward.

## Hidden text commands over SysEx

`Midi.cpp` accepts `F0 00 20 76 01 62 <~len & 0x7F> <ascii> F7` [notes]:

| Command | Effect |
|---|---|
| `enable-verbose-log` / `disable-verbose-log` | More logging — **useful for engine CPU and load errors** |
| `enable-debug` / `disable-debug` | Debug mode flag |
| `enable-uart-log` / `disable-uart-log` | Route the log to UART |
| `enable-controller-mode` / `disable-controller-mode` | Controller mode flag |
| `enable-battery-log` / `disable-battery-log` | Battery logging |
| `restart-into-content`, `restart-into-synth`, `restart-into-bootloader` | Software mode switch |
| `debug-crash`, `debug-save` | Developer fault injection — **never send** |

## The Score context's test panel

A hidden sequencer context (`ScoreContext.cpp`) contains a **signal generator** with a 6 dB step attenuator and an
**audio-routing A/B switch** that rewires the audio graph and two GPIO lines [notes]. These are factory test
features. They show that the OS has a reconfigurable internal audio graph (`FUN_80023bb8(ctx, from, to, gain)`),
which is relevant if we ever want new routing.

## `general.json` options

All of these exist in `config/general.json` and are user-editable in content mode, though several are not exposed
in TE's UI [code]:

`backlit_keys`, `disable_start_sound`, `disable_microphone_mode`, `disable_track_preview`,
`disable_param_page_reset`, `latch_notes_with_shift`, `temp_param_add_fx_a`, `disable_headphone_db_reduction`,
`generous_chords` (6 chord voices instead of 4), `legacy_input_select`.

`config/midi.json` likewise has keys the app does not surface: `channel_one_to_active`, `alt_program_change`,
`parameter_cc_out`, `midi_echo`, `track_channels`, `track_enable` and others [notes].

## Hardware capabilities beyond the menus

| Capability | Evidence | Why it matters |
|---|---|---|
| **USB host audio, MIDI and hubs** | `USBH_AudioPlaybackMutex`, `USBH_AudioRecordMutex`, `USBH_AudioMIDIMutex`, `USBH_ExtHubMutex` [notes] | External audio interfaces could carry engine output or input |
| **Module port audio** | `enable module audio`, `set 16 bit audio for module I2S`, `set 32 bit audio for module I2S`, `got module state … jack:[…] mic:%d head:%d boom:%d` [code] | The oplab/module port carries I2S audio: an audio input for effects, a second output |
| **DMX out over USB** | `how_to_dmx.txt`, FTDI host driver [notes] | Lights already follow the sequencer |
| **Microphone / line input** | sampler, `disable_microphone_mode`, `legacy_input_select` | Today it feeds the sampler; routing it through FX engines is an open question — Q-13 |

## Engines on drum tracks

Drum tracks use `sampleplayergain.engine` with `trackmask` `0x1`–`0x8`. Nothing in the loader limits an instrument
engine to the synth tracks: the mask is a plug field. **A synthesized drum engine with `trackmask 0x7` should be
assignable to kick, snare and perc.** Untested — Q-14.
