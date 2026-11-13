# Projects and patterns: format, transport, safe writing (2026-10-23)

What the OP-Z stores per project and pattern, how TE's app and our tools can read and write it, what a bad file does,
and how to write patterns without risking the user's projects. For F-05 (generative patterns), F-06 (MIDI ↔ pattern),
F-07 (preset morphing) and F-18 (64-step pages).

Evidence: **[code, device]** = `opz.ldr` 1.2.45 in Ghidra (headless, the `p0-memory-content` project); **[code, app]** =
the arm64 slice of TE's desktop `SnakePlugin` (same sequencer source, with symbols); **[notes]** =
[opz-firmware-notes](https://github.com/ioma8/opz-firmware-notes) `refs/op-z-sysex/docs/` (device-proven on 1.2.45 in
July 2026 by that project, not by us); **[data]** = the user's own content-mode backup of ten project files (2023,
read only, not in git) and TE's `how_to_import.txt`; **[manual]** = TE's guide via [manual-notes.md](manual-notes.md).
Nothing in this document was run on the OP-Z by us.

## 1. Answers in short

| Question | Answer | Evidence |
|---|---|---|
| Where do projects live? | Ten files `projects/project01.opz` … `project10.opz` under `/yaffs2/user/`. **All ten are held in RAM at once** (loaded at boot), so switching projects does not read the file | [code, device] `0x80111934`, `0x801126e4` |
| File format | `u32 major (73)`, 568-byte global block, 16 × 21,392-byte patterns, `u32 minor (7)` = 342,848 bytes; versions 71/72 have no minor word (342,844). No checksum, no magic, no length field | [code, app + device]; [data] all ten user files |
| Pattern layout | Tracks 16 × 12, notes 880 × 8, steps 256 × 54, sounds 16 × 18, mute groups, tape/master masks (§3) | [code, app]; [notes, proven]; [data] |
| Read a project live (normal mode)? | **Yes**: the file service serves projects as download ids **3–12** (= project 1–10). It returns the last *saved* file | [code, device] `0x8012bde8`; [notes] |
| Write a project live? | **Yes, by file-service write**: a completed sync job renames the file into place and, when the path contains `projects/project`, **reloads it into that RAM slot at once** | [code, device] `0x80127a24`; [notes, proven] per 03-content-and-delivery.md |
| Write patterns live (active project)? | **Yes, device-proven by the notes**: StateSync `$08` reads the active project's bank, `$09`/`$0A` (with `$0B` acks) replace patterns 1…N, `$0C` the global block, `$0E` one track's sound | [notes, proven]; [code, device] `0x8011445c`, `0x801141a8` |
| Does the OS validate a project? | **Almost not.** Version major 70–73 and nothing else; step count/length are repaired; every other byte is taken as is (§6) | [code, device] |
| Live pattern writing feasible for the web app? | **Yes.** Recommended route: whole-file write to a spare project over the file service (atomic on the device, persistent, works on any of the ten, reuses our proven file service), with backup, checks, read-back and rollback (§7). The `$09` route is for live edits of the open project later | this document |

## 2. In RAM and on disk

**RAM** [code, device]: the sequencer object at `[0x81059d1c]` holds, at `+0xb90`, the pattern banks of all ten
projects (10 × 342,272 bytes), at `+0x344590` the current project index (`int`), at `+0x344594` the ten global blocks
(10 × 568). `loadAllProjects` (`0x801126e4`) loads all ten at boot; saving project *n* writes slot *n*
(`0x801118b0`/`0x80111a14`: "trying to save illegal project" is logged for *n* > 9 but the save still runs).

**Saving** [code, device, main loop `0x80129c24`]: every pattern setter marks its project dirty (a byte per project at
`0x81125f9c`, set from the pattern index / 16, as `PatternContainer::set*` in the app at `0x3d5dc`). The main loop
saves every dirty project about **2 s** after the last change (when two enable bytes `0x81059d06`/`0x81059d05` are
set; their meaning is open, likely autosave on and "not busy"), saves the current project on request (byte
`0x8101831b`, most likely set by the hidden `debug-save` command, `0x80040d1a` [inferred]), and writes
`session.dat` 10 s after a change. `saveProject` (`0x801117bc`) writes `<path>.tmp` (header, global, bank, minor),
closes it, renames it over the old file and syncs yaffs: a save is atomic against power loss [code; inferred for
yaffs rename]. The `$13` autosave flag and manual-save mode are runtime-global [notes, proven; manual p.32].

**Versions** [code, device `0x80111f68`; app `loadPatternData` `0x7a1f4`]: the version word is `major << 16 | minor`.
`loadProject` (`0x80112508`) reads major, the global block (`0x801116a0`), the bank (only for majors 70–73), the minor,
then runs an upgrade ladder one step at a time until `0x490007`: 70 → 71 → 72 → 73.0 → … → 73.7 (FX parameter
remaps, note pitch +5 for some tracks, micro-timing × 8, insert-FX dry level from plug flag 1, note-style remap,
…). An **unknown version is treated as current** (copied unchanged). Our writer only writes 73.7.

