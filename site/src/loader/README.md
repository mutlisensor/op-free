# The loader (workstream W2)

The panel next to the synth lab on the *Synths & effects* page: it puts the visitor's picks on their OP-Z. It is built
on `@opz/core` (the lab's `web/core`, `OP-FREE/core` in the public repository) and keeps the core's safety flows
exactly: back up both settings files → plan → one confirm sheet showing what changes → push → read back → roll the
slot map back if it differs. Nothing here writes to an OP-Z any other way.

## The five steps

| Step | What happens | Writes |
|---|---|---|
| 1 Pick | **Per track**, as on the OP-Z: tabs for tracks 1–10 (kick, snare, perc, sample · bass, lead, arp, chord · fx1, fx2), and for the track shown its ten black keys, each set to one of our engines, *Original* (the sound the first backup had there) or *Leave as it is*. Every key starts as *Leave as it is*, so nothing changes until the visitor picks a sound for a key (step 3 stays off until then). Ticking an engine in the track's list puts it on the first key left as it is. *Featured* (the catalogue's `featured` engines on keys 1–8, at most `FEATURED_PICKS`), *All original*, and *Copy to* the other tracks of the same kind. *Listen* sends `opfree:audition` to the lab; the lab's `opfree:pick` puts an engine on (or takes it off) every track it is made for | none |
| 2 Connect & back up | Web MIDI with SysEx (Chrome or Edge on a computer, https or localhost) with the *quit TE's app* warning, or the **demo OP-Z** (the core's `FakeOpz`). Reads the settings and backs them up at once: the first backup of an OP-Z in this browser is what *original* and *Restore original layout* go back to | none (a read) |
| 3 Install | One confirm sheet: every slot before → after, the slots left alone (the visitor's samples), the engines and files sent, restart needed, the backup id. Then the core's `install` per engine (install only, no slot), `presetsPush` for engines already installed, and one `writeSlotMap` for the layout, each with its own backup, checks and read-back; the batch stops at the first failure | engines, `patch.json`, the slot map |
| 4 Content mode | The user-sample check (no disk action while any sample the visitor made sits in no slot), the device gesture (hold *track* while switching on, manual p.61), then the bake of the picks' presets into `config/userPresets.dat` through the folder picker (the visitor picks the disk's **`config` folder**: Chrome refuses the top of a disk as "contains system files", seen on macOS 2026-11-06; the disk itself is still taken where a browser allows it). Our presets replace hand-stored ones on the same keys by default (*Also replace…* ticked; the old file is kept): backup (in this browser; saved as a file only when the browser can't keep data, e.g. a private window), confirm, write, read back, the original put back on a mismatch. An OP-Z on which no preset was ever stored by hand has no such file: the bake then **makes** it (an empty store with only our presets; nothing to back up; deleted again on a mismatch), not yet tested on hardware (H-9b below). The demo has *Try it as a new OP-Z* for that case | `config/userPresets.dat` only |
| 5 Eject & play | Eject, pick a sound (hold *track*, tap a black key), and **Restore**: any settings backup (through `writeSlotMap`), or a preset-store backup back onto the disk; **Take OP-FREE off** (the core's `removeOurs`) and **Check a backup folder** before a content-mode restore | the slot map, or `userPresets.dat`; for Take OP-FREE off: an empty boot file, the slot map, the removals |

**Which engine goes on which track.** Synth and drum engines only on tracks 1–8, effects only on fx1 and fx2 (9 and
10), as the OP-Z is laid out; within that, an engine only on the tracks of its kind (synths on bass–chord, drums on
kick–sample) that its trackmask names (`model.ts` `canGoOn`). The panel offers nothing else, a stored choice is cleaned
on load, and `plan.ts` refuses a cell that crosses the line. Cross-placement stays off until it is tested on hardware.

**Leave as it is** keeps whatever the slot holds, one of our engines included: a visit from a fresh browser changes
nothing, and unpicking an engine leaves it where it is. Taking one of ours off a key is *Original* there.

**Original** means the plug the first backup had in that slot: teenage engineering's own sound, which every OP-Z
already carries. TE's engines are never uploaded or hosted; a TE plug we displace stays installed, so putting it back
is a slot-map change only. Before connecting, the panel says *your OP-Z's own sound in this slot*; after, it names the
plug from the device's `plugs.json`.

**The visitor's samples** (flag-2 plugs) are never touched: once connected, a key holding one shows as locked on its
track and can't be given anything; a track still on its default moves its engines past those keys; a stored choice
that names such a cell is skipped there (listed on the sheet); the core's `writeSlotMap`
refuses any map that would leave one in no slot, a restore keeps their current cells, and step 4 stays locked while
one is loose (SAFETY.md rule 5).

**Room and taking OP-FREE off** (docs/roadmap/restore-space.md in the lab). Step 3 and the install sheet show how many
of the OS's 200 sound definitions are free now and after the install, and warn below 60 (a content-mode restore of the
visitor's own backup needs one per sample; a full table loses samples silently). Step 5 has *Take OP-FREE off…* (the
core's `removeOurs`: an empty boot file, the slot map with the first backup's sounds put back, the removals; one sheet,
each write read back) and *Check a backup folder…* (the core's `checkContentRestore` on a backup of the OP-Z disk:
names and sizes only). The visitor's guide: `OP-FREE/guides/restore-a-backup.md`.

**When the OP-Z doesn't answer.** Each session open waits up to 15 s for the OP-Z's `$34` (normally ~0.5 s), then,
after a 3 s pause, tries once more (`controller.ts` `TrackingOpz.session`); after 2 s the busy line counts down. The
OP-Z receives no file before it answers, so the second try repeats no step of a flow [code: `web/core/src/session.ts`
`open`, `push`]. A friend's first install stopped right there on 2026-11-05: 30 s with no `$34`, 2 ms after the
backup's session closed; the cause is open (another client such as TE's app is the first guess). The interrupted-write
banner appears only when a push went out and was never read back; otherwise the log says *nothing written* or
*nothing was left half-written*. The core's warning about tracks 12–15 is dropped here: every OP-Z's slot map lists
them [proven: our own backups].

