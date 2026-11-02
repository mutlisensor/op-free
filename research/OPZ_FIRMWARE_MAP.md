# OP-Z firmware map — and how new synths and effects fit in

> Evidence-level reference with function addresses. For the streamlined explanation start at the
> [README](../README.md) and [research/architecture](architecture/). Plans now live in docs/roadmap;
> sections 10 and 11 below are superseded by it and by docs/ideas.

A streamlined picture of the OP-Z firmware, built from the decrypted 1.2.45 package, the installed
bootloader dump, the desktop app (`OP-Z.app` 1.3.1, `SnakePlugin.bundle`), and the
[opz-firmware-notes](https://github.com/ioma8/opz-firmware-notes) research. Everything marked
**[code]** was read from decompiled code in this session; **[notes]** comes from the notes repo;
**[inferred]** is a reasoned conclusion not yet tested on hardware.

---

## 1. The short version

- The OP-Z's synths and effects are **plug-in DSP modules** ("engines", `*.engine`) that the OS loads at
  runtime from `/yaffs2/user/engines/`. Factory engines are delivered the same way as DLC.
- An engine is a plain **ADI Blackfin boot stream** (LDR), linked to a **fixed address** inside a
  **memory-slot window**. The loader checks only header checksums, the address window, and five exported
  symbols. **There is no signature or CRC on engines.** [code]
- An engine is reached through a **plug**: a JSON record naming the engine, its memory slot, which
  tracks may use it, two parameter names, and presets.
- Plugs and engines are installed in **normal mode over MIDI SysEx** with the companion file-transfer
  protocol: upload `engines/<name>.engine` plus a `settings/patch.json` (which triggers plug import) plus an
  updated `settings/slotConfiguration.json`. The notes' author has already loaded **custom engines**
  (`clean-tone.engine`, `researchmute.engine`) this way. [notes]
- So adding synths and effects means **building engines against a known 5-function ABI and shipping them
  as user plugs**. No firmware modification is needed.

---

## 2. Hardware and images

| Part | Chip | Firmware | Role |
|---|---|---|---|
| Main CPU | ADI Blackfin+ (BF70x) | `bootloader.ldr`, `opz.ldr` | OS, sequencer, synth, USB, file system |
| BLE | Nordic nRF52832 | `nrf52832_opz.bin` (+ signed `.dat`) | Bluetooth MIDI / app link |
| Power / keys / LEDs | Cypress PSoC 5LP | `SnakeKeyboardModule_*.cyacd` | charging, USB-PD, key scan, LED driver |

Storage [code/notes]:

| Where | What |
|---|---|
| SPI flash `0x00000` | 41 KB first-stage loader (LDR) |
| SPI flash `0x10000` | main bootloader (on this unit 1.2.8, Mar 24 2020), erased to `0x50000` |
| SPI flash `0x0D000` | 2-byte boot record (`01 01` on this unit) |
| NAND (yaffs2) | `/yaffs2/os.ldr` (the OS), `/yaffs2/user/…` (engines, settings, samples, projects), `/yaffs2/upgrade/…` (install staging) |

Boot modes: plain power-on → normal (synth) mode; SCREEN held → update mode ("OP-Z Boot", CDC serial
console, update volume); TRACK held → content mode ("OP-Z Disk", mass storage).

The `.zfw` update is AES-256-CBC (key not published, IV at `0x70`, filename slot at `0x300`, payload from
`0x400` as one CBC stream) wrapping a ZIP. Install integrity is the per-entry ZIP CRC only. [code]

---

## 3. OS architecture (opz.ldr)

```
 keys / MIDI / BLE / app SysEx
            │
     Sequencer (16 tracks, patterns, step components, master effects)
            │ noteOn / noteOff / parameters
            ▼
          Synth ──► InstrumentManager ──► Voice[n] ──► engine.render()      (tracks 0–7)
            │
            └──────► FxManager ──────────────────────► engine.renderFX()    (FX1, FX2, tape, master)
            │
            ▼
      mixer → codec, VU, DMX / light flush
```

- Two RTOSes: uC/OS-III for the application, SMX for USB. [notes]
- Source tree is internally called `snake` (`C:\snake-software\…`); the desktop app's `SnakePlugin`
  is **the same C++ classes compiled for macOS, with symbols** (Synth, InstrumentManager, FxManager,
  PlugManager, Voice, Lfo, every engine class). That makes it the best reference for host behaviour. [code]

### Tracks and track masks

A plug's `trackmask` is a bitfield with bit *n* = track *n*:

| Bits | Tracks | Used by |
|---|---|---|
| `0x1` `0x2` `0x4` `0x8` | kick, snare, perc, sample | sample-pack plugs (`sampleplayergain`) |
| `0xF0` | bass, lead, arp, chord | synth engines |
| `0x100` / `0x200` | FX1 / FX2 | delay (`0x100`), reverb, dist, crush, chorus-80 (`0x300`) |
| `0x400` | tape | tapetwo |
| `0x800` | master | chorus |

Default voice counts (app tables) [code]: kick/snare/perc/sample 2 each, bass 2, lead 3, arp 2, chord 4
(6 with "generous chords"). The OS kills notes on CPU overload (`cpu overload: kill notes %d`).

---

## 4. Content model: engines, plugs, presets, slots

| File (under `/yaffs2/user/`) | Content |
|---|---|
| `engines/<name>.engine` | engine binaries |
| `settings/plugs.json` | `{"engines":[{path, memoryslot, version}], "plugs":[{id, engine, samples, flags, version}]}` |
| `settings/plugDefinitions.dat` / `plugPresets.dat` | binary caches: 200 plug records × `0x7ac` B; 200 preset records × `0x6dc` B |
| `settings/slotConfiguration.json` | which plug sits in each of the 10 slots per track |
| `settings/patch.json`, `settings/plugs_patch.json` | import requests (`plugs_patch.json` is imported, then deleted) |

**Import path** [code, `PlugManager::importPatch`]: the JSON's `auto_import[]` entries are
`{engine:{path, memoryslot, version}, plug:{id, version, …, presets}, tracks:[…]}`. For each entry it
registers the engine definition (keyed by path, value `(memoryslot << 8) | version`), imports the plug,
and assigns it to the listed tracks. Then top-level `engines[]` and `plugs[]` are processed,
`loadRequestedEngines()` runs, and `plugs.json` is re-exported. A plug is skipped if one with the same id
and an equal or newer version already exists.

**Plug fields** (from TE's DLC catalogue): `id`, `name`, `engine`, `samples`, `trackmask`, `flags`,
`version`, `parameter_names` (the two custom knob names), and `presets`. Each preset has `slot`,
`index`, `default_octave`, a name, and **18 parameter bytes**.

TE plug ids run up to ~152. User plugs created by the companion tooling use **60000+** (60001–60006 seen
in the notes).

---

## 5. The `.engine` file

An engine is an ADI LDR boot stream: a series of 16-byte block headers
`flags u16 | xor-checksum u8 | 0xAD | target u32 | count u32 | argument u32`, each optionally followed
by `count` bytes. All 13 engines in the 1.2.45 package parse this way, every header XORs to zero, and
the walk ends exactly at end of file. (The notes' "unidentified varying bytes" at `0x02`/`0x12` are
these header checksums.) [code]

Typical layout (crush.engine):

| Block | Target | Size | Meaning |
|---|---|---|---|
| FIRST \| IGNORE | `0x11a00000` | 0 | stream header (flags `0x5000`); argument = offset of the FINAL header (file length − 16) |
| data | **window base** | `0x38` | export table |
| data | base + `0x38` | code | `.text` |
| data | … | … | `.data` / constants (including the export name strings) |
| FILL (value 0) | … | … | `.bss` |
| FINAL | `0x11a00000` | 0 | end |

**Export table** (at the window base) [code, `FUN_801419a8`]: two magic words `0xfeed0ad1 0xad1facef`,
then `(name_ptr, target)` pairs ending in a zero name pointer. Example (delay.engine, base
`0x81be4c00`): `initModule → 0x81be4ca8`, `init → 0x81be4cdc`, `renderFX → 0x81be4d08`,
`onNoteOn → 0x81be4c44`, `onNoteOff → 0x81be4c38`.

### What the loader checks [code, `dlm.cpp`: `FUN_8002ae50`, `FUN_8002ac00`, `FUN_8002b02c`]

1. Open `/yaffs2/user/<path>` and read it whole into a heap buffer ("DLM does not fit in memory" if the allocation fails).
2. For every block: byte 3 must be `0xAD` ("Bad header"), the header must XOR to 0 ("Bad header
   checksum"), and the block must lie inside the slot window ("Bad address %08x-%08x (%08x-%08x)").
   FILL blocks must use value 0 ("Unsupported fill value" is logged, and the block is zero-filled anyway).
   The data is then copied to its target and the instruction cache is flushed for the range.
3. Look up `initModule` and call `initModule(0, &moduleCallbackInterface, &out)`; it must return non-zero.
4. Look up `init`, `onNoteOn`, `onNoteOff`, and `render` (or, if absent, `renderFX`). All must exist
   ("Could not lookup symbols for engine %s").

There is no signature, no hash and no version gate on the binary itself.

---

## 6. Memory slots

Each engine definition carries a `memoryslot`, and the OS turns it into a fixed address window
[code, `FUN_800fb556`]:

| Slots | Window per slot | Start address |
|---|---|---|
| 0–7 | 50 KB (`0xC800`) | `0x81B00000 + slot × 0xC800` |
| 8–57 | 10 KB (`0x2800`) | `0x81B64000 + (slot − 8) × 0x2800` |
| 58 and up | 5 KB (`0x1400`) | `0x81BE1000 + (slot − 58) × 0x1400` |

The OS image's static memory ends at `0x81AEF033`, so the engine area begins right after it.

**Slots used by official engines:** ep 4, tapetwo 9, sampleplayergain 10, fm003b 13, saw013a 16,
organ 18, analoga 27, chord 59, chorus 60, delay 61, digital 62, reverb 66, synthsampler 68, volt 70,
electric 77, dist 78, crush 79, ministringshort 82, cluster 83, chorus2 88, fdnreverb 89.

**Consequences**
- An engine is linked for one slot. It cannot be moved to another slot without re-linking (there is no
  relocation data).
- Two engines in the same slot overwrite each other. New engines must use **free slots**: 50 KB slots
  1–3, 5–7 (avoid 0, the fallback when a slot is missing); 10 KB slots 8, 11, 12, 14, 15, 17, 19–26,
  28–57; 5 KB slots 58, 63–65, 67, 69, 71–76, 80, 81, 84–87.
- Above slot 89: the loader accepts slots up to 255 (the slot is one byte) and nothing in the OS refers to the
  memory past `0x81C09000` except the content-mode USB disk [code]. Stay at or below 89 until hardware test H-16
  passes ([memory-map.md](memory-map.md), Q-16).
- The window holds code, constants **and** static data, so big tables or per-voice arrays must fit in it.

---

## 7. The engine ABI

Signatures from the app's symbols (the device exports the same names unmangled) [code]:

| Export | Signature | Called |
|---|---|---|
| `initModule` | `bool initModule(uint, const ModuleCallbackInterface*, uint* out)` | once at load; return true |
| `init` | `void init(void* state, uint track, uint, void* mem, void* scratch)` | when a voice / FX slot switches to this plug |
| `onNoteOn` | `void onNoteOn(int note, uint8 velocity, void* state, const uint* params)` | note start |
| `onNoteOff` | `void onNoteOff(int note, void* state)` | note end |
| `render` (instrument) | `void render(fract32* out, uint, int, void* state, const uint* params, ADSR* adsr)` | per voice, per block |
| `renderFX` (effect) | `void renderFX(const uint* params, int, void* state, fract32* left, fract32* right, fract32 gain)` | per FX track, per block |

**Host side on the device** [code]:
- *Instruments* (`FUN_8012d278`, InstrumentManager::noteOn): voices are `0x17c`-byte records; engine state
  starts at `+0x48`, giving **≤ 308 bytes of state per voice**. On a plug change the host calls
  `init(voice+0x48, track, mem, mem, scratchpad)`; the shared scratchpad is at `0x8156f2b8`.
- *Effects* (`FUN_8012ced0`, FxManager::noteOn): one `0xe0`-byte record per FX track, state from `+0x10`, giving
  **≤ 200 bytes of state**. `init` receives a per-track dynamic buffer (`dynFxMem + memOffsets[track]`); in the
  app build FX1 gets 2 MB and FX2 256 KB. Effects also receive `onNoteOn`, so they can react to notes
  sequenced on the FX track.
- **Parameters**: each 8-bit value becomes `b·0x808080 | b>>1` (0 … ≈`0x7F7F7F7F`, Q31-style) and is
  smoothed every block with a 1/64 one-pole (`Synth::setTargetParameters` / `interpolateParameters`).
  Indices 0 and 1 are the engine's two named parameters; the full order is given below.
- **Block size**: 64 frames (effects copy 64 samples per channel; the instrument pitch slope is `>> 6`).
- **Arithmetic**: fixed point (`fract32`, saturating). The Blackfin+ has no FPU, so floating-point DSP must be ported.

**ModuleCallbackInterface** (13 entries, from the app's `moduleInterface` at `0x95000`; device offsets are 4 × index) [code]:

| # | Offset | Entry |
|---|---|---|
| 0 | `+0x00` | `sequencerSetValue(int, int)` |
| 1 | `+0x04` | `sequencerGetValue(int)` |
| 2 | `+0x08` | `k_lp_table` (low-pass coefficients) |
| 3 | `+0x0C` | `g_sine` |
| 4 | `+0x10` | `g_sineTable` |
| 5 | `+0x14` | `rFilterCoeffs` |
| 6 | `+0x18` | `releaseTable` |
| 7 | `+0x1C` | `updateParametersADSR(ADSR*, a, d, s, r)` |
| 8 | `+0x20` | `updateParametersAHD(ADSR*, u32, u32, u32, u32, fract32, fract32)` |
| 9 | `+0x24` | null in the app; used by device engines — device-only service |
| 10 | `+0x28` | `getPresetData(int, int, void*)` |
| 11 | `+0x2C` | `IIRFilter_updateCoefficientsPeak(IIRFilter*, u16, u8)` |
| 12 | `+0x30` | `IIRFilter_filter(IIRFilter*, fract16*, u8)` |

The offsets match what the notes saw engines call (`+4`, `+8`, `+0x1c`, `+0x20`, `+0x24`).

**`initModule`'s third argument is a memory-size request** [notes]: chorus 16 KB, chorus2 32 KB, reverb 58 KB,
fdnreverb 239 KB, delay and tapetwo 2 MB. The app's FX pools (`memOffsets`) are FX1 2 MB, FX2 256 KB, tape 2 MB,
master the remainder, which is why delay is FX1-only.

**`render` arguments** (app `Voice::renderVoice`) [code]: `render(out, pitch, (pitchEnd − pitch) >> 6, state,
params, adsr)` — the host applies portamento, bend, vibrato and detune and hands the engine a pitch word plus a
per-sample slope over a **64-frame** block.

**Parameter order** (18 bytes, from the project format in the notes) [notes, proven]: P1, P2, attack, decay,
sustain, release, FX1 send, FX2 send, filter, resonance, pan, level, portamento, LFO depth, LFO speed, LFO target,
LFO shape, note style.

**Unreleased engines** present in the app: `Lead`, `DxFm`, `Fm`, and a `Swirl` effect. `Lead` and `DxFm` are
`EP`'s 4-operator engine without its P2; `Fm` is an early `uranus`; `Swirl` is a pitch-shifting delay ambience. The
app never calls them and they have no ids or paths (Q-11); see [04 — Hidden and unreleased](architecture/04-hidden-and-unreleased.md)
and [hidden-engines.md](hidden-engines.md).

---

## 8. Delivery paths

| Path | Status | Notes |
|---|---|---|
| **Normal-mode SysEx file server** (`F0 00 20 76 01 <id> … F7`, messages `$33 $34 $35 $51 $52 $53 $55`) | proven [notes] | Host serves a `syncjob.json` manifest. The OP-Z pulls each file in 200-byte zlib parts and swaps it in after a CRC match. `patch.json` triggers plug import. Custom engines have been installed this way. Removal: `{"operation":"remove","plugId":N}`, which deletes the plug and its sample but **leaves the engine file** behind. |
| Content mode (mass storage) | partly known | The importer filters `wav\|aif\|aiff\|engine` and can log "rejecting invalid engine", but engine import through content mode is undocumented. |
| Firmware update ZIP | possible, not needed | Engines can ship in an update, but that touches the install path. Avoid it. |
| Serial console (update mode) | **avoid for writes** | It is a root shell: `0x15` formats NAND, `0x10`/`0x14` write or format SPI flash, `0x31` runs code. Only read-only commands (`0x00 0x02 0x07 0x17 0x19 0x21 0x27 0x35 0x39`) are safe. |

---

## 9. Building engines: options

1. **ADI CrossCore Embedded Studio (CCES)** for Blackfin+. This is what TE used (the engines are CCES-style DXE/LDR
   output). Use a linker description that places everything inside one slot window with the export block at the
   base, then produce an LDR stream. Commercial licence (evaluation available).
2. **GNU `bfin-elf` toolchain** plus our own LDR packer **[inferred]**. Blackfin+ runs classic Blackfin code, and
   the GCC port uses the VisualDSP/CCES calling convention (R0–R2 arguments). Both must be verified with a minimal
   engine on hardware.
3. **Native simulation first.** Write each engine in portable C with a thin `fract32` layer, compile it for
   macOS against a host shim that reproduces the call pattern above (params Q31, 64-frame blocks, note on/off,
   state limits), render to WAV and unit-test it. Then cross-compile the same source.

---

## 10. Roadmap

| Phase | Goal | Done when |
|---|---|---|
| 0 | Reference material | This map; decompiled host and engine functions kept for reference |
| 1 | Host shim and simulator on macOS | A test engine renders to WAV through the same ABI |
| 2 | Toolchain proof | A minimal engine (sine or silence), built for a free 10 KB slot, passes the loader checks offline |
| 3 | First install | That engine installs as user plug 60xxx over SysEx, appears in `plugs.json`, plays on one track and can be removed |
| 4 | First real engines | One effect (big memory, simple ABI) and one synth (≤ 280 B per voice) |
| 5 | Companion workflow | One command to build, install, assign and roll back, with backups of `plugs.json` and `slotConfiguration.json` |

Measure CPU as you go: "cpu overload: kill notes" shows up in the log, and the hidden text command channel
(`enable-verbose-log`, doc 04 in the notes) exposes more logging.

---

## 11. Synth and effect ideas, sized to the constraints

Mutable Instruments' eurorack code is largely MIT-licensed. Check each file, and port it from float to
fixed point. Don't redistribute TE's own engine binaries.

### Synth engines (tracks 4–7, ≤ 280 B state per voice)

| Idea | Inspiration | Slot class | Notes |
|---|---|---|---|
| Macro virtual-analog | Plaits VA, Braids | 10 KB | two variable-shape oscillators with PolyBLEP; cheap |
| West-coast wavefolder | Plaits waveshaping, Buchla | 10 KB | sine/triangle into a folder; P1 fold, P2 symmetry |
| Phase distortion | Casio CZ | 10 KB | very cheap, distinctive |
| Wavetable | Plaits 8×8 wavetable, OP-1 Digital | 50 KB | tables need the big slot |
| Formant / vowel | Plaits grain formant, VOSIM | 10 KB | |
| Modal / plucked | Rings-lite, Plaits string & modal | 10–50 KB | ministringshort already covers "bow" |
| String machine | Solina, Plaits chords | 10 KB | chord track (4–6 voices) |
| Additive | Plaits harmonic | 50 KB | CPU-heavy: keep partial count low |

### Drum engines (tracks 0–2)

The drum tracks are sample-only today, so synthesized drums are a clear gap: analog kick, snare and
hi-hat models (Plaits drum models, 808/909 style), using `trackmask` `0x1`/`0x2`/`0x4`.

### Effects (FX1/FX2 with large buffers, tape, master)

| Idea | Inspiration | Notes |
|---|---|---|
| Granular texture | Clouds | needs the large FX1 buffer |
| Plate / shimmer reverb | Dattorro plate, Elements | |
| Tape delay with wow and flutter | Echoplex, tape | saturation in the feedback path |
| Phaser / flanger | the name of TE's unreleased `Swirl` (which turned out to be a pitch-shift ambience, Q-11) | |
| Ring mod / frequency shifter | Warps | |
| Note-tuned resonator | Rings | uses the FX `onNoteOn` to tune a comb or modal bank |
| Filter with envelope follower | SVF / ladder | |
| Beat repeat / stutter | — | note-triggered via `onNoteOn` |
| Master bus glue | compressor / saturation | master track (`0x800`) |

---

## 12. Risks and open questions

- Engines run with no memory protection. A bad engine can crash the OS. Keep removal (SysEx remove, content
  mode) and settings backups ready before each test.
- Exact meaning of `render`'s 2nd/3rd arguments, the `ADSR*` contract, parameter indices beyond 0/1, sample
  rate and block size need confirming by decompiling a small official engine (crush, chord) on the device side.
- The device's FX memory offset table (`0x8156f21c`) has not been read yet; the sizes above come from the app.
- Whether slots above 89 are safe.
- Whether the GCC `bfin-elf` output is ABI-compatible in practice.
- The notes' custom-engine proofs (`clean-tone`, `researchmute`) are private evidence; reproduce them before
  relying on them.

---

## 13. Evidence index

| Item | Location |
|---|---|
| dlm load-module / apply blocks / load engine | `opz.ldr` `FUN_8002ae50`, `FUN_8002ac00`, `FUN_8002b02c` |
| Export symbol lookup | `opz.ldr` `FUN_801419a8` |
| Slot → window | `opz.ldr` `FUN_800fb556` |
| loadRequestedEngines | `opz.ldr` `FUN_800fff1c` |
| InstrumentManager / FxManager noteOn | `opz.ldr` `FUN_8012d278`, `FUN_8012ced0` |
| PlugManager import, engine ABI symbols | `OP-Z.app/…/SnakePlugin` (arm64) |
| Host tables (`voicePolyphony`, `memOffsets`, `moduleInterface`) | `SnakePlugin` `__const` |
| DLC catalogue, factory plugs, engine slots | notes repo: `te-dlcs.json`, `opz-factory-plugs.json`, `engines-memoryslot.json` |
| File-transfer protocol | notes repo: `refs/op-z-sysex/docs/clean-room-file-transport.md`, `user-plug-removal.md` |
| Ghidra projects | `ghidra/` (bootloader); OS and app projects were built in the session scratchpad |
