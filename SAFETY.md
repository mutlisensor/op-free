# Safety rules for working with a real OP-Z

Engines run with no memory protection: a wrong engine can crash the OS. These rules keep every experiment
recoverable. The website and `tools/opz.py` follow them for you; they matter when you test your own engines or use
the command-line tools directly. Several were first learned by the
[opz-firmware-notes](https://github.com/ioma8/opz-firmware-notes) project.

## Never

1. **Never send unknown commands to the update-mode serial console.** It is a root shell: one command formats the
   NAND and left a unit able to boot only into update mode, others write or erase the SPI flash or run code.
   Don't type into it at all unless a documented recovery step tells you to.
2. **Never send the hidden `debug-crash` text command**, nor `restart-into-bootloader` casually.
3. **Never write into the factory sample-pack folders** (`samplepacks/<track>-<name>/…`). Files written there
   cannot be removed from the computer again.
4. **Never set a plug's `samples` to a pack that is not installed.** It breaks content mode.
5. **Never remove, overwrite or unassign your own samples** (plugs with flag 2, or any pack our tools did not
   write). Removing a plug deletes its sample file, and content mode deletes user samples that sit in no slot
   ([samples.md](research/samples.md)). Before using content mode, put every sample of your own back in a slot.
6. **Never overwrite or reuse a teenage engineering engine path or memory slot.** Use your own names and the slots
   in [memory-slots.md](sdk/docs/memory-slots.md). Never put two engines in one memory slot.
7. **Never assign an untested engine in the project the unit starts with.** Test in a spare, disposable project.
8. **Never run our tools while teenage engineering's OP-Z app is connected.** Two clients fight over the session.
9. **Never put anything but our plugs' `plugs[]` entries in the boot file `settings/plugs_patch.json`.** The OP-Z
   imports it at every start, so a file that crashed the import would crash every start. `opz.py presets persist`
   builds and checks it. Never write `settings/plugPresets.dat` or `config/userPresets.dat` by hand (the file
   service cannot read them back, and a bad file stops your stored presets loading).
10. **Edit `config/userPresets.dat` only through `opz.py presets bake` (or the website), on the content-mode
   disk.** It backs the file up first, checks it strictly, changes only our plugs' records, keeps presets you
   stored by hand, and reads back what it wrote.
11. **Never write a project file the OS could not have written itself**, nor any project name but
   `project01.opz` … `project10.opz`. The OP-Z loads a written project at once and checks almost nothing in it.
   Write only a spare project, through `opz.py projects put` (backup of all ten, check, read-back, rollback)
   ([patterns.md](research/patterns.md)).

## Always

1. **Back up first**: `opz.py backup` (or the website) saves `settings/plugs.json` and
   `settings/slotConfiguration.json` with their hashes before every install.
2. **Run `opz_verify` and `opz_crashcheck`** on every engine before it leaves the computer
   (`python3 tools/opz_crashcheck.py engines/<name> --full`; [crash-safety.md](research/crash-safety.md)).
3. **Dry-run first**: every `opz.py` command that writes takes `--dry-run`; read the plan before you run it.
4. **Verify every write by reading it back**, never by what was sent (the tools do this).
5. **Keep the engine binary** on your computer: installed engines cannot be read back.
6. **Change one thing per test**, and write down what you changed.

An installed engine stays registered on the unit: removing its plug does not free its memory slot. Install only
engines that passed the simulator, crash and CPU checks.

## If something goes wrong

The full ladder, with what each step changes and why: [the recovery runbook](research/crash-recovery.md).

| Symptom | First step |
|---|---|
| Track silent after install | Put another sound in that slot, then restore (website) or `opz.py restore` |
| The OP-Z restarts by itself | A CPU exception: the OS logs it and restarts. The log is `systeminfo/crash01.log` on the update-mode disk; decode it with `tools/opz_crashlog.py` |
| Crash when the track plays | Switch off and on; remove the plug or restore right away, before playing |
| Crash at start (engine assigned in the start-up project) | Try the restore right after power-on. Content mode (hold *track*) can back up and replace projects but **cannot reach `settings/` or `engines/`**. If the OS never gets that far, update mode (hold *screen*) with teenage engineering's official firmware file reinstalls their content |
| Freezes, no restart | A hang (no watchdog runs in normal mode): switch off and on, then as for a crash |
| Content mode will not mount | Check every plug's `samples` field (rule 4) |
| Crash right after writing the boot file, or at every start since | Switch off and on; if it starts, `opz.py presets persist --off` at once. If every start crashes: content mode and remove `settings/plugs_patch.json` if reachable, else the official firmware in update mode |

Please report crashes as an issue (*Bug report*), with the engine, track, what you did, and the crash log if you
have it.