**The problem log.** The panel's *Log* is the site's problem log (`src/shared/diag.ts`): the loader's and the bake's
lines, uncaught errors, `console.error`, the page being frozen (a visible tab's timer more than 2 s late), page opens
and closes (a visit with no close crashed, froze or was killed), and the lab's `opfree:audition` / `opfree:pick`. Each
line is saved to localStorage as it is written, so a frozen tab leaves it for the next visit; at most 600 lines. Nothing
is sent anywhere: *Copy report* (in the Log and in every page's footer) puts the browser, the loader's state and picks
(`reportPart`) and the log on the clipboard, or saves a text file where the clipboard is blocked. Testers paste it into
their message or a GitHub issue.

## Files

| File | What |
|---|---|
| `model.ts` | No DOM, no device: per track, what each black key gets; the default eight; which engine may go on which track |
| `plan.ts` | No DOM: each track's keys against an OP-Z's settings (cells, installs, the target slot map, the restore target), with the core's `planInstall` and `slotMapProblems` |
| `controller.ts` | No DOM: connection, one operation at a time, the install batch and the restore on the core's flows, the interrupted-write record (slimmed from `web/app/src/controller.ts`) |
| `store.ts` | Backups in IndexedDB (`op-free`), the first backup per device, the bake's copies (from `web/app/src/backups.ts`) |
| `restorecheck.ts`, `disk.ts`, `samplegate.ts`, `explain.ts`, `dom.ts`, `transport/*` | Ported from `web/app/src` (the bake on the content-mode disk, the user-sample gate, failure advice, the DOM helper, Web MIDI, the demo transport, worker timers) |
| `demo.ts` | The demo OP-Z's factory state (every drum and synth slot filled, three of the visitor's samples, one in sample slot 4) and an in-memory content-mode disk |
| `sheet.ts`, `index.ts`, `loader.css` | The confirm sheets and the panel |

## Catalogue fields the loader reads

`dir`, `label`, `kind`, `summary`, `featured`, `presets`, `engine` and `manifest`. `manifest` must be `plug.json` as
written: the loader needs `plug.id`, `plug.version`, `plug.trackmask`, `memoryslot`, `engine_version` and, for master,
`master_in_place`. Presets may carry `default_octave` (as in `presets.json`) or `octave`. `engine` is the URL of the
built `.engine`; without it an engine can be installed only on the demo OP-Z (a placeholder). `caution` (an engine
that crashed or froze a real OP-Z): shown on its row, never ticked by default, refused for a real OP-Z (the plan
errors), allowed on the demo. Test plugs (60900–60999) are never offered.

**The lab's ticks.** The lab starts with the featured engines ticked and follows `opfree:pick` from anyone. A tick
there means "on my OP-Z": the loader puts the engine on every track it is made for (first key left as it is); an untick
takes it out of the picks (those keys go back to *Leave as it is*, so an engine already installed stays on the OP-Z). It sends `opfree:pick {dir, picked}` when an engine comes onto its first track or leaves its last
one, for each stored pick that differs from the featured set after it loads, and `picked: false` back when it refuses
a tick from the lab (every key of those tracks taken).

## Tests

```bash
npm --prefix OP-FREE/site test       # model, plan, and the whole flow against FakeOpz (node:test, @opz/core via test/register.ts)
npm --prefix OP-FREE/site run typecheck
```

In the browser: `.claude/launch.json` → `op-free-loader` (port 5181), `#/synths`, step 2 → *Try the demo OP-Z*.

## Not yet tested on hardware

The install as one batch (install without a slot, then one slot-map write), the restore through `writeSlotMap` and
the bake from this page. Each part is a core flow the command-line tools use (the bake is proven with `opz.py presets
bake`, H-9); the panel says so where it matters. Hardware checklist for the device session:

1. **Prepare.** Engines built and passed `opz_verify` and `opz_crashcheck`; avoid `fold` until its freeze (H-10,
   docs/STATUS.md) is understood. TE's app closed, one session on the device, desktop Chrome at
   `http://localhost:5181/#/synths`. Note the active project; use a disposable one (SAFETY.md rule 7).
2. **Connect.** Step 2 → *Connect over USB*. Expect *Backed up* with the first backup's id. Optional: `opz.py backup`
   after disconnecting; both settings files must be byte-identical (the log has the SHA-256 in `backup.json`).
3. **One engine.** Every track *All original* except bass: one engine on key 1, the other keys *Leave as it is*.
   *Review and install…* is the dry run: check the sheet lists exactly one cell (bass 1) and one engine, then *Cancel*:
   nothing must change (`opz.py status`).
   Run it again and *Install*: expect *Installed and verified*. On the OP-Z, hold *bass* and tap black key 1: the
   engine plays. The visitor's samples are where they were.
4. **Original back.** Bass → *All original* → install: the sheet sends no engine, one cell goes back; verified; the TE
   sound is back on key 1, our plug stays installed (`opz.py status`: displaced).
5. **Different sounds per track.** Bass and lead with different engines on the same key, one effect on fx1 only: the
   sheet and the OP-Z (hold the track, tap the key) agree, track by track.
6. **The featured batch** (*Featured* on every track): note how long it takes; keys 1–8 hold the picks, 9 and 0 are
   unchanged, the samples untouched. A restart warning appears only for an upgrade.
7. **Restart** the OP-Z: engines and slots stay, presets are gone (RAM); connect and install again: the sheet offers to
   push the presets again only.
8. **Content mode.** Disconnect, switch off, hold *track* while switching on. Step 4 shows *Sample check passed
   (checked …)*. *Choose the OP-Z's config folder and bake…* (OP-Z → config → Select) → the sheet (no download dialog) → *Write presets* →
   *written and read back*. Eject. After the restart, with nothing connected, the white keys recall the baked presets;
   `opz.py presets bake --disk … --show` lists them.
9. **Put back** that preset-store backup from step 8 in content mode: the file is the original again.
10. **Restore original layout** in normal mode: the sheet lists every cell; verified; the OP-Z has its first layout.
11. **Take OP-FREE off** (step 5) after step 6's batch: the sheet lists every cell going back to the first backup's
    sound, an empty boot file and the removals; *Remove OP-FREE* → verified; `opz.py status` shows no plug
    60900–61999 and the visitor's samples in their slots; after a restart still none (H-RS1 in the lab's
    docs/roadmap/restore-space.md). *Check a backup folder…* on a copy of the OP-Z disk: "It should go through".

