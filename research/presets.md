# Plug presets and live sound parameters (`$0E`)

Findings for the open half of **Q-09** (what `slot` and `index` mean, what the OS does with a plug's `presets`) and
for **F-03** (a preset editor with live audition). Offline work: TE's desktop app, the device image and the
factory content of firmware 1.2.45, no hardware. Decompiled TE code stays in `build/` and the session scratchpad
(git-ignored); this page gives our description and the addresses to check it.

Evidence labels: **[code, app]** = `SnakePlugin` arm64 slice (symbols present; `lipo -thin arm64`, then
`objdump -d -C --arch=arm64`, or Hopper through REA). **[code, device]** = `opz.ldr` with the Blackfin+ extension in
Ghidra (`tools/ghidra/os_project.sh`); the OS builds addresses with 48-bit immediate loads, so string users were
found by scanning for the `hi, lo` halfword pair, and callers by decoding `CALL`/`JUMP` offsets. **[data]** = a file
from the firmware's `system/content`. **[proven, notes]** = hardware result published in the notes repo
(`refs/op-z-sysex/docs/native-state-sync-analysis.md`).

## Summary

| Question | Answer |
|---|---|
| What is `slot`? | **The synth track the preset is for**: 0 bass, 1 lead, 2 arp, 3 chord. The OS looks presets up with `slot = track − 4` on tracks 4–7 and **`slot = 0` on every other track** (drums, FX, tape, master) [code, app + device]. TE's factory cache follows this: bass presets carry `defaultOctave` −2 [data] |
| What is `index`? | The preset number within that slot, **0–13** (14 per slot); other values are ignored. `slot` is clamped to 0–3 [code, app + device] |
| How many are kept? | 4 slots × 14 = 56 per plug. Each keeps a name of **11 characters** (longer names are cut), the 18 values and `defaultOctave` (clamped to −4…4; −1 if missing) [code, app] |
| Does the device apply them? | **Yes.** When a track switches to a plug and the pattern has no stored parameters for that plug, the OS applies **preset `index` 0** of the track's slot: the 18 values (except pan, and on the arp track the four LFO values), level scaled to the track, and `defaultOctave` as the track's octave. With no preset there, it falls back to TE's per-track default sound [code, app] |
| Who lists them? | The device: in the track's sound-selection context a key press applies the preset with that key's index and a key hold saves the current sound into it (a *user preset*) [code, app]. **White key *n* = `index` *n*−1** (F3 = 0 … E5 = 13; black keys never recall a preset) [code, app: key table, see below]. **FX tracks have no user-assignable presets** [proven, user on the device]: an effect plug only ever gets slot 0 index 0, when it is chosen — which is why TE's FX plugs carry one slot-0 preset. TE's app has no preset browser: it only copies presets from TE's DLC catalogue into the `patch.json` it sends [code, app managed DLL; inferred from its strings] |
| Do they survive a restart? | **Not on their own.** At boot the OS clears every plug record, reloads `plugs.json` (which has no presets) and takes presets only from the factory cache `settings/plugPresets.dat` and from `config/userPresets.dat` (presets the user saved on the device). Nothing ever rewrites the factory cache [code, app + device]. Imported presets live in RAM until the next restart [proven, user 2026-10-19]. **But** boot also imports `settings/plugs_patch.json` before the preset caches, then deletes it [code, device]: our tools now write that file (the *boot file*), so the presets come back after **one** restart without a push; every install, `presets push`, `remove` or `presets persist` writes it again — see [The boot file](#the-boot-file-settingsplugs_patchjson-2026-10-19); **hardware check H-7**. **For presets that stay with no computer:** presets stored by hand survive [proven, user], and content mode exports `config/userPresets.dat` to its disk and imports an edited one on eject [code] — so writing our presets into that file as user presets makes them permanent; see [Presets that stay](#presets-that-stay-configuserpresetsdat-through-content-mode-2026-10-19) |
| Updating presets | A plug in `auto_import` is skipped unless its version is higher. **A plug in the top-level `plugs[]` array is imported without a version check** (no engine import, no track assignment), so presets can be replaced without a version bump [code, app + device] |
| Why only white key 1 lights for our plugs | **The blinking key 1 is not a preset.** Choosing a sound slot sets the track's remembered preset index to 0 when it has none, and the LEDs blink that index whether or not a preset is there; presets light **solid**. One lit key = an empty preset mask. Our `patch.json` has the shape both OS parsers read, and nothing at runtime clears imported presets; only a restart does. So the presets were lost (or never arrived) before the test, and a restart between the install and the test explains it. Not settled: **H-6** [code, app + device; cause inferred] |
| `$0E` | Payload (before 7-bit packing) = **track (0–15) + the 18 parameter bytes**, exactly 19 bytes. It writes the **active pattern's** sound block for that track and the live parameter buffer: the sound changes at once and is saved with the project. It does not select a plug, apply a preset or change the octave [code, app; proven, notes] |

## Where presets live

Each plug record (`PlugDefinition`, 0x7ac = 1964 bytes, 200 records) holds a preset block at `+0xCC` [code, app
`readPresetSettings` @ `0x43b48`, `getPreset` @ `0x469bc`; device `0x800fb878`]:

| Offset in the block | Size | Content |
|---|---|---|
| `slot·434 + index·31` | 12 | name, NUL-terminated (11 characters kept, `strncpy` 11) |
| `+12` | 18 | the 18 parameter values (a value that is not a JSON integer becomes 0) |
| `+30` | 1 | `defaultOctave`, signed |
| `1736 + slot·2` | 2 × 4 | preset mask per slot: bit `index` set = preset present |
| `1744 + slot·2` | 2 × 4 | user mask per slot: bit set = saved by the user on the device |

The flags byte is at record `+1960` and the version at `+1961` — **one byte**: keep `plug.version` below 128 (the
app compares it as a signed byte, the device as unsigned) [code, app `readPlugSettings` @ `0x43968`, device
`0x800fc0b8`].

`settings/plugPresets.dat` and `config/userPresets.dat` are dumps of the blocks: a 2-byte header (`01 00`) and
200 records of 1756 bytes (block, both masks, plug id `u32` at `+1752`) [data: 351,202 bytes in 1.2.45's content;
code, app `savePresets` @ `0x46bb4`, `loadPresets` @ `0x4476c`].

### How TE uses them [data, `settings/plugPresets.dat`]

53 plugs have records. 26 have no presets, 16 one preset in slot 0 only (FX and drum packs; often named
`default`), and the synths four per synth slot (masks `0xf, 0xf, 0xf, 0xf`; one plug has eight in slot 3). Bass
presets (slot 0) mostly use `defaultOctave` −2, lead and arp 0…+2. Many factory presets have empty names.

## Import

`readPresetSettings(plug, presets)` [code, app `0x43b48`; device `0x800fb878`] walks the array. An entry is used
only if it has an integer `index` 0–13, a string `name` and an array `values`; `slot` defaults to 0 and is clamped
to 0–3; `defaultOctave` defaults to −1 and is clamped to −4…4. It **sets** the mask bit and overwrites that
`slot`/`index`; it never clears other bits.

`importPlugFromPatch` [code, app `0x45540`; device `0x800feca0`] clears the masks only when the plug id is new.
Re-importing an existing plug therefore **merges**: given `slot`/`index` pairs are replaced, others stay until the
next restart.

`importPatch` [code, app `0x442e4`; device `0x801001d6`]:

1. `auto_import[]`: needs `plug.id` and `plug.version`; skipped unless the version is higher than the installed one.
   Imports the engine, the plug (with presets) and assigns `tracks`.
2. `engines[]`: engine definitions.
3. `loadRequestedEngines`.
4. **`plugs[]`: `importPlugFromPatch` for each, no version check.** `readPlugSettings` overwrites the plug's
   `engine`, `samples`, `flags` and `version` with what the entry gives, so a presets-only update must repeat the
   installed values.
5. `exportPlugDefinitions` rewrites `settings/plugs.json` (without presets).

`importPresets(path)` and `exportPresets(path)` read and write a JSON file of `{plugId, presets}` records
(`settings/plugPresets.json`), but nothing calls them on the device (no code refers to the path) or in the app
[code, app; device: the string at `0x8105942c` has no user].

### The device's import, checked against our `patch.json` (2026-10-19)

On the device (2026-10-19), only white key 1 lit for `table` (installed through `auto_import` with 5 presets per synth
slot) and for the plugs pushed through `plugs[]` on 2026-10-18, so the presets did not reach the device. A second
offline pass read the OS image's import code line by line and compared it with the exact `patch.json` that
`opz.py install` sends (`--dry-run` prints it). **Every step accepts our file** [code, device; strings at
`0x81059294`–`0x810592b4`]:

| Step | Device code | Our file |
|---|---|---|
| FileClient applies a finished sync job, renames each file from `inbox/` and calls `importPatch(path, true)` for a path containing `patch.json` | `0x8012819a` (string `patch.json` at `0x81114484`) | `settings/patch.json` ✓ |
| `importPatch`, `auto_import[]`: reads `id` and `version` from `entry.plug` (`readJsonUInt`), imports only if `comparePlugVersion(id, version) < 0` (not installed, or lower version) | `0x801003a0`–`0x801003be` | new plug, or version bumped (`install` refuses otherwise) ✓ |
| … then `importEngineFromPatch(entry.engine)`, `importPlugFromPatch(entry.plug)`, tracks | `0x801003e4`, `0x8010040a` | presets inside `plug` ✓ |
| `importPatch`, `plugs[]`: `importPlugFromPatch` for each entry, no version check | `0x8010068c` | `presets push` ✓ |
| `importPlugFromPatch`: finds or allocates the record; a **new** id clears the four slot masks (8 bytes at record `+0x794`); `readPlugSettings`, then `readPresetSettings(record, plug["presets"])` | `0x800feca0`, mask clear `0x800fecfc`, call `0x800fed6a` | ✓ |
| `readPresetSettings`: array; per entry `index` (int, 0–13, required), `slot` (int, default 0, clamped 0–3), `name` (string, required), `values` (array, required; non-integers → 0), `defaultOctave` (int, default −1, clamped −4…4); sets bit `index` of the mask at record `+0x794 + slot·2`, name (11 chars) at `+0xcc + slot·434 + index·31`, values at `+12`, octave at `+30` | `0x800fb878`–`0x800fba78` (slot clamp `0x800fb8fe`, mask `0x800fb974`–`0x800fb994`, octave `0x800fba48`) | keys, types, ranges and placement match ✓ |

The device code is the same as the app's (`readPresetSettings` @ `0x43b48`, `importPlugFromPatch` @ `0x45540`,
`importPatch` @ `0x442e4`). The plug record's name, `trackmask` and `parameter_names` are not read by the OS:
`readPlugSettings` takes only `flags`, `version`, `engine` (lower-cased) and `samples` [code, device `0x800fbd14`].

**Nothing at runtime clears imported presets.** The only writes to the masks are `readPresetSettings`, the new-plug
clear above, `storePreset` (a user save) and `loadPresets`, which only ORs factory and user presets in. The 200
records are zeroed only by `_importPlugDefinitions` (`memset` at `0x800ff454`), whose one caller (`0x800ff9a8`) runs
only from boot init (`0x801007c2`). After a sync job the OS reloads only the slot map (`reloadSlotConfiguration`
@ `0x800ff1cc`, called at `0x80127d20`; `loadSlotConfiguration` @ `0x800fedf0` does not touch presets). At boot it
imports `settings/plugs_patch.json` if present (then deletes it) but **not** `settings/patch.json`
(`0x801007e0`–`0x8010086c`) [code, device].

**Conclusion** [inferred]: the format is not the cause. The presets were in RAM after the import and were gone by the
time the keys were checked: the OP-Z must have restarted, been switched off or crashed in between (the table test
came about three hours after the install; on 2026-10-18, the day of the presets push, macro crashed the OP-Z
repeatedly, and when the keys were checked relative to those crashes is not recorded). A
device-side difference that static reading misses cannot be ruled out. **H-6** settles it: push and count the
solid keys at once, without a restart in between. `opz.py install` and `presets push` now say that the presets are
RAM-only and print which white keys should light per track.

## Persistence

| When | What happens | Evidence |
|---|---|---|
| Boot | `plugs.json` import zeroes all 200 plug records (`memset`, `0x5fe60` bytes) and reads id, engine, samples, flags, version | [code, app `_importPlugDefinitions` @ `0x45020`; device `0x800ff454`] |
| Boot | If `settings/plugs_patch.json` exists (TE's factory plugs, no presets): import, then delete it | [code, device `0x801007e0`; app `preInit` @ `0x43fac` uses `patch.json`] |
| Boot | `loadPresets`: copy each record of `settings/plugPresets.dat` whose plug id exists, then from `config/userPresets.dat` only the presets with a user-mask bit | [code, app `0x4476c`; device `0x800fce20`] |
| Content-mode import done | `loadPresets` again | [code, device `0x80039012`] |
| User holds a key | `TrackContext::savePlugPreset` → `storePreset`: current 18 values and octave into that index, mask and user-mask bits set, dirty flag set | [code, app `0x39514`, `0x46af8`] |
| Main loop, dirty flag set | `savePresets(user = true)` writes **all** blocks to `config/userPresets.dat` | [code, device: single caller `0x8012a4e6`, `R1 = 1`, guarded by the byte at manager `+0x2a4`] |

The factory cache is never written by the OS (the one `savePresets` call passes `user = true`). Presets that came in
through `patch.json` have no user bit, so they are not restored from the user file either. **Consequence:** after a
restart our plugs have no presets until a `patch.json` brings them again — `opz.py presets push` (no version bump
needed), a fresh install, or the boot file below, which the OS imports at boot.

### The boot file: `settings/plugs_patch.json` (2026-10-19)

The user wants the presets to survive a restart (on 2026-10-19 a push made them appear at once, H-6, and a
restart wiped them). A third offline pass read the boot path in the OS image and every place presets come from at
boot. All addresses are **[code, device]** (`opz.ldr`, Ghidra project of `tools/ghidra/os_project.sh`) unless marked.

**Boot order** (`0x801007c2`, the plug manager's part of boot init, in this order):

| Step | Code | What it does |
|---|---|---|
| 1 | `0x800ff948` → `0x800ff454` | `settings/plugs.json`: `memset` of all 200 plug records, then id, engine, samples, flags, version (no presets) |
| 2 | `0x800ff1cc` | slot map |
| 3 | path from `[0x81059270]` = `settings/plugs_patch.json` (string `0x810593dc`, after the root prefix); `0x80128a78` = exists (`open() != −1`, `0x8000e762`) | if the file is there: |
| 4 | `CALL 0x801001d6` at `0x8010086c`, `R2 = 0` | `importPatch(path, false)`: the same function as the runtime import (`importPatch(path, true)` at `0x8012819a`). The flag only reaches `auto_import`'s track handling; `plugs[]` does not read it (importpatch decompile, `0x801001d8`) |
| 5 | `CALL 0x80128754` at `0x80100894` → `0x8000df34` | **deletes the file** (name of `0x8000df24` inferred). Reached whatever the import returned: there is no branch on its result (`0x80100870`–`0x80100898`) |
| 6 | `CALL 0x800fce20` at `0x8010081a` | `loadPresets`: factory cache, then user file (below) |

So a `plugs_patch.json` with our `plugs[]` entries — exactly the entries `presets push` sends — is imported at
boot, **after** the records are cleared and **before** the preset caches load. Its presets survive the rest of boot:

- `loadPresets` copies a factory record (`settings/plugPresets.dat`) only for plug ids it lists (`memcpy` of the
  whole 0x6dc-byte block into record `+0xcc`, loop at `0x800fce20`; id at record `+0x6d8`). Our ids are not in TE's
  file, so our blocks are left alone.
- Then `config/userPresets.dat`: only presets with a user-mask bit, 31 bytes each, OR-ing the mask and user-mask bits.
  A preset the user stored on our plug **wins** over the boot file's for the same slot/index (it is copied after).

**Import details that matter for safety** [code, device]: `importPatch` reads at most `0x40000` bytes into
`0x81019260` (`func 0x80128ae0`, start of `0x801001d8`); if the JSON does not parse it logs `Failed to import update
file` and returns (`0x801001d8`, string `0x81059730`); step 5 still deletes the file. `plugs[]` entries are imported
without a version check; an id that is not installed gets a **new** plug record (`importPlugFromPatch`
`0x800feca0`), so a stale boot file would bring back a plug that was removed since it was written — our tools write it
again after every `remove`. After step 4, `importPatch` re-exports `plugs.json` (`0x800fd2ec`), as at runtime.

**The file is also imported at once.** FileClient imports every written file whose path contains `patch.json`:
`string::find` against `npos` at `0x80128108`–`0x80128116` (first `projects/project`, string `0x811146cc`, which
reloads the project) and `0x80128122`–`0x80128130` (`patch.json`), then `importPatch(path, true)` at `0x8012819a`;
nothing deletes it at runtime. So writing the boot file is also a presets push for every plug in it, and the
runtime import is a canary for the boot import: same function, same bytes.

**TE's own use** [data]: firmware 1.2.45's `system/content/settings/plugs_patch.json` (2,730 bytes) holds five
`auto_import` entries (TE's plugs, no presets, no `plugs[]`). Since every start deletes it, the device normally has
no `plugs_patch.json` [inferred]: writing ours replaces nothing of TE's. The file service does not serve it (whitelist:
`slotConfiguration.json`, `plugs.json`, a few virtual ids), so it cannot be backed up or read back.

#### Options, ranked by risk

| # | Option | Persists | Risk | Verdict |
|---|---|---|---|---|
| 1 | **Boot file `settings/plugs_patch.json`**, plugs[] only, our plug ids only | **One restart per write**: the start that imports it deletes it | Low. TE's own boot mechanism; the same import our pushes use (proven at runtime, H-6), on a file that is normally absent; a bad file is deleted at the next start; no TE file is touched. Residual: a crash *inside* the boot import would repeat at every start (the delete comes after it) — the runtime import of the same bytes runs first and would show it; recovery then = TE's firmware installer in update mode, whose content brings TE's `plugs_patch.json` [inferred] | **Done** (below) |
| 2 | Re-push after each restart (`presets persist` or `presets push` with several engines) | Until the next restart | None beyond today's push | The fallback; also what re-writes option 1 |
| 3 | User-stored presets (hold track + white key 2 s on each preset) → `config/userPresets.dat` | Yes [code; H-2b unchecked for user plugs] | None (TE's own feature), but manual per preset; a store takes the current sound, so pan and the track-scaled level differ from ours | Recommend for favourites |
| 4 | Merge our records into `settings/plugPresets.dat` | Until a firmware update | **Out.** The file service cannot read it, so no backup and no read-back of what the device holds. If it is missing or not exactly 2 + 351,200 bytes with header 1, `loadPresets` returns **before the user file** (`0x800fba9c` "Wrong file format"/"Failed to read all bytes"; branch at `0x800fcea2`–`0x800fceb2`): TE's factory presets *and* the user's stored presets are gone from RAM, and the next user store rewrites `config/userPresets.dat` from RAM — the user's presets lost for good | Not done |
| 5 | Write `config/userPresets.dat` | Yes | **Out.** Cannot be read back, so it would overwrite every preset the user stored on any plug | Not done |
| 6 | `settings/plugPresets.json` (`importPresets`) | — | Nothing calls it on the device (no reference to `0x8105942c`) | Not possible |
| 7 | An engine writing into the OS's plug records | — | No memory protection; corrupting the OS from DSP code | Out |

#### What the tools do

- `opz.py presets persist [ENGINE_DIR ...] [--off] [--dry-run] [--yes]`: backup, then the boot file with a `plugs[]`
  entry for every **installed** plug that an engine directory describes (same id and engine path, presets without
  errors, id ≥ 60000) — by default every directory under `engines/`; TE's plugs and the app's sample plugs never go
  in. The entries repeat the installed engine, samples, flags and version, like `presets push`. Limit 128 KiB (all
  19 of ours with presets: about 25 KB). Verify: every plug record and the slot map read back unchanged after the
  runtime import; a copy of the file goes into the backup (`plugs_patch.json`). `--off` writes `{"plugs":[]}`
  (nothing imported now or at the start, which deletes it). If the OP-Z skips it as unchanged, it still holds that
  file: there was no restart since it was written.
- `install`, `presets push` and `remove` write it after they succeed (all of `engines/`, the engines just named
  first), each with its own confirmation; `--no-boot` skips it. So after an engine update, whose restart drops the
  presets, they come back by themselves.
- `@opz/core`: `persistPresets(engines, {off})`, `bootEntries`, `buildBootPatch`, `BOOT_PATCH`; `install` and
  `presetsPush` take `boot: true` for the advice line. The web app's Presets view has *Write the boot file…* and
  *Empty it…*, and offers it after each install, presets push and remove when its setting is ticked (off by default
  until H-7).
- The fakes model it: imported after a sync that wrote it and kept; `restart()` drops presets, imports the file and
  deletes it (also when it does not parse).

Limit: the boot file covers **one** restart. After a start has used it, run `opz.py presets persist` (one command:
re-imports every plug's presets and arms the next start). Pending H-7.

### Presets that stay: `config/userPresets.dat` through content mode (2026-10-19)

The boot file covers one restart. The user wants presets that stay with no computer attached. Two device results
settle what the OS itself keeps [proven, user 2026-10-19]: a preset stored by hand on our plug (hold track + white key
2 s) **survives a restart**, so the user-preset store works for user plugs (old H-2b); and the content-mode disk
showed **no** `userPresets.dat` in `config/` at that moment (see below why). A fourth offline pass read content mode.
All **[code, device]**, `opz.ldr`; content-mode strings found by scanning the LDR blocks for the 48-bit loads of their
addresses (the Ghidra analysis has no references to them).

**What content mode does with `config/`:**

| Step | Code | What it does |
|---|---|---|
| Enter content mode | `0x8003760c` → `0x8002ede0` (`mass_storage.cpp`, "create and mount FAT") | formats a **fresh FAT disk** every time, makes `projects`, `bounces`, `samplepacks`, then registers the synced folders |
| Register a folder | `0x800371b0` (100-byte entries in the vector at `0x0802cc3c`) | `rejected`, `config` with the filter **`json|dat`** (string `0x81005280`) and the import callback `0x80034d68` (address loaded at `0x8003772e`), `projects`, `bounces` |
| Export, at registration | `0x80128bc8` (yaffs dir walk, call at `0x80037434`) → callback `0x80034fb0` → `0x8002e934` | every file in the device's folder whose extension passes the filter (`0x80034c94`) is **copied onto the disk**: so `config/userPresets.dat` is put on the disk whenever it exists on the device |
| Eject | `0x80038cd8` ("[IMPORT STARTED]" … "[IMPORT COMPLETE]") → per folder `0x80035798` | each disk file that passes the filter goes to the folder's callback; a refused file is moved to `rejected/` |
| `config` callback | `0x80034d68` | equality tests (`0x040233c4`) against `userPresets.dat` and `dmx.json`: any other config file is imported only if the device already has it; **`userPresets.dat` is imported even when the device has none**; a file whose checksum (`0x80128804`) equals the device's is skipped; `dmx.json` must parse as a DMX config (`0x8003b7c0`); then `copyFile(src, root + name, overwrite)` (`0x80128930`) and "SUCCESS" |
| After the import | `0x80038fe4`–`0x80039012` | "Rebuilding plug definitions" (`0x800ffc88`), then **`loadPresets` (`0x800fce20`)** |

So TE built content mode to back up and restore the user presets file: it is exported to the disk and an edited one
is taken back, even onto a unit that never had one. It was missing from the disk in the user's look because the OS
writes `config/userPresets.dat` only on the first hand-stored preset (`savePresets(user = true)` from the main loop when
the dirty byte at manager `+0x2a4` is set, `0x8012a4e6`), and that look came before the store [inferred: order of
the two checks]. Firmware 1.2.45's content has no `config/` folder [data], so a firmware update should leave it alone.

**The file** (`savePresets` `0x800fd138`, `writeCache` `0x800fbb60`, `readCache` `0x800fba9c`): `u16` version **1**,
then exactly **200 records × 0x6dc (1,756) bytes** = 351,202 bytes, little-endian. The OS fills one record per
installed plug in plug-id order (its plug map at `0x0802de18`): a copy of the plug's preset block (record `+0xcc`), then
the plug id `u32` at `+0x6d8`; the rest of the 200 have id 0 and are skipped on load. Per record: slot *s* (0–3),
index *i* (0–13) at `s·434 + i·31`: name 12 bytes (NUL-terminated, 11 characters), 18 values, `defaultOctave` (signed
byte); preset mask `u16[4]` at `1736`, **user mask** `u16[4]` at `1744`. A wrong version or size makes `readCache` fail
("Wrong file format", "Failed to read all bytes"): then no user presets load at that start (the factory cache, read
first, is not affected).

**Load** (`0x800fcf8c`–`0x800fd058`, read instruction by instruction): for each record whose id is an installed plug
(`0x800fc060`), for each slot and index whose **user-mask** bit is set, copy the 31 bytes into the plug's block and set
both its preset-mask and user-mask bits. Nothing else is read: the preset mask in the file does not matter.

**Why this persists without a computer:** a preset with its user bit set is loaded at every start, and from then on
the OS rewrites the whole file itself from RAM (`savePresets` copies every plug's block, user masks included) whenever
the user stores a preset. A later `presets push` to the same slot/index replaces the values in RAM and keeps the user
bit, so the next save keeps the new values.

#### The route: edit `userPresets.dat` on the content disk

1. Content mode (hold **track** at power-on). The disk's `config/` has `userPresets.dat` once at least one preset was
   stored by hand (store any one first if it is missing; the site's loader instead makes the file, H-9b, not yet run).
2. On the Mac, `opz.py presets bake [ENGINE_DIR ...] --disk /Volumes/OP-Z` (offline, no MIDI;
   `tools/opz_userpresets.py`) checks the file strictly (351,202 bytes, version 1, no plug id twice, no mask bit past
   index 13, parse → write gives the same bytes), copies it to `backups/<time>-userpresets/`, and writes our presets
   into the records of our plug ids (a free id-0 record, zeroed, for a plug with none), **setting their user-mask
   bits**. Every other record, and every preset the user stored by hand at the same key, stays as it is
   (`--overwrite-user` replaces those). It writes the file in place (no temporary file on the disk), reads it back,
   and puts the original back if the bytes differ. `--show` lists the file's user presets (read-only), `--dry-run`
   prints the plan, `--restore <backup>` puts a backup back. Tested on synthetic files and, locally, on TE's factory
   `plugPresets.dat` (same layout: 53 records, read back byte for byte).
3. Eject. The OS imports the file (its checksum differs), reloads the presets and restarts. From then on our presets
   are user presets: they survive every restart with nothing connected; the user can overwrite or re-store them on
   the device like any other.

**Rollback:** content mode again, put the backed-up `userPresets.dat` back in `config/`, eject. **Risks:** no
normal-mode write, nothing of TE's is touched (the factory cache is a different file). A malformed file only stops
user presets loading at that start; the next hand-stored preset would then rewrite the file from RAM without the old
ones, which is why the backup and the strict checks come first. A plug that is not installed is skipped at load and
dropped from the file by the OS's next save.

| Route | Persists | Status |
|---|---|---|
| **`userPresets.dat` edited on the content disk** | **Yes**, across restarts, no computer; OS-maintained | Recommended; `opz.py presets bake`; checks H-8, H-9 |
| Store each preset by hand on the device | Yes [proven] | Works today; slow, and stores the current sound (pan, level as played) |
| Boot file `settings/plugs_patch.json` | One restart per write | Built (`presets persist`); for between bakes |
| Re-push after each restart | Until the next restart | Fallback |

## Applying a preset on the device

`PatternContainer::setTrackActivePlug(track, plug)` [code, app `0x3db30`]: switching a track to another plug stores
the old plug's parameters in the pattern's parameter memory, then recalls the new plug's stored parameters; **if the
pattern has none, it applies preset 0**. Choosing the plug that is already active applies preset 0 again. *[manual: the guide describes only the user's side: hold track + black key for the plug, track + lit white key for a preset (p.25).]*

`PatternContainer::applyPreset(track, index, all)` [code, app `0x3cd50`]:

1. `getPreset(track, plug, index)`; if that bit is not set, `index` 0; if that is missing too, TE's default sound for
   the track (`g_defaultPresets`).
2. Writes the 18 values through `Sequencer::parameterChangeEx` (the same path as turning a knob, so connected apps
   receive `$0E`), **except pan** (and on the arp track LFO depth, speed, target, shape) unless `all` is set; level
   is rescaled by the track's relative volume (formula not decoded).
3. Sets the track's octave to `defaultOctave`, clamped to the track's range.

`TrackContext::applyPreset(index)` [code, app `0x386dc`] runs only if the preset bit is set and remembers the index
per track and sound slot. `TrackContext::onNotePressed` calls it from the keys in the track's plug-selection context,
on a short press (< 800 ms): it looks `(note − 53) mod 24` up in a 24-entry table, which maps the white keys F3 … E5
to `index` 0 … 13 and every black key to −1 (no preset) [code, app `0x38e14`–`0x38e48`, table at `0x80258`; this
settles the old H-3 question]; a hold saves (`savePlugPreset` from `fixedUpdate` @ `0x395d0`). **Not on FX
tracks:** they have no user-assignable presets [proven, user on the device], so on fx1, fx2, tape and master only the
preset applied when the plug is chosen (slot 0, index 0) is ever used.

**What the white keys show** [code, app `TrackContext::updateLeds` @ `0x39088`]: while a track is held, each bit of
`getPresetMask(track, plug)` lights its white key **solid** (LED table at `0x801e4`: index *n* → LED 16 + *n*), and
the track's remembered preset index for the active sound slot **blinks** (`0x392cc`–`0x39320`). Choosing a sound slot
(`TrackContext::selectPlug` @ `0x39014`) sets that remembered index to **0 when it is still −1** (`0x39050`–`0x39070`),
so **white key 1 blinks after any slot change, even with no presets at all**. A plug with presets 0–4 shows white keys
1–5 solid (key 1 also blinking). One lit key, and white keys 2–5 doing nothing, means the mask is empty. Choosing a
slot also recalls the sound the pattern stored for that plug (`setTrackActivePlug`); preset 0 is applied only when there
is none, so a `presets watch` after switching back and forth shows the stored sound, which can be TE's default if the
plug was first chosen while it had no presets.

## TE's app

- The C# side (`Assembly-CSharp.dll`) has a `TE.PlugPreset` model (`name`, `slot`, `index`, `default_octave`,
  `values`) that reads TE's catalogue spelling `default_octave` and writes `defaultOctave` into the `patch.json` it
  builds for owned DLC (`PlugManager.SyncOwnedDLC`, TE's `/dlcs` API) [code, app managed]. No preset-browser strings
  were found; the app keeps names for TE's own plugs only, which is why our tools carry the catalogue.
- The native `StateSync` has asymmetric roles: the OP-Z is master, the app a replica [proven, notes]. The `$0E`
  sender runs in `updateSlaveDevice`, i.e. on the master: for each track whose parameters changed it sends
  `[track, 18 values]`, at most every 31 ms [code, app `0x2a75c`–`0x2a7c4`]. So in normal use the OP-Z sends `$0E`
  to the app (16 of them right after a session starts, then on every change); the app does not send it.

## `$0E` in detail

```text
F0 00 20 76 01 0E <pack7(track, p0 … p17)> F7       track 0–15, p = the 18 sound parameters, 8-bit each
```

- **Receive** [code, app `StateSync::processMessage` @ `0x2972c`, case `$0E` @ `0x29854`]: unpacks to exactly 19
  bytes (anything else is dropped), copies the 18 bytes into the **active pattern**'s sound block (pattern offset
  21,056 + 18·track, as in `opzsysex` `set_sound_parameter`) and into the live parameter buffer. `processMessage`
  only runs while a StateSync session is up (a flag in the sync object is checked first).
- **Device** [proven, notes, 2026-07-27]: after the `$00` hello, a `$0E` that changed Lead filter 127 → 255 showed
  up in a fresh pattern-bank read and in the audio; sending the original record restored the bank byte for byte.
- **Session**: `$00` hello (`client id` big-endian, protocol 6) → the OP-Z answers `$01` and, on the first hello of
  a session, sends a state burst that includes one `$0E` per track. The session ends about 5 s after the last hello
  [proven, notes]. So the burst gives the current values to restore after an audition.
  **Not on the user's OP-Z (1.2.45, 2026-10-26):** the burst there has no `$0E` at all, only `$11 $14 $03 $04 $07
  $0C` and the first `$09` bank part [proven; device-tests.md]. The sending side
  never builds a `$0E` [code, app]; the current sounds are in the bank, which the OP-Z sends part by part as the
  client acks them with `$0B` (Q-44, [patterns.md §5](patterns.md#5-how-tes-app-and-our-tools-move-them)). Our
  sessions now read it.
- It changes the stored project (active pattern only) as well as the sound: an audition must put the original
  record back. It does not change the plug, the octave or the preset masks.

## What this means for our tools

- `presets.json`: `slot` = synth track (0 bass, 1 lead, 2 arp, 3 chord). **Effects and drum engines: slot 0 only**
  — presets in slots 1–3 are unreachable on those tracks. **Effects: one preset** (slot 0, index 0), since FX tracks
  have no preset keys; more variations belong in a second plug or in the docs as settings to dial in. Index 0 is the sound a track gets when the plug is first
  chosen. Names: 11 characters. `opz.py` checks all of this (`preset_problems`).
- `opz.py presets push ENGINE_DIR [ENGINE_DIR ...]`: per engine a `patch.json` with only `plugs[]`, repeating the
  installed engine, samples, flags and version, so nothing but the presets changes. Verify = the plug record and slot
  map read back unchanged (presets themselves cannot be read back). Run it again after every OP-Z restart or crash
  (until H-2 says otherwise); several engines in one command restore them all.
- `install`, `presets push` and `remove` then write the boot file `settings/plugs_patch.json` (`presets persist`;
  `--no-boot` skips it): the next start imports it, so the presets come back after one restart without a push
  ([The boot file](#the-boot-file-settingsplugs_patchjson-2026-10-19)).
- `install` and `presets push` end with the white keys that should light **solid** per track (index *n* = white key
  *n*+1; FX tracks none), and `install` says the presets are RAM-only — after an engine update, whose restart drops
  them at once, it says to push them after the restart. Same lines in `@opz/core`.
- `opz.py preset audition|set`: `$0E` inside a StateSync session; the restore point is the track's sound in the
  active pattern of the bank the session reads at its start (or a `$0E` from the OP-Z, as the notes describe).

## What the user guide says [manual]

From TE's manual for 1.2.45 ([manual-notes.md §2](manual-notes.md#2-sound-slots-and-presets), p.25). It does **not** mention
user plugs; this is how a person reaches presets on the device:

- Choose a plug: hold **track** + a **black** (value) key 1…0. Lit black keys are slots with a plug.
- Recall a preset: hold **track** + a **lit white** key. There are 14 white keys and 14 presets per plug, so the
  mapping is white key *n* → `index` *n*−1 (F3 = 0 … E5 = 13); the app's key table confirms it [code, app `0x80258`;
  see [Applying a preset](#applying-a-preset-on-the-device)].
- Store: hold **track** + a white key for **two seconds**. Randomize: **track** + **rec** (also on FX tracks, 1.2.31).
- The white keys light for presets that exist, so after `presets push` the keys show what the plug carries. The manual does
  not say whether FX tracks accept recall or store; "no user-assignable presets on FX tracks" stays as the user's finding.
- User-stored presets are the manual's own way to keep a tuned sound; whether one stored on our plug survives a restart is
  the new check **H-2b**.

## Hardware checks

For the main session, with the user approving each write, TE's app closed, a test plug installed (60901
`opz_test_sine` or any of ours) and its presets in `presets.json`. Each step names what to watch and what would
contradict this page.

| # | Step | Expect | If not |
|---|---|---|---|
| H-1 | `opz.py presets push engines/<name>` (after `install`); start `opz.py preset watch --track <t>` in a second terminal **after** the push (one client at a time); on the device choose another sound on the plug's track and back to the plug, in a pattern that has not used it (e.g. a cleared one; in a used pattern, switching back recalls the sound the pattern stored for the plug, not preset 0) | `watch` prints preset 0's 18 values for the track; the octave follows `defaultOctave` | `plugs[]` import is not honoured on the device: fall back to a version bump + `install` |
| H-2 | Restart the OP-Z, then repeat the sound change of H-1 without pushing | TE's default sound (presets gone) | presets do persist: drop the "push again after restart" advice |
| H-3 | On a synth or drum track: hold the track button and tap keys; then hold a key for a second. **Gesture [manual]:** hold **track** + a **lit white** key = recall; hold track + a white key for **2 s** = store; the black keys are slots, not presets | the first *N* white keys are lit **solid** for *N* presets (key 1 also blinks after a slot change) and recall presets 0…*N*−1 in key order; a hold stores a user preset (FX tracks have neither). Key mapping now settled by code (white key *n* = index *n*−1) | only white key 1 lit, blinking: no presets in RAM (see H-6) |
| H-6 | **Do presets survive the runtime import?** No version bump, no restart. `opz.py presets push engines/table --dry-run`, then the same without `--dry-run`; **straight after** (no restart, no crash), hold **bass** with table's slot chosen and count the **solid** white keys; repeat on lead, arp, chord. Then start `opz.py presets watch --track bass`, hold bass and tap white key 2 briefly | white keys 1–5 solid on all four tracks; the watch prints `reso bass` (P1 60, P2 128, decay 50, sustain 100, release 25; pan and level follow the track) | 5 keys: the import works, so the 2026-10-19 failure was a restart between install and test (H-2). Only key 1, blinking: the runtime import drops presets in a way static reading did not find; next, check that the push log shows `settings/patch.json` requested, then try `settings/plugs_patch.json` (imported at boot) |
| H-7 | **Does the boot file bring presets back after a restart?** Full steps and rollback below | after the restart, without a push: white keys 1–5 **solid** for `table` on bass, lead, arp and chord | only key 1, blinking: the boot import did not bring them; check that the write's sync log listed `settings/plugs_patch.json` as requested; fall back to `presets persist` after each restart |
| H-2b | Store a preset on our plug (track + white key, 2 s), restart, hold track and look at the white keys | the stored key is still lit (user presets persist via `config/userPresets.dat`) | **passed 2026-10-19** (user): a hand-stored preset on our plug survives a restart |
| H-8 | After H-2b's store: content mode, look in the disk's `config/` (read only, change nothing), eject | `userPresets.dat` is there, 351,202 bytes | **passed 2026-10-19** (user, `presets bake --show`): 351,202 bytes, every strict check passed, 76 plug records; hand-stored presets on plug 10 and on table (bass keys 1 and 4), all with empty names |
| H-9 | Bake `table` into `userPresets.dat` in content mode, eject, restart with nothing connected (steps below) | white keys 1–5 lit **solid** on table's tracks after every restart; the hand-stored one still there | **passed 2026-10-19** (user): `--overwrite-user` bake (18 new, 2 replaced), eject; white keys 1–5 solid and still there after restarts, nothing connected. Those presets still had LFO depth 0 (the branch predated the fix): re-bake updates them |
| H-9b | On an OP-Z with **no** `config/userPresets.dat` (never a hand-stored preset, e.g. after a format): the site's loader makes the file in content mode (an empty store, version 1, 200 records, with only our presets and their user bits), eject (steps in `OP-FREE/site/src/loader/README.md`) | the OP-Z imports it (the config callback takes `userPresets.dat` even when the device has none [code, device `0x80034d68`]): our presets on solid white keys after every restart; the next content mode exports the OP-Z's own copy | not run; the loader's create path is offline-tested only (2026-11-05). Why it matters: friends without hand-stored presets hit "userPresets.dat not found" |
| H-4 | `opz.py presets audition engines/macro --track lead --slot 1 --index 0` (macro selected on lead) | sound changes while held; after Enter the original values come back | if the restore record is missing the tool refuses to write (no burst seen) |
| H-5 | After H-4, `opz.py backup` and a project save/reload on the device | project's lead sound unchanged | `$0E` restore failed: re-run `preset set` with the logged original values |

`preset watch` is read-only: it relies on the OP-Z sending `$0E` whenever a track's parameters change, which is how
`applyPreset` reaches connected apps (`parameterChangeEx` → `PatternContainer::setTrackParameter` → `StateSync::addParameterChange` [code, app `0x4dcb8`, `0x3db2c`]). If it prints nothing on a knob turn
either, the state session is not up and the other checks cannot be judged from the terminal: use your ears.

### H-7 in detail: boot file, one restart

For the main session, with the user approving each write; TE's app closed. **Not with macro**: macro crashes the
OP-Z when its lead preset 2 is chosen, so the check uses `table` (61016, installed, 20 presets: 5 per synth track).
The boot file holds `table` only, so the boot import runs on the smallest file (one entry, the same as the H-6 push).

1. Dry run (no write): `.venv/bin/python tools/opz.py presets persist engines/table --dry-run`. Expect
   `plan: boot file settings/plugs_patch.json (… bytes), 1 plug (presets): table 61016 (20)` and the file's JSON:
   `{"plugs":[{"engine":"engines/table.engine","flags":0,"id":61016,"presets":[…],"samples":"","version":1}]}`
   (values as in the device's `plugs.json`).
2. Write (user approves): `.venv/bin/python tools/opz.py presets persist engines/table`. Expect the sync log to list
   `settings/plugs_patch.json` (it is imported at once: a presets push of `table`) and `boot file written and
   verified`. **Right after, no restart:** hold bass with table's slot chosen: white keys 1–5 solid (the runtime
   import, as in H-6). If the OP-Z crashes here, stop: see rollback.
3. The user switches the OP-Z **off and on** (or restarts it). No command, no push.
4. Hold **bass** with table's slot chosen and count the **solid** white keys; repeat on **lead, arp, chord** (choose
   table's slot on each first if needed). Expect **1–5 solid on all four** (key 1 may also blink).
5. Optional, read-only: `.venv/bin/python tools/opz.py presets watch --track bass --seconds 60`, tap white key 2 →
   `reso bass` (P1 60, P2 128, decay 50, sustain 100, release 25).
6. Optional second restart: expect **only key 1, blinking** — the start in step 3 deleted the file. This confirms
   the one-restart limit; `opz.py presets persist` re-arms it.
7. Only after 4 passed: `.venv/bin/python tools/opz.py presets persist --dry-run` lists every installed plug of ours
   with presets (about 25 KB); writing it is the normal state from then on.

**Rollback.** Nothing of TE's is touched; the plug records and slot map are verified unchanged (backup
`backups/<time>-boot-presets/` holds them and the boot file written). To disarm before a restart:
`.venv/bin/python tools/opz.py presets persist --off` (writes `{"plugs":[]}`; the next start imports nothing and
deletes it). After a restart there is nothing to undo: the start deleted the file. If the OP-Z crashed at step 2 or
does not finish booting after step 3 (a crash inside the boot import repeats at every start, because the delete
comes after it): power-cycle once and, if it boots, run `presets persist --off` at once; if it keeps crashing, boot
to content mode (hold TRACK) and check whether `settings/` is reachable to delete `plugs_patch.json`, else reinstall
the official 1.2.45 firmware with TE's installer in update mode (SAFETY.md) [inferred: its content carries TE's
`settings/plugs_patch.json`, which replaces ours]. Not expected: the runtime import of the same bytes in step 2
runs the same function first.

### H-9 in detail: presets that stay (content mode)

TE's app closed; nothing else to connect. Uses `table` only (not macro: its lead preset 2 crashes the OP-Z, and a
baked preset stays until it is overwritten).

1. On the OP-Z: at least one preset stored by hand already (H-2b did this; else hold a track + a white key 2 s).
2. Content mode: switch off, hold **track** while switching on (all track LEDs green); the Mac mounts the disk.
3. Read only: `.venv/bin/python tools/opz.py presets bake --disk /Volumes/OP-Z --show` → `… plug records, …
   free; … with user presets` and the hand-stored one listed (H-8: the file is there). If the volume has another name,
   use it.
4. Dry run: `.venv/bin/python tools/opz.py presets bake engines/table --disk /Volumes/OP-Z --dry-run` →
   `backup: backups/<time>-userpresets`, `plan: table: 20 new, 0 replaced, 0 already there` (or a `kept your
   stored …` part if a hand-stored preset sits on one of table's keys), `plan: 1 of 200 records change`.
5. Write (user approves): the same command without `--dry-run` → `written and read back`.
6. Eject the disk in Finder (or press play on the OP-Z). The OP-Z imports, then restarts.
7. With nothing connected: choose table on bass, hold **bass** → white keys **1–5 solid**; the same on lead, arp,
   chord. Switch the OP-Z off and on twice; check again: still lit.
8. Optional: hold bass, tap white key 2 → "reso bass" plays (a user preset now; it can be re-stored like any other).

**Rollback:** content mode again, then `.venv/bin/python tools/opz.py presets bake --disk /Volumes/OP-Z
--restore backups/<time>-userpresets`, eject.

**Updating baked presets** (a new `presets.json`): bake again. A user preset at one of our keys that carries our
preset's name is ours from an earlier bake and is updated (`… updated (from an earlier bake)`); presets stored on the
device have no name and are kept unless `--overwrite-user`. Run from the main checkout or this worktree
(`.venv` must exist); `--disk` finds the mounted OP-Z disk if the path given is not there. The tool removes the
`config/._userPresets.dat` macOS writes next to the file (file attributes on a FAT disk), which the OP-Z would
otherwise move to `rejected/`. **If step 7 shows no presets:** content mode, `--show` to see whether
the file on the disk carries table's 20 user presets (the device exports its own copy, so this shows what it kept);
look for `rejected/userPresets.dat` and the disk's `import.log`.
## Hardware results (2026-10-19)

- **H-6 passed** [proven, user]: right after `opz.py presets push engines/table` (no restart), holding bass on table's
  slot showed **4 solid white keys and the selected one blinking** (5 presets), and white key 2 recalled "reso bass"
  (`presets watch` reported P1 60, P2 128, decay 50, sustain 100, release 25). The earlier "one lit key" was the
  slot-change blink on an empty preset mask: the presets from the install three hours earlier were gone, most
  likely because the OP-Z restarted in between. Rule: **push our presets again after every restart**.
- **LFO depth is bipolar, 128 = off** [proven, user]: every preset we shipped had LFO depth 0 (and speed, target,
  shape 0), which is full negative depth, so the presets sounded stuttery until the depth dial was centred. TE's
  default sound uses depth 128, speed 100, target 255, shape 0; all our presets now use that for "no LFO", and
  `opz.py presets check` (and core's `presetProblems`) warn on depth 0.
