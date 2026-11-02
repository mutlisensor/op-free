# Firmware mods: coming soon

Changes to the OP-Z's own OS for features that new engines can't reach. **Nothing here has been built or run on a
device yet**: this folder is the plan, and where proposals and design notes will live.

## How it will work: bring your own firmware

OP-FREE will never host firmware, modified firmware, or keys. Instead:

1. You download teenage engineering's official update file yourself ([get your own copy](../guides/extract-your-firmware.md)).
2. On the website you pick the mods you want and choose your file. It is read **in your browser**, never uploaded.
3. The builder checks that your file is exactly the version the mods were made for, applies each mod's small patch
   records, and gives you the modified update file to install in update mode.
4. To go back, you install the official file again.

The patch format and the rules for proposals: [propose a firmware mod](../guides/propose-a-firmware-mod.md).

## Before anything ships

Recovery comes first. No mod is published until the project has proven, on a spare unit, that reinstalling the
official firmware brings back a unit after a deliberately bad image. Each mod then needs its own hardware check and
a written revert path.

## Planned mods

All research first; each will get a question in [OPEN_QUESTIONS.md](../research/OPEN_QUESTIONS.md) and a design note
here.

| Mod | What it would do | Status |
|---|---|---|
| **64-step sequencer** | Four pages of 16 steps per pattern | research |
| **Button debouncing** | Stop keys that double-trigger on some units | research |
| **Bluetooth audio** | Find out what the radio and the firmware can do | research |
| **New arpeggiator and *player* note styles** | More note styles for the step components | research |
| **Module updates** | Line module and DMX output improvements | research |
| **More CPU for instruments** | Raise the per-block overload line so instruments get more cycles | patch drafted offline; waits for recovery and a hardware measurement ([CPU headroom](../research/cpu-headroom.md)) |

Ideas welcome: open a *Firmware mod proposal* issue.