### H-9b: making the preset file on an OP-Z that has none

Content mode takes a `config/userPresets.dat` from the disk at the eject even when the OP-Z has none [code, device:
the config import callback `0x80034d68`, docs/research/presets.md]; no OP-Z has been seen to yet. Run it from this
branch on `http://localhost:5181/#/synths` (the `op-free-loader` dev server), not the live site.

1. **Keep what's on it.** Before formatting: content mode, copy the whole OP-Z disk to the Mac (projects,
   samplepacks, `config/userPresets.dat` with your hand-stored presets), eject; in normal mode `opz.py backup`.
2. **Format** the OP-Z your usual way.
3. **No file yet.** Content mode: the disk's `config/` has **no** `userPresets.dat` (look only), eject. If it is there,
   stop: the format kept it, and this check needs an OP-Z without one.
4. **One engine.** Normal mode, desktop Chrome, step 2 *Connect over USB*, step 1 one engine with presets (e.g. table
   on bass key 1), step 3 *Review and install…* (check the sheet: one cell, one engine) → *Install* → verified.
5. **Make the file.** Disconnect, content mode, step 4 *Choose the OP-Z's config folder and bake…*: in the window,
   *OP-Z* in the sidebar → open *config* → *Select*. Expected: the sheet says
   *Create config/userPresets.dat on OP-Z with our presets?*, no *Backed up first* line → *Write presets* → *The preset
   file was made and read back*. On the Mac: `config/userPresets.dat` is 351,202 bytes;
   `opz.py presets bake --disk /Volumes/OP-Z --show` lists table's presets.
6. **Eject.** Expected: the OP-Z imports and restarts. Hold *bass*: the white keys of table's presets lit **solid**.
   Switch off and on with nothing connected: still there.
7. **The OP-Z keeps it.** Content mode again: `config/userPresets.dat` is back on the disk (the OP-Z's own copy),
   `--show` lists the same presets. Then store one preset by hand on another key, restart: ours and that one both
   there.

If step 6 shows no presets: next content mode, look in `rejected/` and `config/`, and send *Copy report* plus
`--show`. Rollback: none needed (the OP-Z had no stored presets); step 1's copy brings back the old projects and
presets (put `userPresets.dat` back in `config/` in content mode, eject).

Report each step's sheet and result, and after a failure or a hang *Copy report* (in the *Log* at the bottom of the panel).