**The user's files** [data]: six are 73.7 (342,848 bytes, header `49 00 00 00`, trailer `07 00 00 00`), four are 71.0
or 72.0 (342,844 bytes, no trailer). All ten round-trip byte for byte through `tools/opz_project.py`; the six 73.7 files
pass its checks.

## 3. Pattern layout (21,392 bytes; all little-endian)

| Offset | Size | Contents | Evidence |
|---:|---:|---|---|
| 0 | 16 × 12 | Track records (below) | [code, app] setters `0x3d55c` … |
| 192 | 880 × 8 | Note records: per step 55 slots, track offsets 0,2,4,6,8,12,16,24,28,29,30,31,35,41,47,51 (slots 2,2,2,2,4,4,8,4,1,1,1,4,6,6,4,4) | [notes, proven]; app table just before `0x838a8` |
| 7,232 | 256 × 54 | Step records, track-major: `7232 + (track × 16 + step) × 54` | [notes, proven] |
| 21,056 | 16 × 18 | Sound: the 18 parameters per track (P1, P2, attack, decay, sustain, release, FX1, FX2, filter, resonance, pan, level, portamento, LFO depth, speed, value/target, shape, note style). `$0E` writes this for the active pattern | [notes, proven]; presets.md |
| 21,344 | 10 × 4 | Mute groups, 2 bits per track (upper bit = mute) | [code, app] `setTrackMute` `0x3d3ac` |
| 21,384 | 2 | Tape routing mask (bit = track) | [notes, proven] |
| 21,386 | 2 | u16. Low byte = **master routing mask** (bit = track). **Bit `0x0800` = the pattern is in use**: clear it and the OP-Z ignores the pattern and leaves its key dark, whatever notes it holds (§9). Bit `0x1000` seen once in the user's files, meaning unknown (Q-90) | [notes, proven] for the routing bits; [proven] (§9) for `0x0800`; [data] for the values seen |
| 21,388 | 1 | Active mute group | [code, app] `setMutePreset` `0x3d440` |
| 21,389 | 3 | Unknown (0 in all user files) | [data] |

**Track record (12 bytes)** [code, app setters; values from data]: `+0` active plug id (u32), `+4` **step count**
1–16, `+5` "timing" (`setTrackTiming`; 0/3/5/9/16 seen; probably the FX/LFO timing note value of Q-18 [inferred]),
`+6` **step length** (raw 1, 2, 4, 8 seen; the manual's key 1…0, where 9 = 16× and 0 = trig driven [inferred
mapping]), `+7` quantize, `+8` a byte the 73.4 upgrade remaps 0/1/2 → 0/2/3 (0 in every user file), `+9` note length
(64 default), `+10` last note played (runtime), `+11` unknown (0).

**Note record (8 bytes)**: `i32 duration` (6,200 per step; the OS itself stores −256 for some notes, meaning open
[data]), `note` (FF = unused slot), `velocity`, `i8 micro` (raw = ticks × 8, −96…+88 = −12…+11 ticks of 24 per step),
`age`. [notes, proven; data]

**Step record (54 bytes)**: `u16` component mask, 16 component values, 18 lock values, 18 lock enables (FF = on).
Components 0–13: pulse, parameter spark, jump, velocity, ramp up, ramp down, random, glide, multiply, sweep, pulse
hold, tonality, trigger spark, component spark; bits 14–15 reserved. Values 1–10 = keys 1–0; sparks also use high
nibble modes (`0x12`, `0x24`, … seen in user files). [notes, proven; data]

**Defaults** (`PatternData::PatternData`, app `0x3eda0`): no plug, 16 steps, timing 5, step length 1, note length 64,
last note 60; every note slot unused with duration 2,560 and velocity 100; component values
4,2,4,5,4,4,4,4,2,2,4,4,2,2 (table app `0x838a8`); no locks; sounds 0; tape mask `00FF`, master mask `00F0` — **the in-use bit is clear**, which is why a pattern the
device has never used stays dark until something sets it (§9).

## 4. Global block (568 bytes)

[notes, proven; code, app `GlobalData::GlobalData` `0x4784`]: `0` 16 chain records × 32 (15 saved + the live chain;
entries are zero-based patterns up to the first FF; the firmware plays at most 31), `512` levels drum, synth, punch,
master, `516` tempo u16 40–200, `518–560` opaque (keep), `561` swing, `562` metronome level, `563` metronome sound,
`564` selected chain (FF none), `565–567` opaque. The constructor fills only 16 of each chain record's 32 bytes with
FF, so bytes after the terminator are leftovers [code; data: non-FF bytes there in user files]. **[manual p.12, p.30]**
14 saved chains and 32 patterns per chain; the code has 15 slots and plays 31 (Q-42).

