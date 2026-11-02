# Plug format

## Plug fields

| Field | Type | Notes |
|---|---|---|
| `id` | u32 | TE uses < 200. **User plugs: 60000+.** This project uses 60900–60999 for test engines and 61000+ for released ones. |
| `name` | string | Shown in the app |
| `engine` | string | `engines/<name>.engine` (in `plugs.json`) or `<name>.engine` (in TE's catalogue) |
| `samples` | string | Sample pack path, or `""`; at most 95 characters. The OS uses crc32 of it as the sample-pack id that `init` gets as `arg2`. **Never name a pack that is not installed** — it breaks content mode [notes]. See [Sample plugs](#sample-plugs) |
| `trackmask` | u16 | Bit *n* = track *n*. Synths `0xF0`, drums `0x1`–`0x8`, FX1 `0x100`, FX1/FX2 `0x300`, tape `0x400`, master `0x800`. Used by companion apps; the device does not read it. **Our tools put a plug on fx1, fx2, tape or master only if its mask has one of bits 8–11** (`0xF00`): a mask with only instrument tracks, or 0, marks a synth or drum engine, and what one does on an effect track is untested, so `opz.py install`/`assign` and the web app refuse it there. Other mismatches only warn. See also [`master_in_place`](#master_in_place) |
| `flags` | u32 | **One byte** (higher bits are dropped). `0x01` insert-type FX (an old project upgraded to the current format gets FX dry level 0 instead of 255); `0x02` user sample plug, a clean-up candidate — **never set it**; `0x04` the engine filters itself: the OS track filter is skipped on that track, `params[8]`/`[9]` are the engine's; `0x08` / `0x10` P1 / P2 bipolar (centre-detent LED cue on the dial); `0x20` FX plug: sending tracks' dry is ducked by send² instead of send (as TE's `delay`); `0x40`, `0x80` unused. **No flag changes voicing** (voice count, mono, legato, glide come from the note style and portamento). Default 0; recommended values per engine in [host-tables-flags.md](../../research/host-tables-flags.md#recommended-flags-for-our-engines) [code, app + device; Q-08] |
| `version` | u32 | A plug in `auto_import` is only re-imported if the version increases. Stored in one byte on the device: keep it below 128 |
| `parameter_names` | [string, string] | Labels for P1 and P2 |
| `presets` | array | See below |

## Presets

Each plug carries up to 56 presets: 4 `slot`s × 14 `index`es. Findings and evidence:
[research/presets.md](../../research/presets.md).

```json
{ "slot": 0, "index": 0, "defaultOctave": -2, "name": "bias",
  "values": [58, 2, 0, 80, 84, 58, 8, 87, 60, 89, 137, 100, 0, 26, 178, 175, 190, 255] }
```

| Key | Meaning |
|---|---|
| `slot` | **The synth track the preset is for: 0 bass, 1 lead, 2 arp, 3 chord.** Every other track (drums, FX, tape, master) reads slot 0 only, so effect and drum plugs use slot 0 [code, app + device]. FX tracks have no user-assignable presets [proven]: **an effect plug uses only slot 0 index 0**. Clamped to 0–3 |
| `index` | 0–13 within the slot; other values are ignored. **Index 0 is the sound a track gets when it switches to the plug** (unless the pattern already has stored parameters for it) [code, app] |
| `name` | 11 characters kept. TE's app does not show names for our plugs; our tools do |
| `defaultOctave` | The track's octave when the preset is applied, −4…4 (clamped; −1 if missing) |
| `values` | The 18 parameter bytes: P1, P2, attack, decay, sustain, release, FX1 send, FX2 send, filter, resonance, pan, level, portamento, LFO depth, LFO speed, LFO target, LFO shape, note style. Applying keeps the track's pan (and on the arp track its LFO). **On FX1/FX2, byte 12 is the effect's dry level**: 255 keeps the sending track's dry signal (send/return), 0 crossfades it into the effect (insert-like) [code, app; name inferred from the manual's "dry mix", p.6]. **Note style** (byte 17) on synth tracks: 0–85 poly, 86–170 mono, 171–255 legato; with portamento (byte 12) > 0 this makes a gliding mono lead without any engine change |

The device reads the keys `slot`, `index`, `name`, `defaultOctave`, `values` (`readPresetSettings`). TE's web
catalogue spells it `default_octave`; our `presets.json` may use either, and `opz.py` writes `defaultOctave`.

**Lifetime.** Presets are imported with the plug and kept in RAM. Re-importing a plug merges: the given
`slot`/`index` pairs are replaced, the rest stay. **They do not survive an OP-Z restart**: at boot the OS restores
presets only from TE's factory cache and from presets the user saved on the device (key hold) [code, app + device;
hardware check H-2]. `opz.py presets push` re-sends them without a version bump, through the top-level `plugs[]`
array below.

**Live sound.** SysEx `$0E` sets a track's 18 values in the active pattern (the sound and the stored project), not
a preset: see [presets.md](../../research/presets.md#0e-in-detail).

## Sample plugs

A sample plug is a plug whose `samples` names an OP-1 format `.aif` pack; the engine is TE's
`engines/sampleplayergain.engine` (drum tracks) or `engines/synthsampler.engine` (synth tracks). Format, memory and
evidence: [research/samples.md](../../research/samples.md).

| Kind | `id` | `samples` | `flags` |
|---|---|---|---|
| TE's factory packs | 71, 121–145 | `samplepacks/<Name>.aif` | 0 (4 on 144, 145) |
| The user's own (made on the OP-Z, TE's app, content mode) | **crc32 of the `samples` path** [proven: 12 of 12] | `samplepacks/user/<name>.aif` | **2** |
| Ours (`opz.py samples install`) | 61400–61499 | `samplepacks/opzk_<name>.aif` (one folder deep: the file service can't write into `samplepacks/user/`, Q-49) | 0 |

Two OS behaviours make the user's samples fragile [code]: **removing a plug deletes its pack file**, and **content
mode drops every flag-2 plug that sits in no slot and then deletes the files in `samplepacks/user/` that no plug
names**. Our tools never remove, overwrite or unassign a user sample (SAFETY.md). The file service's `import`
operation also makes flag-2 user samples (`samplepacks/user/<name>.aif`, the track's first empty slot or none); our
tools don't use it ([samples.md §6](../../research/samples.md), Q-28). A sample plug's engine need not be TE's: any
instrument engine on a plug with `samples` gets the pack (Q-27, [samples.md §5](../../research/samples.md)).

## `plugs.json` (device, after import)

```json
{
  "engines": [ { "path": "engines/analoga.engine", "memoryslot": 27, "version": 2 } ],
  "plugs":   [ { "id": 10, "engine": "engines/analoga.engine", "samples": "", "flags": 20, "version": 25 } ]
}
```

(`flags` 20 = `0x10` P2 bipolar + `0x04` own filter: analoga runs its own filter on the filter/resonance knobs.)

## `settings/patch.json` (import request)

```json
{
  "auto_import": [
    {
      "engine": { "path": "engines/opz_test_sine.engine", "memoryslot": 57, "version": 1 },
      "plug":   { "id": 60901, "version": 1, "name": "testsine", "engine": "engines/opz_test_sine.engine",
                  "samples": "", "trackmask": 240, "flags": 0,
                  "parameter_names": ["shape", "drive"], "presets": [] },
      "tracks": [4]
    }
  ]
}
```

`tracks` are zero-based track indices; each gets the plug in its first free slot. The device itself reads only
`id`, `version`, `engine`, `samples`, `flags` and `presets` from `plug` (the other fields are for companion apps).
An `auto_import` plug is skipped unless its `version` is higher than the installed one. Top-level `engines[]` and
`plugs[]` arrays are also accepted; **a `plugs[]` entry is imported with no version check** and assigns no track,
so it must repeat the installed `engine`, `samples`, `flags` and `version` (they are overwritten) [code, app +
device]. `version` is stored in one byte: keep it below 128. Removal is a separate sync-job entry:
`{"operation": "remove", "plugId": 60901}`.

## Plug manifest in this repository

Each engine directory will carry a `plug.json` that the build turns into the `patch.json` entry. Proposed shape:

```json
{
  "engine": "swirl",
  "memoryslot": 56,
  "engine_version": 2,
  "plug": { "id": 61001, "version": 2, "name": "swirl", "trackmask": 2816, "flags": 0,
            "parameter_names": ["rate", "character"] },
  "presets": "presets.json"
}
```

### `master_in_place`

Top-level, optional, `true` or absent (default: not set). It tells our tools the engine has a **master mode**: when
`renderFX` gets `out == NULL` it processes `in` in place and writes nothing through `out`. **No engine sets it yet.**

The master track (11) runs its plug in place: `renderFX` gets the main mix as `in`, **`out = NULL`** and `gain` =
`0x7fffffff`, and the result must go back into `in` ([routing.md §3](../../research/routing.md#3-master-can-our-effects-run-there-q-19-q-20)).
An engine that writes `out` there writes to address 0, which crashes and reboots the OP-Z, and because the plug
choice is stored in the project, probably at every start of that project. The OP-Z does not stop such a slot-map
entry and does not read `trackmask`, so the tools do:

- `opz.py install --track master` and the web app's install refuse unless the engine's `plug.json` has
  `"master_in_place": true` (any other value, `false` or `"yes"`, counts as absent).
- `opz.py assign ID --track master`, the web app's assign and its slot-map writer refuse to put a plug on master
  unless it is TE's master chorus (plug 39) or a local `plug.json` with that id declares `master_in_place`. A
  master entry that is already on the OP-Z is left alone.
- Tape (10), FX1 and FX2 call `renderFX` with separate `in` and `out` buffers, so an effect engine needs no
  flag there; it still needs an effect track in its `trackmask` (see `trackmask` above).

Setting `trackmask` bit 11 (`0x800`) does not replace this field: master needs both an effect track in the mask and
`master_in_place`.
