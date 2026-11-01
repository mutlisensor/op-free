# Sample packs and drum kits

How OS 1.2.45 stores, loads and plays sample packs, how a pack reaches an engine, and how our tools can upload kits
of their own (feature F-09) without ever touching the user's samples. Offline research (2026-10-19; playback, the
pack's lifetime and the `import` op 2026-10-23): the device OS (`opz.ldr`, Ghidra), TE's engines on the device, the
desktop app's native code (`SnakePlugin`, arm64, which carries symbols), TE's content package and the backups of the
user's `plugs.json`. Nothing here has run on hardware yet; the hardware checks are at the end.

Evidence labels as in [CONTRIBUTING.md](../CONTRIBUTING.md). Addresses are device (`opz.ldr`) unless marked app.

## In short

| Finding | Evidence |
|---|---|
| A pack is an **OP-1 format AIFF**: 16-bit **mono**, **44.1 kHz**, an `APPL` chunk `op-1` + JSON. `"type":"drum"` = 24 keys, `"type":"sampler"` = one synth sample | [code] `FUN_8010576c`, `FUN_80105e94`; TE's `UserSampler.aif` parses as such |
| Max **12.0 s** (529,200 frames, 1,058,400 bytes) per pack; synth samples address only the first **6.0 s** | [code] constants `0x81330`, `0x40998`; [manual] p.38, p.53 |
| The OS keeps **14 packs** in RAM at once (14 × 1,058,400 B at `0x82700000`), loading and evicting by request priority | [code] `FUN_80105010`, `FUN_80106e2c` |
| At most **24 MB** of files in `samplepacks/user/`; checked by content mode and the on-device sampler, **not** by the file service | [code] `FUN_8003377c`, `FUN_80038174`; [manual] p.63 |
| A user sample's **plug id = crc32 of its `samples` path**, flags `2`, version 1, engine `sampleplayergain` (drum tracks) or `synthsampler` | **[proven]** 12 of 12 user plugs in the user's `plugs.json`; [code] `FUN_800ff9e8` (addUserPlug) |
| `init`'s `arg2` (and `mem`) for instruments is the plug's **sample-pack id** = crc32(`samples`), 0 without a pack. Callback `+0x24` is **`getSample(packId, note)`**, not `(track, note)` | [code] `FUN_80100e64` → `FUN_80100d78`; app `SamplePlayerGain::init`/`onNoteOn` |
| **Any engine gets the pack (Q-27)**: the OS requests the pack of every plug in a slot, passes its id to `init` and locks it for each note, by plug id and pack id only; nothing looks at the engine | [code] `FUN_801091ac`, `FUN_800fc160`, `FUN_8012d278`, `FUN_8012e0a4` (§5) |
| A note on a plug whose pack is **not loaded is dropped** before `onNoteOn`: the voice must lock the pack first. The lock lasts until the voice ends, so the slice stays valid while it plays | [code] `FUN_8012e0a4` → `FUN_80100c40`; unlock `FUN_8012e010` → `FUN_80100e80` |
| `sequencerGetValue(8)` is **not** a "samples ready" flag: TE's engines play whatever it says; it only makes them re-read the slice every block (live editing in sample mode) | [code] device `sampleplayergain` `0x81b69f6e`; app `SamplePlayerGain::onNoteOn`/`render` |
| What each slice word does to playback (Q-26): pitch 512 per semitone → rate `2^(cents/1200)/16`; P1 ±32 semitones with a centre detent; playmode < 11,008 gate, < 21,888 trigger, else loop (editable packs only); reverse ≥ 16,384 or P2 past centre plays backwards; gain `(volume/8192)²` | [code] §8; model `tools/opz_sampleplay.py` |
| **Removing a plug deletes its sample file** (`remove` sync op) | [code] `FUN_800fe700` ("remove file %s") |
| **Content mode deletes the user's samples that sit in no slot**: it drops flag-2 plugs that are unassigned or whose file is gone, then deletes every file in `samplepacks/user/` that no plug names | [code] `FUN_80038cd8` → `FUN_800ffc88`, walk with `FUN_800364c4` |
| The file service writes a file **one folder deep** only (`samplepacks/x.aif`, not `samplepacks/user/x.aif`): it stages each download in `inbox/<path>` with a single `mkdir`, and a deeper path is dropped without an error. It requests a listed file unless the file at that path already has the listed CRC, so an identical re-push being skipped proves the file is there (Q-49) | [code] `FUN_80129454`, `FUN_8012bb00`, `FUN_80127a24` (§6) |
| The file service can write a one-level pack path (`write`), and also has an **`import` operation** that validates a WAV/AIFF, converts it and makes a user sample (flag 2) in the track's **first free slot, or no slot** (Q-28) | [code] `FUN_80127a24` (slot −1), `FUN_80037df4`, `FUN_800fcc8c` |

**Risk on the user's OP-Z right now** (from `backups/20261019T183432853440Z-install-vowel`): three of the user's own
samples are in no slot because our tests displaced them — `147160281` (`accguit_starburs-6135.aif`, was lead 10),
`787747859` (`user216445511.aif`, bass 9), `1513459487` (`user941443834.aif`, arp 10). By the code above, **the next
time the OP-Z goes through content mode (disk mode) it would remove these plugs and delete the files** [code,
inferred: not seen on hardware]. Put them back in a slot before any disk-mode session (`opz.py restore
backups/20261017T070002617364Z` for the whole original map, or `opz.py assign ID --track T --slot N`), and don't
enter disk mode until then. `opz.py status` and `opz.py samples list` now print this warning.

## 1. Where packs live

The OS's file root is `/yaffs2/user/` (`DAT_81125fc4`); every path below is relative to it [code].