## 5. How TE's app and our tools move them

| Route | Mode | What | Where it lands | Evidence |
|---|---|---|---|---|
| File-service download id 3–12 | normal | one whole project file | — (read) | [code, device] `0x8012bde8`; `opz.py projects backup` |
| File-service write `projects/projectNN.opz` | normal | one whole file | yaffs file (CRC-checked, renamed into place), then `loadProject` into RAM slot NN−1, then two refresh calls (`0x80108644`, `0x801091ac`) | [code, device] `0x80127a24`–`0x801282b6`; `opz.py projects put` |
| StateSync burst / `$08` → `$09`…`$0A`, `$0B` acks | normal, app session | the **active** project's bank (address = project × 16 + active pattern): part 0 comes with the session's first hello, each further part after the client's `$0B`; `$08` starts it over | — (read) | [proven, part 0 on 1.2.45]; [code, app] below; [notes] |
| StateSync `$09`/`$0A` upload, `$0B` acks | normal, app session | patterns 1…N of project `address >> 4` (a zlib member of N × 21,392 bytes) | RAM only (no dirty flag set by the receiver) | [notes, proven]; [code, device] `0x8011445c`, `0x801141a8` |
| StateSync `$0C` | normal | the active project's global block | RAM | [notes, proven] |
| StateSync `$0E` | normal | one track's 18 sound bytes, active pattern | RAM, saved with the project | presets.md |
| StateSync `$07` | normal | live chain, active pattern, project (selects them) | RAM | [notes, proven] |
| StateSync `$02` deltas | — | the OP-Z sends them to the app on every edit (`StateSync::addPatternChange`); the device ignores incoming ones (replica-only) | — | [code, app]; [notes, proven] |
| Content mode `projects/` | disk mode | the ten files; others rejected; a removed file becomes an empty project | yaffs on eject | [data] TE's `how_to_import.txt`; [code] Q-10 |

TE's app is a StateSync replica: it gets the bank at the start of the session (or again with `$08`), keeps it in sync
from `$02` deltas, and pushes whole banks with `$09`/`$0A` (`StateSync::receiveFullPatternPart`/`applyFullPattern` are
the same code on both sides).

**Reading the bank (Q-44, 2026-10-26)** [code, app: TE's desktop `SnakePlugin`, arm64, symbols; the OP-Z's side of
StateSync is the same class]: the sending side keeps one transfer and one connected client.

- **Start.** `updateSlaveDevice` (`0x2a1e8`) sends a full bank when the client's change cursor is −1 (a new session)
  or more than 20 changes behind (`0x2a224`–`0x2a248`). `sendFullPattern` (`0x2b150`) queues the compression of the
  **active project's 342,272-byte bank** (source `patterns + (address & 0xFFF0) × 21,392`, length from the literal
  at `0x7ecd0`) and takes the address from the sequencer's current pattern index (`[seq + 68]`, project × 16 +
  pattern; the `$0E` receiver writes the same pattern, `0x29894`). Part 0 goes out when the compression is done
  (`0x29584`).
- **Parts.** `sendFullPatternPart` (`0x2a050`): header `u16 address, u16 transfer id, u16 part index`, then at most
  178 compressed bytes; `$0A` when it is the last, else `$09`. The part index then counts up; after the last it is 0.
