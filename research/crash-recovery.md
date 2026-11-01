# Recovery runbook: when an engine or a boot file crashes the OP-Z

What to do when one of our engines (or `settings/plugs_patch.json`, the boot file) makes the OP-Z crash, up to
"it crashes at every start". Evidence and addresses: [research/crash-safety.md](crash-safety.md).
Labels as in [CONTRIBUTING.md](../CONTRIBUTING.md). The rules in [SAFETY.md](../SAFETY.md) still hold; above
all: **never type into the update-mode serial console**.

## First: which kind of crash?

| You see | It is | Evidence |
|---|---|---|
| The OP-Z restarts by itself (screen goes, it boots again) | a CPU exception inside an engine or the OS. TE's handler logged it and rebooted through the watchdog; the bootloader saved the log | [code] |
| It freezes (no sound, no LEDs change, no restart) | a hang: an engine call that never returns, an OS assertion, a parity NMI. No watchdog runs, so it stays frozen until switched off | [code + inferred] |
| A note stops, nothing else | the CPU overload killer, not a crash ("cpu overload: kill notes" in `system.log`) | [code] |

If it froze, switch it off and on. Whether the power switch still works when the main processor hangs is open
(Q-22).

## Where the start-up can crash, and what stays reachable

The order at power-on [code, `opz.ldr` `main_task` `0x8002ce54`; `bootloader.ldr` `0x11a00fe2`]:

```text
bootloader ── SCREEN held? ── yes ─▶ update mode ("OP-Z Boot" disk, systeminfo/ crash logs)        always reachable
    │ no
OS boot init ── plugs.json, slot map, settings/plugs_patch.json import, engine load (initModule)
    │
TRACK held (or /yaffs2/contentmode.plz)? ── yes ─▶ content mode ("OP-Z Disk")
    │ no
audio starts ── FX tracks' renderFX every block, instruments' render while notes sound
    │
normal mode: USB MIDI + SysEx file service (tools/opz.py, the web app)
```

| Crash happens in | Normal mode | Content mode | Update mode |
|---|---|---|---|
| an instrument's `render`/`onNoteOn` (only when its track plays) | yes, as long as nothing plays that track | yes | yes |
| an effect's `renderFX` (runs every block on its FX track) or `init` at project load | only for a moment, or not at all | yes | yes |
| the boot-file import or an engine's `initModule` | no | **no** (both run before the content-mode check) | yes |

## Ladder

Go down only as far as you must. Each step names what it changes.

### 1. Crash when a track plays (most likely)

1. Switch off and on. Don't play that track (and don't let the project play).
2. Remove or unassign our plug over USB: `.venv/bin/python tools/opz.py remove <plug id> --unassign`
   [proven 2026-10-19: macro 61010 removed this way]. Or move the track to another sound slot by hand (hold the
   track + a black key, manual p.25).
3. If the crash came right after a boot-file write, also empty the boot file:
   `.venv/bin/python tools/opz.py presets persist --off` (writes `{"plugs":[]}`; the rollback of hardware check H-7
   on branch `claude/determined-mirzakhani-2f2d48`, not yet on main).

Changes: the slot map and plug records, through the normal file service, with the usual backup first.

### 2. Crash a few seconds after every start (an effect, or a plug the boot project selects)

1. Try step 1 quickly after switching on (connect USB first, run the `remove` command the moment the OP-Z
   appears). It works if the crash needs a note or a track that is not playing.
2. Otherwise content mode: switch off, hold **TRACK**, switch on (all track LEDs green; the Mac mounts the disk).
   Content mode **cannot** reach `engines/` or `settings/` [code: the only synced folders are `rejected`, `config`,
   `projects`, `bounces` and `samplepacks`]. What it can do:
   - **back up** `projects/` and `config/` to the Mac before anything else;
   - take away what makes the boot project select our plug: the project files are in `projects/`; replacing the
     project the OP-Z starts in with an older backup or an empty project changes which sound slot each track uses
     [inferred: the per-track slot choice is stored in the project; not tested];
   - eject (or press play) to apply; the OP-Z restarts.
3. Then step 1 in normal mode to remove the plug for good.

### 3. Crash at every start, content mode included (boot file or `initModule`)

1. **Read the crash log first** (hardware check H-10, read only): switch off, hold **SCREEN**, switch on. If the
   OP-Z is in a crash loop, keep SCREEN held across the restarts: after a crash the bootloader saves the log and
   restarts once more without looking at the keys, and checks them on that next start [code, decompiled; the
   control flow is partly unclear]. Update mode: kick LED blinking white, dial LEDs white (manual p.63); the Mac
   mounts "OP-Z Boot". Copy `systeminfo/` to the Mac, change nothing, eject, switch off. Decode:
   `python3 tools/opz_crashlog.py systeminfo/crash01.log`.
2. **Re-install TE's firmware 1.2.45** with TE's own procedure (copy TE's firmware file onto "OP-Z Boot", eject,
   don't switch off during the update; manual p.63). The installer copies every file of its `system/content/` into
   `/yaffs2/user/`, overwriting: TE's 13 engines, **`settings/plugs_patch.json`** (TE's own boot file, which replaces
   a bad one of ours), `settings/plugPresets.dat`, the readmes and a sample pack [code, `bootloader.ldr`
   `0x828240e0`]. It leaves our engine files, `plugs.json` and the slot map [code for the copy; inferred that it
   deletes nothing else]. At the next start TE's boot file re-imports TE's factory plugs into their tracks
   [inferred]. Then boot normally and continue with step 1 or 2.
3. **Factory reset** (the last resort): in update mode hold SCREEN + STOP for a second; it ends with a blinking
   white LED and four green LEDs. It copies TE's factory content back into `/yaffs2/user/` and **removes all custom
   content** (projects, samples, our plugs) [manual p.63 + code `0x828247d0`]. Restore projects from the step-2
   backup in content mode, then reinstall our engines with `tools/opz.py`.

Never: commands on the update-mode serial console, `debug-crash`, `restart-into-bootloader` from a script, or a
firmware file that is not TE's.

## Afterwards

- Keep the crash logs and the decoded output with the engine's issue; note the engine version that was installed
  (the decoder is right only for that build).
- Run `.venv/bin/python tools/opz_crashcheck.py engines/<name> --full` on the fix before it goes back on the device.
- Note the incident in docs/STATUS.md (the lead's file).

## Avoiding the bad cases

- Run `tools/opz_crashcheck.py` before every install ([engine-abi.md §6](../sdk/docs/engine-abi.md#6-rules-for-engine-authors)).
- Never assign an untested engine in the project the OP-Z starts in (SAFETY rule 7): an effect there runs at every
  start.
- Keep the boot file empty unless its hardware check H-7 passed (research/presets.md "The boot file", branch
  `claude/determined-mirzakhani-2f2d48`): a crash inside its import repeats at every start and only update mode gets
  past it.