| Path | What |
|---|---|
| `samplepacks/<Name>.aif` | TE's factory packs, flat (`TeKicks.aif`, `CuckooC_keypawn.aif`, …). Plug ids 71, 121–145, flags 0 (144 `UserSampler` and 145 `GrantFX` have flags 4) [proven, backup] |
| `samplepacks/user/<name>.aif` | packs made on the OP-Z (sampler: `user%08d.aif`, `FUN_80129c24`) or imported (TE's app, content mode): `ryantechnokicks-8336.aif`, `user216445511.aif` [proven, backup; code] |
| `samplepacks/opzk_<name>.aif` | **ours** (`opz.py samples install`, flags 0, ids 61400–61499): one folder deep like TE's packs, because the file service can't write into `samplepacks/user/` (Q-49). Content mode's cleanup walks only `samplepacks/user/`, so it never touches these; the 24 MB limit doesn't count them [code] |
| `samplepacks/sfx.aif` | referenced by the OS (`0x8110f6ac`) [code] |
| `/yaffs2/tmp/` | staging for uploads and content mode [code] |

Content mode shows a virtual view instead: `1-kick/01/ … 8-chord/10/`, one pack per slot folder
(`/one_sample_pack_per_slot.plz`), duplicates as `~name.aif` of 0 bytes (`readme/how_to_import.txt`) [code, TE text].
The `samplepacks/<track>-<name>/…` folders that SAFETY.md rule 3 warns about are this content-mode view.

## 2. The plug record

`plugs.json` stores `{id, engine, samples, flags, version}`. In RAM the OS keeps up to 200 plug definitions of
1,964 bytes at `0x80045AC8` [code `FUN_800ff9e8`; the app's `addUserPlug` writes the same layout]:

| Offset | Field |
|---|---|
| `+0x000` | plug id |
| `+0x004` | **sample-pack id** = crc32(`samples`) (`makeHash`; 0 for `""`) — what `init` gets as `arg2` |
| `+0x008` | crc32(`engine`) |
| `+0x00C` | `samples` path, 96 bytes (so at most **95 characters**) |
| `+0x06C` | `engine` path |
| `+0x7A8` | flags byte (user samples: `2`) |
| `+0x7A9` | version byte |

A separate map pack id → path (filled by `addUserPlug` and when plug definitions or a `patch.json` plug are read:
app `0x44fa4`, `0x4530c`, `0x4560c` in `loadPlugDefinitions`, `_importPlugDefinitions`, `importPlugFromPatch`) is
how the loader finds the file ("failed to find path for pack id", `FUN_80106cfc`) [code, app + device].

**User samples** are created by `addUserPlug(id, path, track, slot)` (`FUN_800ff9e8`): the id is the caller's —
always crc32(path) in practice (`FUN_80037df4` computes it, `0x80037e9a`) — flags 2, version 1, engine
`engines/sampleplayergain.engine` for tracks 0–3 and `engines/synthsampler.engine` otherwise, then the plug goes
into the given slot (track 16 = none) and the definitions are saved. "plug count full" past 200 plugs. **Proven** on
the user's device: all 12 user plugs have id = crc32(`samples`) and flags 2.

`flags` bit 1 (`2`) is the "user sample" mark (`FUN_800ffc88` tests `+0x7A8 & 2`) [code] — a partial answer to Q-08.

## 3. File format

What the OS accepts [code `FUN_8010576c` (header and type), `FUN_80105e94` (keys), `FUN_80106cfc` (load)]:

- AIFF or AIFF-C read through libaiff. The COMM sample rate must be exactly **44100.0** ("unsupported sample
  rate"). Mono 16-bit: the loader addresses frames as `buffer + frame × 2` [code], so stereo or other widths
  can't play correctly [inferred]. TE's files are AIFF-C `sowt` (little-endian), `FVER` + `COMM` + `APPL` + `SSND`.
- Frames > 529,200 → "Samplepack is too big … Aborting". The reader must return exactly the COMM frame count.
- `APPL` chunk starting with `op-1`, then JSON. `type` decides: `drum`, `sampler`, anything else → "Neither drum or
  sampler data found" (an OP-1 synth patch gets status 13, "op1 synth patches are not supported").
- No `APPL`: a plain sample at 44.1 kHz is accepted as one sample, capped at 264,600 frames (6 s).

**Drum packs** — keys read: `drum_version` (default 2), `start`, `end`, `pitch`, `volume`, `playmode`, `reverse`,
`editable`; also present in TE's files and kept for the OP-1: `name`, `octave`, `dyna_env`, `fx_*`, `lfo_*`.

- `start[i]`, `end[i]`: positions with 2³¹ = 529,200 frames; the OS computes `frame = pos × 529200 / 2³¹` with a
  Q31 multiply (the "×4058" of OP-1 lore). Up to 24 keys; `pitch`, `volume`, `playmode`, `reverse` are padded to 24
  with defaults.
- One 32-byte slice per key (below). A key plays `start .. end`.
- `volume`/`playmode`/`reverse`/`pitch` are OP-1 knob values (0–32767). The slice keeps volume as 16 bits and
  playmode, reverse as `value >> 7`; `sampleplayergain` plays a key reversed when that reverse byte is > 127 (value ≥
  16,384) and P2 is in its lower half [code, device engine]. TE's default-looking values: volume 8192, pitch 0
  [proven, TE file]; our builder writes playmode 8192 and reverse 8192 [inferred: one-shot forward; checked by ear in
  the hardware check].

**Synth samples** (`type: "sampler"`): exactly 8 `knobs` (else "Wrong number of knobs in synth sample"),
`base_freq` (default 261.626 Hz, C4), `adsr`, `synth_version`. The 8 knobs hold, in 6-s units (Q15 of 264,600
frames, truncated): `[0]` start, `[3]` end, `[1] << 16 | [5] << 1` loop start, `[2] << 16 | [6] << 1` loop end, `[4]`
play mode, `[7]` volume; the OS clamps start ≤ loop start ≤ loop end ≤ end ≤ the file's frames (6 s at most) and
stores the loop points relative to start [code `FUN_80105e94` `0x80106140`–`0x801061e8`].

**The slice** (what `getSample` returns; 32 bytes) [code `0x80106880`–`0x8010699a`]:

| Offset | Content |
|---|---|
| `+0x00` | `int16 *` first frame |
| `+0x04` | frames |
| `+0x08` | drum: volume \| playmode byte << 16 \| reverse byte << 24; synth: `knobs[7]` (volume) \| play mode (`knobs[4] >> 7`) << 24 |
| `+0x0C` | the speed: drum from the key's pitch (`FUN_8010774c`), synth from `base_freq` (`FUN_80107760`); `2^27` = unity (§8) |
| `+0x10`, `+0x14` | synth: loop start and loop end, in frames from `+0x00`; drum: 0 |
| `+0x18` | drum: the pitch knob (low 9 bits cleared when `drum_version` < 3); TE's engines don't read it |
| `+0x1C` | the pack's `editable` flag |

The SDK declares it as `OpzSampleSlice` in [opz_engine.h](../sdk/include/opz_engine.h).

**Import status codes** (the byte at `0x81004FE0` that `FUN_80037df4` and the parser set) by what triggers them
[code]: 0 success; 2 the WAV/AIFF converter could not open the file; 5 neither drum nor synth data and no track; 8
neither `.aif`/`.aiff` nor `.wav`; 9 the `op-1` JSON is unreadable or has no usable `type`; 10 a drum track got no
drum pack; 11 a synth track got no synth sample; 12 not 44.1 kHz; 13 an OP-1 synth patch. Their messages are
probably the strings at `0x81005154` (`SUCCESS`, `invalid size`, `invalid file`, …) and `0x810052F8`
(`unsupported bitrate` … `op1 synth patches are not supported`) in that order [inferred].

`tools/opz_samples.py` implements this format: `parse()` applies the checks above, `build_drum_pack()` writes a
pack like TE's (AIFF-C `sowt`, keys spread over 24, `pos_from_frame` choosing positions that map back exactly
under both rounding and truncation). It reads TE's `UserSampler.aif` cleanly (24 keys, 8.0 s) [proven, test].

## 4. Memory

- **RAM**: 14 cache slots of 1,058,400 bytes (`0x102660`) from `0x82700000` to `0x8352193F`, set up by
  `FUN_80105010` [code; matches research-slots]. A slot holds one pack; a 1-s pack still takes a whole slot.
- **Requests** (`FUN_801091ac`, after every pattern or slot change): the plugs on the 8 tracks of the current
  pattern at priority 0, those of the chained patterns that follow at 1, 2, …, then every plug in any of the 80
  slots at 9; each plug's pack id comes from its definition (`FUN_800fc160`, `+0x004`), 0 is skipped, a pack keeps
  its best priority, and only the first **50** requests count (`FUN_80104ea8`) [code].
- `FUN_80106e2c` (the loader's update) sorts the requests by priority, keeps the first 14, keeps what is loaded, and
  loads the rest into free cache slots or over packs no voice holds that are no longer requested ("Unloading pack %s
  to give room for %s"). A cache slot (`0x48` bytes from `0x0802de3c`): `+0x00` pack id, `+0x08`/`+0x0C` the slice
  vector, `+0x14` the buffer, `+0x1C` the lock count, `+0x20` priority, `+0x24` "no longer requested", `+0x25`
  editable, `+0x44` base_freq. A voice locks its pack for each note and unlocks it when it ends, so a loaded pack
  can't vanish under a sounding note, and a remove fails while it is locked ("failed to remove sample pack") [code].
- **Storage**: content mode sums the files in `samplepacks/user/` ("Calculating used sample memory") and refuses an
  import past 24 MB (`0x1800000`, "Can't import %s, sample memory full"); the sampler checks `used + 1 MB < 24 MB`
  before recording (`FUN_8003377c`) [code]. Factory packs are not counted. **The file service's `write` checks
  nothing** — our tools must keep count. The NAND's real free space is unknown.
- **Transfer**: a 12-s pack is 1 MB = ~5,300 file-service parts of 200 bytes; the part index is 16-bit (13 MB max).

## 5. From plug to sound (Q-27)

1. **Request and load** (§4): every plug in a slot has its pack requested (priority 0 if the current pattern uses
   it); the loader finds the path through the pack-id map ("failed to find path for pack id") and loads
   `/yaffs2/user/<samples>` into a cache slot ("Load sample pack %s") [code `FUN_80106cfc`].
2. **`init`, at the first note**: a voice that plays a note on a plug other than its last one resets (unlocking the
   old pack), looks the plug's engine up (`FUN_800fc104`, "Can't load engine"), stores the pack id at voice `+0x1C`
   and calls `init(state, track, packId, packId, scratch)` (`FUN_8012d278`). TE's `sampleplayergain.init` keeps
   it at `state + 0x14` [code].
3. **Lock, then `onNoteOn`**: `Voice::noteOn` (`FUN_8012e0a4`) locks the pack by id (`FUN_80100c40`): only a loaded
   pack that is still requested can be locked; the lock count goes up and the pack id is kept at voice `+0x20`.
   **If the lock fails the function returns: no voice starts and the engine's `onNoteOn` is not called** — a plug
   whose pack is missing or still loading is silent, whatever its engine. A plug without a pack (id 0) skips the
   lock [code, device `0x8012e1cc`–`0x8012e1dc`].
4. **`getSample(packId, note)`** (`+0x24` → `FUN_80100e64` → `FUN_80100d78`): looks the id up in the loaded-pack map;
   NULL for an unknown id, a pack no longer requested, or a pack without slices ("No samples found for id %u",
   "Samplepack %s doesn't contain samples"); else slice `(note + 19) mod 24` (= `(note − 53) mod 24`) clamped to
   the last key. A synth sample has one slice, so every note gets it (TE's `synthsampler` passes note 0). It takes no
   lock itself: call it in `onNoteOn`, while the voice's lock holds [code].
5. `render` reads `slice->data` from the cache in SDRAM (`0x82700000` + 14 × `0x102660`).
6. **Unlock**: when the voice ends (its envelope finishes, `FUN_8012e2c0`) or switches plug, `FUN_8012e010` unlocks
   the pack (`FUN_80100e80`; "Lock count should never go below zero") [code].

**Nothing in this chain looks at the engine**: the request, `init`'s argument, the lock and `getSample` all go by plug
id and pack id. An engine of ours on a plug whose `samples` names a pack gets that pack exactly as TE's player does
[code; hardware check H-S2, §10]. **`sequencerGetValue(8)`** is not a "loaded" flag: TE's engines call `getSample`,
keep the slice and play whatever key 8 says; they only keep the slice pointer for the next blocks when key 8 is
non-zero, and then re-read its rate and volume (and the synth sample's loop points) every block — live editing in
sample mode [code, device `0x81b69f5c`–`0x81b69f7e`, app `SamplePlayerGain::render` `0xb850`]. Earlier versions of
this page, the SDK and `opz_test_sampler` treated it as "samples ready"; that would have silenced our engine on the
device.

## 6. How packs get onto the OP-Z

| Route | What happens | For us |
|---|---|---|
| **Content mode** (disk mode) | drop an OP-1 `.aif` (or `.wav`, converted) into `<n>-<track>/<slot>/`; on eject `FUN_80038174` imports it (`FUN_80037df4`), assigns it, then runs the cleanup below | not scriptable over MIDI; the cleanup endangers loose user samples |
| **On-device sampler** | records `user%08d.aif`, then the same import | — |
| **File service `import` op** | a sync-job entry `{"fileId","path","crc","size","operation":"import","track":T}` (track defaults to 3): the OP-Z downloads the file like any other, then imports it with **slot −1** (details below, Q-28) | not for us (below) |
| **File service `write` + `patch.json`** | write the pack to a path one folder deep (staged in `inbox/`, copied in place after a CRC check), then import a plug that names it [code `FUN_80127a24`; how our engines are installed, proven] | **our route (B)** |

**Removal** (`{"operation":"remove","plugId":N}` → `FUN_800fe700`): unloads the pack (fails silently if a voice has
it locked, `FUN_801056e0`), **deletes the pack file**, removes the plug and saves [code]. The SAFETY.md note "deletes
the plug and its sample" now has its mechanism.

**Content-mode cleanup** (`FUN_80038cd8`, after reading the disk and again after "Rebuilding plug definitions"):
`FUN_800ffc88` removes every flag-2 plug whose file is missing ("found dangling plug") or that is in no slot ("found
unassigned user sample"); then a walk of `samplepacks/user/` with `FUN_800364c4` deletes every file no plug names
("Remove unused sample pack") [code]. Plugs with flags 0 (TE's and ours) are not dropped, and a file named by any
plug stays. Normal boots don't run it (the only callers are in content mode).

### Route B: `opz.py samples` (implemented, `--fake` tested)

```bash
python3 tools/opz.py samples build build/samples/testkit.aif --test-kit
```

```bash
python3 tools/opz.py samples check build/samples/testkit.aif
```

```bash
python3 tools/opz.py samples install build/samples/testkit.aif --name testkit --track kick --dry-run
```

| Command | Does |
|---|---|
| `samples build OUT.aif (--wav F … \| --dir D \| --test-kit) [--name N]` | up to 24 WAVs (mixed to mono, resampled to 44.1 kHz) or 24 sounds synthesised by the tool → one drum pack; fewer than 24 sounds repeat over the keys |
| `samples check FILE` | the OS's checks, key list |
| `samples list [--from BACKUP]` | sample plugs grouped as ours / the user's / TE's / other, with slots; warns about user samples in no slot |
| `samples install FILE --name N --track T [--slot S --replace] [--plug-id ID] [--version V]` | see below |
| `samples remove ID [--unassign]` | only a plug of ours; the OP-Z deletes its pack with it |

`install` writes `samplepacks/opzk_<name>.aif` and a plug of ours: id 61400–61499 (first free), flags 0,
version 1 (or installed + 1 to update the same name), engine TE's `sampleplayergain` (drum pack, tracks kick–sample)
or `synthsampler` (synth sample, bass–chord), the installed engine definition repeated unchanged. Three syncs after
the backup and the confirmation:

1. the pack file alone;
2. **the same file again: the OP-Z must skip it** — it requests a listed file unless crc32 of the file now at that
   path equals the listed CRC (below), so a skip proves it holds exactly these bytes (samples can't be read back).
   If it asks again, stop: sync 1 did not land, and no plug names the path;
3. `patch.json` (+ the slot map with `--slot`); then plugs.json and the slot map are read back and checked, the
   slot map restored on a mismatch. The backup folder keeps `samples.json` and a copy of the pack.

So a plug never names a pack that is missing (SAFETY.md rule 4), and the pack lands before `patch.json`.

**Guards for the user's samples** (all in `tools/opz_samples.py`, used by `opz.py`):

- A plug is **ours** only with an id in 61400–61499, `samples` under `samplepacks/opzk_`, and no flag 2.
- `opz.py remove` (and `samples remove`) refuses any plug with flag 2, any plug whose pack we didn't write
  (factory packs too), and a pack another plug also names — removal deletes the file.
- `install`, `assign` and `samples install` refuse a slot change that would leave a user sample in no slot.
- `samples install` refuses a path already named by a plug that isn't ours, a path whose pack id collides with
  another plug's, a drum pack on a synth track (and the reverse), ids outside our range, a version ≥ 128.
- `status`/`samples list` warn about user samples that are in no slot now.

The fake OP-Z (`tools/fake_opz.py`, and `core/src/fake.ts`) stores written packs, deletes a removed plug's pack,
models the content-mode cleanup (`FakeOpz.content_mode()`) and the sync job below (skip rule, one-level inbox
folders, copy-in at the end); `tools/tests/test_samples.py` covers the format, the three-sync install, a lost
upload, the 2026-11-01 run (`SyncJobRule`), updates, removal and every guard.

### The sync job (Q-49)

What the OP-Z does with a `syncjob.json` it pulled, from the device code (FileClient/FileSync in `opz.ldr`) [code]:

1. **Queue.** `FUN_80127730` walks `files`. Every entry with no `operation`, or with `write` or `import`, is queued as
   (`fileId`, `path`, `crc`). The numbers are read with jsoncpp's `asUInt` (`0x08035ea4`), so CRCs above 2³¹ are fine.
2. **Request or skip.** For each queued entry (`0x8012c44c` loop) `FUN_8012bb00` computes crc32 of
   `/yaffs2/user/<path>` (`FUN_80128804`, hardware CRC unit `FUN_80027ecc`, 0 if the file is missing). If the
   manifest `crc` ≠ 0 and the two match, it logs "Ignore File request … same crc" and moves on. Otherwise it creates
   `/yaffs2/user/inbox/<path>` and requests the parts. **No CRC is stored and no list of tracked files exists**: the
   rule works the same for an engine, a setting, a project or a pack. A manifest CRC of 0 is always requested.
3. **Stage.** `FUN_80129454` makes the staging folder with **one** `mkdir` of the path's last folder
   (`FUN_801288b8` → yaffs mkdir `0x8000e8f8`; the recursive helper `FUN_801288cc` is used only for the final copy).
   Parts arrive in `$53` (`FUN_8012c4bc`), are gathered in 8 KB and appended (`FUN_801291b6`, mode `ab`).
4. **Report.** The heartbeat (`FUN_8012702c`) sends 127 once the queue is empty, **whether or not the files could
   be written**.
5. **Finalise** (`FUN_80127a24`, after the last file). For each write entry, it checks crc32(inbox file) against the
   listed `crc` ("invalid file crc, can't finalize" otherwise), copies the file in place (`FUN_80128930`, which
   makes the folders), reloads a `projects/project` path and imports a `patch.json` path. Then come the `remove` ops
   (`FUN_800fe700`) and `import` ops (`FUN_80037df4`), the new client state (crc32 of plugs.json then
   slotConfiguration.json, `FUN_8012bd30`) and the slot-map reload (`FUN_800ff1cc`). **Nothing deletes a pack that no
   plug names**: the normal-mode callers of unlink (`0x80128754`) are the `remove` op, the boot file, the sampler and
   the FileServer.

So a path two folders deep (`samplepacks/user/x.aif`) can't be staged unless `inbox/samplepacks/` already exists,
which happens only after a one-level `samplepacks/y` upload. The parts are accepted and thrown away, the sync
reports complete, and nothing is written. That is the 2026-11-01 run exactly (§9). Engines, settings and projects
are one folder deep, which is why every earlier upload worked.

Consequences for the tools:

- Our packs live at **`samplepacks/opzk_<name>.aif`**. That folder is where TE's factory packs load from [proven:
  they play], the `remove` op deletes any plug's file wherever it is [code `FUN_800fe700`], and content mode's
  cleanup only walks `samplepacks/user/`.
- `opz_session.push` (and `FileSession.push` in core) **refuse a path two folders deep** before sending anything.
  `samples install` refuses one too.
- The skip probe stays: it is now backed by the code. A skip proves the bytes are at the path. A request proves
  only that they were not there at that moment.
- Whether staged files are deleted from `inbox/` after the copy is open (Q-51). It doesn't affect the probe.

**On the device** the rule gets its check in H-S1's own sync 2 (§9 rerun): a one-level pack sent twice must be
skipped the second time. That needs no separate step, because syncs 1–2 write only a pack that no plug names, and
the tool stops by itself if the skip doesn't come.

### The `import` operation (Q-28)

`FUN_80037df4(src, name, track, slot)` is one function with three callers: content mode (`0x800385cc`, the slot
folder's track and slot), the on-device sampler (`0x8012a0c8`, the slot being recorded into) and the sync job
(`0x80127e80` in `FUN_80127a24`). From the sync job [code, device]:

| Argument | Value |
|---|---|
| `src` | the downloaded file in the tmp folder, under the job's `path` |
| `name` | the job's `path` |
| `track` | the job's `track`, **3** (sample) when absent |
| `slot` | **−1**, a constant (`P5 = -1` at `0x80127c62`) |

1. `FUN_80105c00` reads the file: an op-1 drum pack, an op-1 synth sample, or a WAV/plain AIFF to convert. Status
   codes as in §3: 5 neither drum nor synth with track −1, **10** a drum track (< 4) without a drum pack, **11** a
   synth track without a synth sample, 8 neither `.aif`/`.aiff` nor `.wav`.
2. The destination is **`samplepacks/user/` + the name up to its last dot + `aif`** (`"%s%saif"`; `foo.wav` →
   `samplepacks/user/foo.aif`, directories in the name are kept). The file is moved or converted there,
   **overwriting** any file of that name (`FUN_80128930(src, dst, 1)`).
3. `addUserPlug(crc32(destination), destination, track, −1)` (`FUN_800ff9e8`): a **new definition, flags 2,
   version 1**, engine `sampleplayergain` (tracks 0–3) or `synthsampler` — unless a plug with that id exists, in
   which case the old definition is kept as it was (but its file was just replaced). Then `FUN_800fcc8c(track, −1)`
   puts it in the **first empty slot** of the track; if the plug is already on the track it logs "plug already exist
   on track", and if the track is full, "No free slots to assign plug to" — **the new user sample is then in no slot,
   and the next content mode deletes it**. The definitions are saved. No 24-MB check on this path.

Is it how TE's app adds a user sample? Not the desktop app: its file server (`FileServer::addSyncFile`,
`addRemoveUserPlug`) only sends `write` and `remove`, and in its build of the firmware `PlugManager::addUserPlug` has
no caller at all [code, app]. Its Unity layer has "import from disk" / "Click to import" for packs it then writes
the same way. The sender of `import` is probably TE's mobile app [inferred].

**Should our tools use it? No** (route B stays): it makes a flag-2 user sample with a crc32 id outside our range,
indistinguishable from the user's own and deleted by content mode once unassigned; we can't choose the slot, and a
full track leaves it in none; a name that matches a user's sample overwrites that sample's file; and an existing id
keeps its old definition, so an update can't change it. Its one advantage, the OS's own WAV conversion and
validation, our builder already does offline (`samples check` applies the same checks).

## 7. Sample engines of our own

Yes, by the code: an engine of ours on a plug that names a pack gets the pack id in `init` and can call `getSample`.
The plug: `{"engine": "engines/<ours>.engine", "samples": "samplepacks/opzk_<kit>.aif", …}` (the pack
uploaded first, as above). Sketch:

```c
static const ModuleCallbackInterface *g_cb;
typedef struct { uint32_t pack; const OpzSampleSlice *s; uint32_t pos; /* … */ } Voice;

void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    Voice *v = state;
    v->pack = arg2;                 /* crc32 of the plug's samples path; 0 = no pack */
    v->s = 0;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    Voice *v = state;
    v->s = 0;
    if (v->pack && g_cb->getSample)
        v->s = g_cb->getSample((int)v->pack, (uint8_t)note);   /* valid until this voice ends; NULL: stay silent */
    v->pos = 0;
}
```

`render` then reads `v->s->data[pos]` up to `v->s->frames` with our own DSP (pitch, filters, granular, …) and
the host envelope; `v->s->rate / 2^27` is the key's own transposition (§8). Don't wait for
`sequencerGetValue(8)` (§5), and don't keep the pointer past the voice's end: the lock is gone then. Limits: the
voice state is 280 bytes; the pack occupies one of the 14 cache slots; reads come from SDRAM (budget them in the
cycle model). The simulator serves packs the same way: a scenario attaches an `.aif` with `sample FILE
[SAMPLES_PATH]` (drum packs, synth samples, plain AIFFs), `init` gets crc32 of the samples path, `getSample` returns
that key's `OpzSampleSlice` with every word as the loader builds it, and a plug whose pack is `unloaded` has its
notes dropped ([sim/README.md](../sim/README.md); the host check `sample_probe` compares every slice word with
`tools/opz_sampleplay.py`). The cycle driver serves a fake half-second slice to an engine whose plug names a pack
(`tools/cycles/driver.c`). engines/opz_test_sampler is this sketch as a test
engine, for H-S2 (§10).

## 8. Playback: what TE's engines do with a slice (Q-26)

Decoded from TE's engines on the device (`sampleplayergain` `onNoteOn` `0x81b69f20`) and their twins in the desktop
app, which carry symbols (`SamplePlayerGain::onNoteOn`/`render` `0xc7c8`/`0xb7f4`, `SynthSampler::onNoteOn`/`render`
`0x95d4`/`0x8b64`, `drumPitchToDetune` `0x4000c`, `op1SynthBaseFreqToDetune` `0x4017c`, `s_pitches` `0x7e964`), and
the loader that fills the slice (`FUN_80107648`/`FUN_801076d8`, `FUN_80107760`, the same float maths) [code]. The
model is `tools/opz_sampleplay.py` (`table`, `slices FILE`), tested in `tools/tests/test_sampleplay.py`; its P1 table
equals TE's to the last bit. Not heard on hardware yet (H-S1 step 6 listens for the cheap parts).

**From the pack file to the slice** (what a kit builder controls):

| Slice field | From the pack | What TE's engines do with it |
|---|---|---|
| `+0x00` data, `+0x04` frames | drum: `start[i]`..`end[i]` (Q31 of 529,200 frames, truncated); synth: `knobs[0]`..`knobs[3]` (Q15 of 264,600) | the region that plays; `sampleplayergain` keeps 64–192 frames of margin at the end |
| `+0x08` bits 0–15 | drum `volume`; synth `knobs[7]` | **gain = (volume / 8192)²**: 8192 unity, 16,384 +12 dB, saturating; times velocity, the track level and a per-note gain table |
| `+0x08` bits 16–23 | drum `playmode >> 7` | only for **editable** packs on tracks whose note style is retrig: `3 × byte` < 256 (**< 11,008**) **gate**, < 512 (**< 21,888**) **trigger** (one-shot, = retrig), else **loop**. Not editable: ignored, the track's note style decides |
| `+0x08` bits 24–31 | drum `reverse >> 7`; synth `knobs[4] >> 7` | drum: byte > 127 (**≥ 16,384**) plays backwards; so does **P2 past its centre** (word > `0x40000000`). Synth: backwards when editable and ≥ 16,384 |
| `+0x0C` rate | drum: `pitch` → cents = ⌈\|pitch\| × 4800 / 24,576⌉ (away from zero; low 9 bits cleared first below `drum_version` 3, so **512 = one semitone**) → Q31 of `2^(cents/1200) / 16`; synth: Q31 of `261.626 / (16 × base_freq)` | the speed: **`2^27` = original speed**, clamped to ±4 octaves (`[2^23, 2^31−1]`) |
| `+0x10`, `+0x14` | synth: `knobs[1] << 16 \| knobs[5] << 1`, `knobs[2] << 16 \| knobs[6] << 1`, clamped into start..end, relative to start | synth: the note **loops** between them while held when loopStart < loopEnd < frames; note-off lets it play to the end |
| `+0x18` pitch | drum `pitch` (masked) | not read |
| `+0x1C` editable | the pack's `editable` (default false; packs the OP-Z records or imports itself are editable) | enables playmode and synth reverse; in sample mode (key 8 = 1) these packs are edited live |

**`sampleplayergain` (drum tracks)**, per note [code]:

- Speed. `step = s_pitches[P1] × rate >> 27` (low 4 bits cleared), shifted left/right by the octave
  `⌊(note + 19) / 24⌋ − 3`; per output sample the position (Q10 frames) moves by `step × pitch >> 16`, where
  `pitch` is render's argument, **1024 on drum tracks** at standard tuning (`1024 × tune >> 30`; engine-abi.md said
  ≈2048, which would play every kit an octave high). So frames per sample = `rate/2^27 × 2^(P1 semitones/12) ×
  2^octave × pitch/1024`. `s_pitches[i] = round(65536 × 2^((i − 32)/12))`.
- **P1 = pitch**: byte 0–123 → −32…−1 semitones (four values per semitone), **124–132 → 0 (a centre detent)**,
  133–255 → +1…+31. **P2 = direction**: past the centre plays backwards, otherwise the key's reverse knob decides.
- Envelope words (shown as attack/decay/sustain/release, used as start/attack/hold/decay): start frame =
  `min(frames − 512, start × frames)`; attack = half the rest × attack (at least `2^22` once start is moved); hold
  = the rest × hold, at least 512 frames; decay = what's left × `2 × min(decay, 0.5)`. In **loop** style the
  playback jumps from the hold end back to the attack end while the key is held (with a short crossfade), in **gate**
  style note-off starts the release; otherwise one-shot. (`opz_sampleplay.drum_regions`.)
- Reading: 4-point cubic interpolation, Q10 position, one slice read per frame from SDRAM.

**`synthsampler` (synth tracks)**: `getSample(pack, 0)`; per output sample the position moves by `((pitch × rate
>> 27) & ~15) × 0x15122B6B >> 43` Q10 frames (at least 1), where `pitch` is the note's Q32 frequency: **a sample
recorded at `base_freq` plays at its own speed on that note** (C4 by default), and an octave up doubles it.

**Knob values for our kit builder** (`opz_sampleplay.pitch_knob`, `playmode_knob`, `volume_knob`): pitch
`semitones × 512` (±48); playmode gate 5,440 / trigger 16,384 / loop 27,264; reverse 0 forward, 32,767 backwards;
volume `8192 × √gain`. Our builder writes playmode 8192 (gate) and reverse 8192 (forward) and no `editable`, so the
playmode is ignored and every key is a one-shot on a retrig track. **What a TE kit uses**: TE's `UserSampler.aif` has
volume 8192, pitch 0, reverse 12,000, playmode 10,239–14,335 and no `editable` [proven, TE file].

Still open after Q-26: the per-note gain table (`frequencyGainTable`) values, the loop crossfade length, and
whether the device's float library rounds `2^(cents/1200)` exactly like the app (a 1-LSB difference at most).
Remaining sample questions: Q-25 (H-S1 below) and the hardware half of Q-27 (H-S2, §10).

## 9. Hardware check H-S1: Q-25 (for the lead; first run stopped 2026-11-01, rerun below)

Question: does a pack written with the file service (`write` to `samplepacks/opzk_*.aif`, one folder deep: Q-49) plus a
`patch.json` plug of ours on TE's `sampleplayergain` load and play? The safest form of the test: a **tiny kit of
our own** (24 sine blips, 0.96 s, 86 KB, synthesised by the tool), at a path no plug names, as **plug 61400 in an
empty drum slot** (never `--replace`), every step read back, and a rollback that leaves no pack behind. Normal
mode only — **no content (disk) mode** at any point. TE's app closed, the user approves each write.

1. **The user's samples are safe**: `.venv/bin/python tools/opz.py samples list`. If any user sample shows NO SLOT,
   stop and put it back first (`opz.py assign ID --track T --slot N`, or `opz.py restore` of the original map).
   Note an **empty kick slot** from `opz.py status` (kick slots 7–10 were empty in the last backup; check again)
   and that no plug names `samplepacks/opzk_testkit.aif`.
2. Offline: `.venv/bin/python tools/opz.py samples build build/samples/testkit.aif --test-kit tiny --name testkit`,
   then `samples check build/samples/testkit.aif`: 24 keys, 0.96 s, no errors. Optional:
   `.venv/bin/python tools/opz_sampleplay.py slices build/samples/testkit.aif` (every key rate 1.000, forward).
3. Dry run: `.venv/bin/python tools/opz.py samples install build/samples/testkit.aif --name testkit --track kick
   --slot 7 --dry-run` (the empty slot from step 1). Expect plug 61400 v1 on `engines/sampleplayergain.engine`,
   `samplepacks/opzk_testkit.aif`, kick slot 7, a patch.json whose engine entry equals the installed one
   (`memoryslot` 10), no errors. An occupied slot is refused without `--replace` — never add it.
4. Install (the same without `--dry-run`, approve; about 430 parts, ~10 s). The tool backs up both settings files,
   then: sync 1 the pack; sync 2 the same file again, **which the OP-Z must skip** (proof it holds exactly these
   bytes — packs can't be read back); sync 3 patch.json + the slot map; then it reads plugs.json and the slot map
   back and checks plug 61400, its path, engine and version, kick slot 7, **and that every user sample is still in
   its slot**. Expect "sync 2 (probe): the OP-Z holds the pack" and "installed and verified". Note the backup
   directory.
5. `samples list`: plug 61400 under "ours" in kick 7; the user's samples unchanged, none in NO SLOT.
6. Listen (kick track, hold kick + black key 7; volume low): keys 1–24 play a **rising ladder of blips from C5,
   one semitone per key** → **Q-25 yes**. Cheap extra checks of §8: P1 at the centre leaves the pitch alone and a
   small turn does nothing (detent), turning it steps the pitch in semitones; P2 past its centre plays the blips
   backwards (a soft attack instead of a click); the octave buttons give the same 24 blips an octave apart. Then
   the user's own kick sample (its slot) still plays.
7. Restart the OP-Z; slot 7 must still play (the pack is a file, the plug a definition — not RAM like presets).
8. **Rollback**: `.venv/bin/python tools/opz.py samples remove 61400 --unassign` → the tool unassigns, removes the
   plug, and reads back that it is gone; the OP-Z deletes the plug's pack file with it (`FUN_800fe700`). Then
   `samples list` once more: no plug 61400, user samples unchanged. (Keep the kit installed instead if H-S2 follows.)

### First run (2026-11-01, the user's OP-Z): stopped at the probe, nothing installed

Steps 1–3 as written (all 9 user samples in a slot; kick slot 7 empty; kit built and checked, 85,986 bytes; dry run
clean). Step 4: sync 1 sent the pack (requested, "sync complete"); **sync 2 sent the same file again and the OP-Z
requested it again** instead of skipping it, so the tool stopped: "the OP-Z asked for the pack again, so it did not
keep it; no plug was installed" (exit 1) [proven]. A fresh read afterwards: `plugs.json` byte-identical to the backup
before the install, no plug 61400, kick slot 7 empty, user samples unchanged [proven]
(`backups/20261101T143333108297Z-samples-testkit`, `backups/20261101T143440385605Z`).

**Explained offline (Q-49, §6 "The sync job"):** the probe was right and the upload never landed. The OP-Z stages
`inbox/samplepacks/user/opzk_testkit.aif` with a single `mkdir`, which fails without `inbox/samplepacks/`. It took
the parts, reported complete, and its finalise step found no file with the CRC, so it copied nothing [code]. Sync 2
failed the same way. **So no `opzk_testkit.aif` is on the device**, not even in `inbox/`, because the file could
never be created [code; sync 2's request is the device's own word that the path didn't hold the pack]. There is
nothing to purge. The tools now write `samplepacks/opzk_<name>.aif` and refuse two-level paths. Before Q-49 was
answered, the note read: the probe's premise — the OP-Z requests only files whose CRC differs from what it holds —
does not hold for this file [proven]. Either the OP-Z does not compare `samplepacks/user/` files by CRC (it may only skip files
it tracks, such as engines and settings), or it did not keep the pack (for instance by deleting a pack no plug names)
[inferred; Q-49]. The premise was never seen on hardware before [the doc's "only downloads files whose CRC differs" had no
evidence label]. So **whether an 86 KB `opzk_testkit.aif` is now on the device is unknown**; no plug names it, so it
cannot break content mode (rule 4 is about plugs naming missing packs). `samples purge` relies on the same probe and
was not run. H-S1 and H-S2 wait for Q-49.

**If a step fails**: step 4 stops on its own before any plug names the pack when the probe fails ("did not keep
it"), and restores the slot map when the read-back differs. Either way a pack file of ours may be left with no
plug naming it (`samplepacks/opzk_testkit.aif` now): `.venv/bin/python tools/opz.py samples purge testkit
--dry-run`, then without `--dry-run`. It
overwrites the path with a 64-frame placeholder (and proves it landed), names it with a temporary plug of ours in
no slot, then removes that plug, so the OP-Z deletes the file — no orphan pack, and at no point a plug naming a
missing file. If the kit is silent although plug 61400 is in slot 7: the pack is not loaded (the OS then drops every
note, §5) — note it, check `opz.py status`, and roll back as in step 8.

### Rerun checklist (for the hardware session, after Q-49)

Same set-up: TE's app closed, project 9, volume low, normal mode only. The user approves each write and sees each
result before the next step. Read-only steps are marked (ro).

0. Offline, on the machine that talks to the OP-Z: pull the branch with the Q-49 fix and check
   `.venv/bin/python -m unittest discover -s tools/tests` passes (`SyncJobRule` included).
1. (ro) `.venv/bin/python tools/opz.py samples list`: 9 user samples, none in NO SLOT; no plug 61400; no plug names
   `samplepacks/opzk_testkit.aif` or `samplepacks/user/opzk_testkit.aif`. (ro) `opz.py status`: kick slot 7 is
   still empty. If either differs from the 2026-11-01 read, stop and report.
2. Rebuild the kit (offline, step 2 above). The bytes are the same as on 2026-11-01 (85,986 bytes); the path changes.
3. Dry run (step 3 above). It must show `samplepacks/opzk_testkit.aif`. If it shows `samplepacks/user/`, the
   machine is on the old tools: stop.
4. Install (step 4 above, approve). Expect sync 1 "requested ['samplepacks/opzk_testkit.aif']", then "sync 2
   (probe): the OP-Z holds the pack", then "installed and verified". If sync 2 asks again: stop, nothing is
   installed. Report the full log, because that would contradict `FUN_8012bb00` (and then a pack could be at the
   path after all: `samples purge testkit --dry-run` shows the clean-up, which runs only on the user's yes).
5. Steps 5–8 above (list, listen, restart, rollback or keep for H-S2).

## 10. Hardware check H-S2: Q-27 (for the lead; not run)

Goal: our own engine, on a plug that names H-S1's pack, plays that pack's slices. Engine:
engines/opz_test_sampler (plug 60905, slot 72, `samples`
`samplepacks/opzk_testkit.aif`); it beeps instead when it gets no id or no slice, with a different pitch for
each reason (table in its README). **Only after H-S1 passed** (steps 1–7), with the kit still on kick slot 7. Same set-up: TE's
app closed, project 9, volume low, the user approves each write.

1. **The pack is there**: `python3 tools/opz.py samples list` shows plug 61400 with
   `samplepacks/opzk_testkit.aif`, and no user sample shows NO SLOT (H-S1 step 1). If 61400 is missing, stop:
   a plug naming a missing pack breaks content mode, and `opz.py install` refuses this one ("install the pack
   first").
2. Offline: `make -C sim test` passes; `python3 tools/opz_build.py engines/opz_test_sampler` and
   `python3 tools/opz_verify.py build/bfin/opz_test_sampler/opz_test_sampler.engine --slot 72 --strict` → PASS.
3. Dry run: `.venv/bin/python tools/opz.py install engines/opz_test_sampler --track kick --slot 8 --dry-run`. Expect
   plug 60905 v1 on `engines/opz_test_sampler.engine`, memory slot 72, kick slot 8, no errors. Slot 72 stays taken
   on this OP-Z afterwards (engine definitions are not removed; [memory-slots.md](../sdk/docs/memory-slots.md)).
4. Install (the same without `--dry-run`, approve). Note the backup directory it prints.
5. Listen: kick track, **slot 8** (hold kick + black key 8), envelope knobs at their defaults. Play keys 1–24 and
   compare with slot 7 key by key:
   - **the same 24 blips in the same order** (the rising ladder from C5) at the same pitch → **Q-27 yes**, and the
     key mapping `(note + 19) mod 24` holds. The pack's volume is 8192, so TE's player is at unity gain too (§8):
     the levels should match; note any difference.
   - the octave buttons still give the same 24 blips (our engine ignores the octave; TE's shifts it);
   - a **low beep** (220 Hz) on every key → no pack id in `init`; a **high beep** (1760 Hz) → `getSample` returned
     NULL; **silence** → the OS dropped the notes (pack not loaded, §5). Note whether that changes after a few
     seconds (the pack loading).
6. Switch slots 8 → 7 → 8 and back while playing, and play fast repeated hits on slot 8: no crash, no stuck sound.
   Then slot 7 (TE's player) and the user's own kick sample (slot 6) still play.
7. Restart the OP-Z; slot 8 must still play the kit. Note whether the first hits after the restart are silent (the
   pack still loading).
8. Report the outcome of step 5 (kit / which beep), anything from steps 6–7, and the level difference.

Rollback: put the slot map back with `.venv/bin/python tools/opz.py restore <backup dir from step 4>`. **Do not try
to remove plug 60905**: the OS deletes a removed plug's pack file, and 61400 uses the same file, so `opz.py remove`
refuses it (and refuses `samples remove 61400` while 60905 names the pack). An unassigned 60905 is harmless while
61400 holds the pack. Removing both plugs together needs a tool change first (follow-up).