- **Acks gate the stream.** The next part is sent only when the ack flag is set (`0x2a260`–`0x2a278`), and only the
  `$0B` handler (`0x29bdc`) sets it: an 8-byte payload whose `u16` at 4 is the transfer id and `u16` at 6 the index
  of the part just sent (bytes 0–3 are not read). TE's receiver (`0x2b67c`) sends exactly that after every non-final
  part: `09 00 00 00`, transfer id, part index (`0x2b7d4`–`0x2b804`). Nothing times out: without an ack the transfer
  waits for ever. This is why the user's OP-Z sent part 0 and nothing more [proven, 2026-10-26: header `82 00 00 00
  00 00` = project 9, pattern 3, transfer 0, part 0].
- **`$08`** (`0x29bd0`) sets the client's change cursor to −1, whatever the payload: the next update sends the whole
  bank again, from a new compression. TE's receiver sends `$08 FF FF FF FF` when a part is out of order or the
  wrong size. Neither `$0B` nor `$08` changes pattern data; they only move the sender's cursor.
- **`$07`** (play state, 20 bytes, `0x2a42c`–`0x2a45c`): byte 17 = the same current pattern index as the bank's
  address (low byte), byte 19 = the current project. Bytes 0–15 are the live chain, 16 and 18 not traced.
- **The OP-Z sends no `$0E`.** Nothing in the sending side builds a `$0E`; the device only receives it. The notes
  repo's "one `$0E` per track in the burst" does not match this code or the user's unit [proven].

So a client reads each track's current sound by acking the parts as they come, inflating the bank and taking the
18 bytes at `(address & 15) × 21,392 + 21,056 + 18 × track`. `opz_session.StateSession` and `@opz/core` do this
(restore points for `presets audition`/`set` and `opz.py test`); `fake_opz.py` sends the bank the same way. The
device's own copy of the sending side (`opz.ldr`) was not decompiled for this [inferred: same source, as the
receiver matched at `0x8011445c`]; the read-only check in device-tests.md
confirms it.

**Receiver details that matter** [code, device `0x8011445c`/`0x801141a8`; app `0x2b67c`/`0x2ba58`]: a non-final part
must decode to exactly 184 bytes (6 header + 178), the final one to at most 184; the transfer id must stay the same
and the part index must count up from 0, else `$08 FF FF FF FF`. Non-final parts are stored at `(index × 178) &
0xFFFF`, so **a compressed bank over 65,504 bytes wraps onto its own start**. The destination is `patterns +
(address >> 4) × 342,272` with **no bounds check**: an address ≥ `0xA0` writes past the ten projects (into the
current-project index, the global blocks and beyond). zlib inflates straight into the project, so a stream that
fails half-way leaves a half-written bank ("failed to uncompress").

## 6. Malformed files: what the OS does

All [code, device] unless marked; for the file-service write path and for boot (`loadAllProjects`).

| Bad input | Result |
|---|---|
| Path `projects/project00.opz`, `project11.opz` … | The index after `projects/project` is parsed with `%02d.opz` and used as `index − 1` **without a bounds check** (`0x8012824a`): the load writes 568 + 342,272 bytes before or after the ten projects. **RAM corruption, likely a crash.** Our tools only produce 01–10 |
| Unknown major version | `loadGlobalData` logs "Loading old project format, default global data", **resets the live global block** (tempo, chains, levels) to defaults and the load returns: patterns stay as they were in RAM, the bad file stays on disk; at the next boot the same happens and the RAM patterns are the constructor's, so the project is effectively lost once saved |
| Major 70–73 with a wrong minor | The ladder runs from that minor; an unknown minor is copied as current |
| Truncated file | `fread` results are not checked: the missing tail keeps whatever the load buffer `0x8105a4e8` held — **the previously loaded project's bytes** [inferred from the code path] |
| Step count 0, step length 0 or > 10 | repaired to 16 and 1 after every load |
| Anything else out of range (note > 127, lock enables, plug ids that are not installed, components, chain entries > 15, tempo) | taken as is; effects unknown (an unknown plug id falls back somehow; a note or chain index out of range may index tables) [inferred] |
| `$09` bank stream that fails to inflate | half-written bank in RAM, `$08 FF FF FF FF` or silence |

So **the OP-Z does not protect itself or the user's projects**: our side must make every file one the OS could have
written. `opz_project.Project.problems()` checks exactly that (version 73.7, step count/length, notes, micro, lock
enables, reserved component bits, mute group, chains, tempo, selected chain; durations are not checked because the OS
writes −256 itself). It also warns when a pattern holds notes but lacks the in-use bit (§9) — not a file the OS would
refuse, but one whose patterns the user will never see.

## 7. Safe writing: design

**Rules** (all enforced by `tools/opz_project.py` / `opz.py projects`; to be mirrored in the web app):

1. **Back up first**: read all ten projects (`opz.py projects backup`, ~1,715 file parts each) and keep the files with
   their hashes. The backup is the rollback.
2. **Write only a spare project**: one whose backup has no notes in any pattern, read while another project is open
   (the open project's file keeps changing; H-P1). Anything else needs `--overwrite`
   and the user's say-so. Never the project open on the device: the reload discards its unsaved edits and the
   effect on playback is unknown (Q-39).
3. **Only project numbers 1–10** (`project_path`, `project_file_id`; `$09` project index 0–9).
4. **Only checked files**: start from the device's own file (or `Project()` defaults), change bytes through the typed
   setters, require `problems() == []`, version 73.7.
   **Every pattern you generate must carry the in-use bit** (§9) or the OP-Z will ignore it: the content setters
   mark it, `set_pattern_in_use(p)` sets it by hand, and `problems()` warns when notes are there without it.
5. **Verify by a fresh read** of the file (id 3–12) and compare byte for byte; on any difference write the backup
   back and verify that.
6. **One client**: TE's app closed (SAFETY.md rule 8); the device idle (no edits on the device during the write).
7. **No content mode for this.** It is not needed (normal mode reads and writes projects), and entering it deletes
   user samples that sit in no slot (SAFETY.md rule 5). If content mode is ever used for projects, first check that
   every user sample is in a slot (`opz.py samples list`).
8. For `$09` later: compressed size ≤ 65,504 bytes (`bank_upload_frames` refuses more), whole patterns only, project
   from the device's own `$09` address, read back with a fresh `$08` after the old session lapses (6.25 s, [notes]),
   restore the pre-edit bank on any mismatch, and `debug-save` or an on-device change to persist [notes, proven].

**Why the file route first**: one atomic write on the device (temp file, CRC, rename), persistent by construction
(the file *is* the saved project), works on a project that is not open, carries the chains and tempo too, and uses
the file service our install path already proved on hardware. Its costs: ~343 KB per write (about 1,700 parts each
way; seconds to tens of seconds over USB [inferred]) and a reload of the whole project.

**Tools now** (offline-tested against `tools/fake_opz.py`, which serves ids 3–12 and models the reload, including
the index bug):

```bash
python3 tools/opz.py projects backup
```

```bash
python3 tools/opz.py projects put 8 song.opz --dry-run
```

`tools/opz_project.py` (`info`, `check`, `diff`, `empty`, `vectors`) and its library: `Project.parse/to_bytes`, typed
reads (`track`, `notes`, `step`, `sound`, `muted`, `routing`, `pattern_in_use`, `chain`, `summary`), setters (`set_notes`,
`set_component`, `set_lock`, `set_sound`, `set_track`, `set_muted`, `clear_track`, `copy_pattern`, `set_chain`,
`set_tempo`, `set_pattern_in_use`), generators (`euclid`, `write_rhythm`, `write_pages`) and `bank_upload_frames`. `@opz/core` has the same
format code as `project.*` (shared vectors `test-vectors/project.json`); the web transport for projects is still to do
(its zlib port compresses at most 16 KB, so pulls work but a 343 KB push needs a bigger deflate or stored blocks).

## 8. Feasibility of the features

| Feature | Feasible? | How | Limits |
|---|---|---|---|
| **F-05 generative patterns** | **Yes** | Generate notes per track into a spare project (`write_rhythm`, `euclid`; Grids/Turing are host-side math over the same setters), write the file | No step probability field exists: randomness is baked in per write (or via the OS's own random/spark components) |
| **F-06 MIDI ↔ pattern** | **Yes** | One step = a sixteenth × step length; duration in 6,200ths of a step; micro-timing ±12 of 24 ticks; up to 2/4/8 notes per step per track (drums 2, bass/lead/chord 4, arp 8); drum keys MIDI 53–76 (Q-14). Export reads the same records | 16 steps per pattern; finer than 1/24 step and longer than 16 steps need several patterns; note style/arp settings change playback |
| **F-18 64-step pages** | **Yes, with the known limit** | `write_pages`: four consecutive patterns + a saved chain; every page starts as a copy of the first, so the other tracks repeat | All tracks switch pattern together; 31 entries per chain (Q-42); the device shows one page at a time |
| **F-07 preset morphing** | Yes (unchanged) | `$0E` stream | writes the active pattern's sound; restore at the end |

## 9. Hardware check H-P1 (run 2026-10-26: passed)

Goal: prove the read (ids 3–12) and the write + reload route on the user's OP-Z with nothing at risk.

| Step | Do | Expect | If not |
|---|---|---|---|
| 0 | TE's app closed; OP-Z in normal mode; open project 9 (the test project) on the device and stop playback; `opz.py samples list` shows no user sample in NO SLOT (not needed for normal mode, but keeps the rule) | — | — |
| 1 | `opz.py projects backup` | ten files, each 342,848 bytes (or 342,844 for old ones), "version 73.7" for recently saved ones; note which projects have no notes | not served: stop (Q-38 no); keep using content-mode backups |
| 2 | Pick a **spare** project *S* (no notes, not 9, not the open one). Build the test file from *S*'s own backup: `opz_project.py rhythm backups/<time>-projects/projects/projectSS.opz test.opz --pattern 1 --track kick --hits 4` (a kick on steps 1, 5, 9, 13, note 53; plugs and sounds stay *S*'s) | it prints `x...x...x...x...` and 0 problems; `opz_project.py check test.opz` says ok | fix the file |
| 3 | `opz.py projects put S test.opz --dry-run` | plan line, nothing written | — |
| 4 | `opz.py projects put S test.opz` (confirm) | "written and verified" | the tool restores the backup by itself; report the log |
| 5 | On the device: switch to project *S*, play | the kick on steps 1, 5, 9, 13; switch back to 9 | reload did not happen: note it (Q-38), restore with step 6 |
| 6 | Rollback: `opz.py projects put S backups/<time>-put-projectSS/projects/projectSS.opz --overwrite` | "written and verified"; project *S* is as before | report |
| 7 | `opz.py projects backup` again and compare with step 1's files (`opz_project.py diff`) | identical except nothing | report the differing fields |

Keep the verbose log on. Change nothing else on the device during the test. A crash at step 4 or 5 would point to the
reload path: power-cycle, then do step 6.

### Result (2026-10-26, the user's OP-Z, firmware 1.2.45)

- **No spare project existed**: all ten projects have notes. With the user's say-so, project 9 (their test project)
  was used with `--overwrite`, the kick written into its empty pattern 3; the OP-Z was on project 10 during the write.
- **Step 1 (read)** [proven]: all ten projects served as ids 3–12 in normal mode, 342,848 bytes each, version 73.7,
  1,715 parts per file; read six times in the session (`backups/20261026T170542988907Z-projects` and later).
- **An open project changes on disk** [proven]: project 9's file differed between reads 6 minutes apart while it was
  the open project (85 bytes in patterns 3–5: chord, lead, arp, fx2 track/sound bytes; no notes), and stayed
  byte-identical over three reads once project 10 was open. So the OS saves the open project as it is used
  [inferred: autosave or save on edit]; build a write from a read taken **after** switching away (added to rule 2).
- **Step 4 (write)** [proven]: `opz.py projects put 9 build/hp1/test09.opz --overwrite` → "written and verified"
  (sha256 `8ca5afc4…`, about 4.5 min including the two backups); rollback copy
  `backups/20261026T173033240039Z-put-project09` (= the stable project 9, `9589d686…`).
- **Step 5 (reload)** [proven, user by ear]: switching to project 9, pattern 3 played the kick on steps 1, 5, 9 and 13;
  patterns 1 and 2 unchanged. No crash. **Q-38 answered: yes.**
- A read after the listening check differed from the written file by one byte: pattern 3 kick resonance 0 → 2
  (`backups/20261026T173303998611Z-projects`), most likely a knob touched while playing [inferred].
- **Steps 6–7 skipped by the user's choice**: the test kick stays in project 9 (pattern 3). The rollback file above
  can still restore project 9 as it was before the test.

### Follow-up (2026-11-01, H-C1 set-up): written patterns that never appear

For H-C1, patterns 4–16 of project 9 were generated (`opz_project` setters: plug 60907 `opz_test_load` on all eight
instrument tracks, one long note per track on step 1, neutral sounds) and written three times with `projects put`.
Each write was "written and verified" by a byte-for-byte read-back [proven]. **The OP-Z never showed them**: the
patterns stayed dark (the pattern key's "has notes" light), opened patterns were empty, and a power cycle did not
change that [proven, user].

| Write | Device state at the write | Notes | Result |
|---|---|---|---|
| 1 (14:50 UTC) | project 9 possibly open (the user is not sure) | 16 steps (99,200 units) | dark |
| 2 (15:29) | project 10 open (confirmed) | 99,200 units | dark |
| 3 (15:50) | project 10 open (confirmed) | 98,303 units (the longest any of the user's own projects holds, `0x17FFF`) | dark |

Also seen [proven, fresh reads after each step]:

- **The OP-Z saves the patterns the user opens, from its memory, over the file**: each pattern opened on the device
  (4–8, 10) came back in the next read as the device's own (empty) pattern, with its own defaults on every track
  (fx1 … motion too), and the live chain record changed to the pattern last opened. Patterns not opened kept the
  written bytes. So the device's copy of project 9 is not the file, and leaving or using the project writes parts of
  it back [inferred: a per-pattern save of visited patterns].
- **The note length is not the cause** (write 3). *(At the time we also wrote here that no byte marks a pattern
  "in use". That was wrong — see "What it really was" below; one byte does, and our search for it had missed it.)*
- Still unexplained, against H-P1 (a one-step kick on TE's plug 130 in pattern 3, which played at once): either the
  written file is not loaded into the device's copy at all (H-P1's success then needs another explanation), or
  patterns naming plug 60907 on every track are dropped on load. Open as **Q-50**. A differential test would write
  H-P1's kick rhythm into one of these patterns (TE plug, short notes) with project 10 open.

**Content mode, same result (2026-11-01, 18:30 local).** In content mode the disk's `projects/project09.opz` already
held the file-service writes' patterns 4–16 byte for byte, while the device showed them empty [proven]: the file
on disk and the device's working copy differ. A new file (`build/hc1/cm09.opz`, built from the disk's own copy;
patterns 4–15 the test patterns, pattern 16 a **control**: H-P1's one-step kick on TE's plug 130 in the original empty
pattern 16; track byte +5 set to 5 on every track of 4–16, as in all 62 of the user's patterns with notes) was copied
over it, verified byte for byte on the disk, and ejected. After the import and restart, **patterns 4–16 were all
dark, the control included**; 1–3 lit as before [proven, user]. So neither TE's own import route nor the file
service brings these patterns in. The user notes that restoring projects through content mode is well known to
work for files the OP-Z wrote itself [notes, user] — so the difference is most likely in the bytes we generate
[inferred]. Candidates: the other track records and sound blocks of a never-used pattern (the control kept pattern
16's constructor defaults, e.g. plug 13 on kick), or a check not yet found in `loadProject`. Decisive next test:
`copy_pattern` of a pattern the OP-Z wrote (pattern 1) into an empty pattern, nothing else changed, then bisect.

**Answered by a copy-pattern test (2026-11-01, 17:46 UTC).** Patterns 5–8 of project 9 were rebuilt on a copy of
the user's pattern 1 (a pattern the OP-Z wrote) and written with `projects put` while project 10 was open: 5 = exact
copy; 6 = the copy with plug 60907 on all eight instrument tracks; 7 = the copy with its notes removed and H-P1's
kick on steps 1, 5, 9, 13; 8 = the full H-C1 zone-0 pattern (60907 on every track, neutral sounds, one 98,303-unit
note per track) on the copy. **All four lit up** [proven, user]. So the file-service write and reload work, and our
plug ids, notes, long durations and sounds are accepted; what made the earlier patterns vanish was the base they were
built on. The rule we wrote then — build on a copy of a pattern the device wrote — worked, but we did not know why.

### What it really was: the in-use bit (2026-11-13, Q-50 closed)

**Bit `0x08` of byte 21,387 — bit `0x0800` of the u16 at 21,386, the field §3 calls the master routing mask — is the
flag the OP-Z uses to treat a pattern as in use.** Clear, and the pattern key stays dark and the pattern is ignored,
whatever notes the file holds. Set it and the pattern loads, lights and plays.

Reported by Reddit user u/Economy_fig3800 via the OP-FREE repository, 2026-11-13, from their own device work
(a framework that converts jazz standards into OP-Z project files); verified here against the user's own files and
our Q-50 device history before being written down.

What each piece of evidence actually shows:

- **[data] The byte-level correlation across the user's ten projects** (`backups/20261026T170542988907Z-projects`,
  read-only, 160 patterns): every one of the **62 patterns that hold notes has bit `0x0800` set — no exceptions**.
  Of the 98 patterns with no notes, 94 have it clear and **4 have it set**, which fits "in use" (a pattern the user
  opened and left empty) and does not fit "has notes". Observed u16 values: unused `0x00F0` and `0x00F8`; used
  `0x0803 0x080B 0x080F 0x0820 0x0870 0x08B0 0x08E0 0x08E4 0x08F0`, plus one `0x18F0`. The low byte varies — that is
  the routing — while `0x0800` tracks use. `0x1000` appears once and is unexplained: **Q-90**.
- **[proven] Our own 2026-11-01 device runs, re-read in this light.** Walking `project09.opz` through the backups of
  that day in time order and printing, per pattern, whether it holds notes and whether `0x0800` is set:

  | Backup (UTC) | Patterns with notes | In-use bit | What the user saw |
  |---|---|---|---|
  | 15:08, 15:17, 15:36, 15:42 (writes 1–3) | 4–16 (varying) | **clear** | dark |
  | 16:27 (content mode, incl. the TE-plug control in pattern 16) | 4–16 | **clear** | dark, control included |
  | 16:38, 16:42, 16:46 | 4–16 | **clear** | dark |
  | 16:49 (the copy-pattern test) | 1–9, 11–16 | **set on 5–8** | **5–8 lit** |

  Every pattern the device refused to show had the bit clear; the four that lit were exactly the four that carried
  it. Nothing else distinguishes them. So the copy-pattern rule worked for one reason only: a copy carries the
  source pattern's bit along with everything else.
- **[notes, outside report] The decisive single-byte write**, which we have not run ourselves: in a project copied
  off the OP-Z in content mode, six generated patterns were written and only pattern 1 — whose byte came from the
  template — lit. A second write that changed **only byte 21,387 to `0x08` in patterns 2–6** made all six light and
  play, every note matching over MIDI, the chain advancing correctly. The same report found that in a project saved
  on the device with notes only in pattern 1, the only pattern-level differences between pattern 1 and the unused
  patterns were the notes, track byte `+10` (last note played) and byte 21,387: bytes 21,384..21,391 were
  `ff 00 f0 08 00 00 00 00` against `ff 00 f0 00 00 00 00 00`, in two different projects. Our hardware check
  **H-P2** (§10) runs this ourselves; until it passes, this one step stays [notes], and what is [proven] here is
  the correlation above, not the causal single-byte write.

**Why our earlier search missed it.** We looked for a byte that separates patterns *with notes* from patterns
*without* and found none that was clean — because four empty patterns carry the bit. Read as "in use" rather than
"has notes", the same data separates perfectly. The claim "no byte or global field marks a pattern in use" is
withdrawn.

**Rule, replacing the copy-pattern rule: set the in-use bit on every pattern you generate.**
`opz_project.set_pattern_in_use(p)` / `@opz/core`'s `setPatternInUse(p)` do it, and every setter that writes content
into a pattern (`set_notes` with notes, `set_component`, `set_lock`, `set_sound`, `set_track`, and the `write_rhythm`
/ `write_pages` generators) now marks the pattern automatically, so ordinary use needs no extra call. `copy_pattern`
still works, because the copy carries the source's bit — it is no longer required, only one way of getting the bit.
`problems()` warns when a pattern holds notes but lacks the bit. Clearing notes never clears the bit, matching the
device, which keeps visited-but-empty patterns marked.

Still true regardless: verify a project write **on the device**, not only by the read-back, and expect the patterns
the user opens to be saved over the file.

## 10. Hardware check H-P2: the in-use bit on a never-used base (not run)

Goal: prove on our own OP-Z what the outside report found — that the in-use bit alone, with nothing else changed, is
what makes a generated pattern appear. §9's evidence is a correlation over files plus someone else's device run;
this closes it as [proven] here. Nothing in the user's music is at risk: one spare pattern of the test project.

Rules of SAFETY.md apply: TE's app closed, one client, the user confirms each write and sees the result before the
next step, and the dry run comes first.

| Step | Do | Expect | If not |
|---:|---|---|---|
| 0 | OP-Z in normal mode, playback stopped, **project 10 open** (not 9). TE's app closed | — | — |
| 1 | `python3 tools/opz.py projects backup` | ten files; keep the folder — this is the rollback | not served: stop |
| 2 | In the fresh backup of project 9, pick a pattern **P that has never been used**: `pattern_has_notes(P)` false **and** `pattern_in_use(P)` false (`opz_project.py info` lists `patterns_in_use`). Patterns 1–3 are the user's; do not touch them | one such P exists (patterns 10–16 of project 9 were in use after the 2026-11-01 runs, so expect to have to pick carefully, or use a project with spare patterns and the user's say-so) | no never-used pattern: ask the user which project to use, or clear the bit on a spare one first (step 7 restores it) |
| 3 | Build file A from that backup: write H-P1's kick (steps 1, 5, 9, 13, note 53, TE's plug 130 as pattern 3 uses) into pattern P on its **never-used bytes** — no `copy_pattern` — and then **clear** the in-use bit (`set_pattern_in_use(P, False)`), so the only difference from the old failing writes is deliberate. `opz_project.py check` must report the new warning ("holds notes but is not marked in use") and no other problem | the warning, nothing else | fix the file; do not write it |
| 4 | Build file B from file A by setting **only** that one byte: `set_pattern_in_use(P)`. Confirm with `cmp -l A B` that **exactly one byte differs** (offset 4 + 568 + P × 21,392 + 21,387, `00` → `08`) | one differing byte | rebuild; a second difference invalidates the test |
| 5 | `python3 tools/opz.py projects put 9 A --overwrite --dry-run`, then (user confirms) without `--dry-run` | "written and verified" | the tool restores the backup itself; report the log |
| 6 | On the device: switch to project 9 and look at pattern P, then switch back to 10 | **P is dark and empty** (the 2026-11-01 result reproduced) | if P lights, the bit is not the cause: stop, record it, Q-50 reopens |
| 7 | `python3 tools/opz.py projects put 9 B --overwrite` (user confirms), then on the device switch to project 9, open P and play it | **P lights and plays the kick on steps 1, 5, 9, 13**; switch back to 10 | if P stays dark, the single byte is not sufficient: record it, Q-50 reopens |
| 8 | Fresh `opz.py projects backup`; `opz_project.py diff` against step 1 | project 9 differs in pattern P only (plus whatever the device saved for patterns the user opened) | report the differing fields |

**Rollback** (any step, including a crash — power-cycle first): `python3 tools/opz.py projects put 9
<step 1's backup>/projects/project09.opz --overwrite`, then a fresh read and `opz_project.py diff` against step 1's
file to confirm project 9 is back as it was. Projects 1–8 and 10 are never written.

Expected result if §9 is right: step 6 dark, step 7 lit, one byte apart. That makes the single-byte write [proven]
here and not only [notes, outside report].

## 11. Open

Q-39 (reloading the open project), Q-40 (track bytes +5, +8, +11, note duration −256, global 518–560), Q-41 (does a
`$09` write persist without `debug-save`), Q-42 (chains: 14 or 15 slots, 31 or 32 entries), Q-43 (does content mode
check project files on import), **Q-90** (bit `0x1000` of the u16 at 21,386, seen once, and the exact meaning of its
other bits): see [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md). Q-38 and **Q-50 are answered** (§9); H-P2 above is the one
hardware check still to run for Q-50's single-byte step.
