# The loader (workstream W2)

The panel next to the synth lab on the *Synths & effects* page: it puts the visitor's picks on their OP-Z. It is built
on `@opz/core` (the lab's `web/core`, `OP-FREE/core` in the public repository) and keeps the core's safety flows
exactly: back up both settings files → plan → one confirm sheet showing what changes → push → read back → roll the
slot map back if it differs. Nothing here writes to an OP-Z any other way.

## The five steps

| Step | What happens | Writes |
|---|---|---|
| 1 Pick | **Per track**, as on the OP-Z: tabs for tracks 1–10 (kick, snare, perc, sample · bass, lead, arp, chord · fx1, fx2), and for the track shown its ten black keys, each set to one of our engines, *Original* (the sound the first backup had there) or *Leave as it is*. Ticking an engine in the track's list puts it on the first key left as it is. *Featured* (the default: the catalogue's `featured` engines on keys 1–8, at most `DEFAULT_PICKS`), *All original*, and *Copy to* the other tracks of the same kind. *Listen* sends `opfree:audition` to the lab; the lab's `opfree:pick` puts an engine on (or takes it off) every track it is made for | none |
| 2 Connect & back up | Web MIDI with SysEx (Chrome or Edge on a computer, https or localhost) with the *quit TE's app* warning, or the **demo OP-Z** (the core's `FakeOpz`). Reads the settings and backs them up at once: the first backup of an OP-Z in this browser is what *original* and *Restore original layout* go back to | none (a read) |
| 3 Install | One confirm sheet: every slot before → after, the slots left alone (the visitor's samples), the engines and files sent, restart needed, the backup id. Then the core's `install` per engine (install only, no slot), `presetsPush` for engines already installed, and one `writeSlotMap` for the layout, each with its own backup, checks and read-back; the batch stops at the first failure | engines, `patch.json`, the slot map |
| 4 Content mode | The user-sample check (no disk action while any sample the visitor made sits in no slot), the device gesture (hold *track* while switching on, manual p.61), then the bake of the picks' presets into `config/userPresets.dat` through the folder picker: backup (in this browser; saved as a file only when the browser can't keep data, e.g. a private window), confirm, write, read back, the original put back on a mismatch | `config/userPresets.dat` only |
| 5 Eject & play | Eject, pick a sound (hold *track*, tap a black key), and **Restore**: any settings backup (through `writeSlotMap`), or a preset-store backup back onto the disk | the slot map, or `userPresets.dat` |

**Which engine goes on which track.** Synth and drum engines only on tracks 1–8, effects only on fx1 and fx2 (9 and
10), as the OP-Z is laid out; within that, an engine only on the tracks of its kind (synths on bass–chord, drums on
kick–sample) that its trackmask names (`model.ts` `canGoOn`). The panel offers nothing else, a stored choice is cleaned
on load, and `plan.ts` refuses a cell that crosses the line. Cross-placement stays off until it is tested on hardware.

**Original** means the plug the first backup had in that slot: teenage engineering's own sound, which every OP-Z
already carries. TE's engines are never uploaded or hosted; a TE plug we displace stays installed, so putting it back
is a slot-map change only. Before connecting, the panel says *your OP-Z's own sound in this slot*; after, it names the
plug from the device's `plugs.json`.

**The visitor's samples** (flag-2 plugs) are never touched: once connected, a key holding one shows as locked on its
track and can't be given anything; a track still on its default moves its engines past those keys; a stored choice
that names such a cell is skipped there (listed on the sheet); the core's `writeSlotMap`
refuses any map that would leave one in no slot, a restore keeps their current cells, and step 4 stays locked while
one is loose (SAFETY.md rule 5).

## Files

| File | What |
|---|---|
| `model.ts` | No DOM, no device: per track, what each black key gets; the default eight; which engine may go on which track |
| `plan.ts` | No DOM: each track's keys against an OP-Z's settings (cells, installs, the target slot map, the restore target), with the core's `planInstall` and `slotMapProblems` |
| `controller.ts` | No DOM: connection, one operation at a time, the install batch and the restore on the core's flows, the interrupted-write record (slimmed from `web/app/src/controller.ts`) |
| `store.ts` | Backups in IndexedDB (`op-free`), the first backup per device, the bake's copies (from `web/app/src/backups.ts`) |
| `disk.ts`, `samplegate.ts`, `explain.ts`, `dom.ts`, `transport/*` | Ported from `web/app/src` (the bake on the content-mode disk, the user-sample gate, failure advice, the DOM helper, Web MIDI, the demo transport, worker timers) |
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
there means "on my OP-Z": the loader puts the engine on every track it is made for (first key left as it is) or takes
it off all of them. It sends `opfree:pick {dir, picked}` when an engine comes onto its first track or leaves its last
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
6. **The default batch** (*Featured* on every track): note how long it takes; keys 1–8 hold the picks, 9 and 0 are
   unchanged, the samples untouched. A restart warning appears only for an upgrade.
7. **Restart** the OP-Z: engines and slots stay, presets are gone (RAM); connect and install again: the sheet offers to
   push the presets again only.
8. **Content mode.** Disconnect, switch off, hold *track* while switching on. Step 4 shows *Sample check passed
   (checked …)*. *Choose the OP-Z disk and bake…* → the sheet (no download dialog) → *Write presets* →
   *written and read back*. Eject. After the restart, with nothing connected, the white keys recall the baked presets;
   `opz.py presets bake --disk … --show` lists them.
9. **Put back** that preset-store backup from step 8 in content mode: the file is the original again.
10. **Restore original layout** in normal mode: the sheet lists every cell; verified; the OP-Z has its first layout.

Report each step's sheet, result and log (the *Log* at the bottom of the panel).
