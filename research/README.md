# Research: how the OP-Z works

What we know about the OP-Z's firmware (OS 1.2.45) and how new synths and effects fit into it, written so that
anyone can check it and build on it. The goal is enough understanding to extend the firmware one day; today
everything we ship runs as a *user plug*, loaded by teenage engineering's own OS.

This folder is copied from the project's lab by `tools/opfree_sync.py`; corrections are welcome as issues or pull
requests (they are carried back into the lab).

## Reading order

1. [architecture/01 — System overview](architecture/01-system-overview.md): the chips, the OS, boot modes, storage.
2. [architecture/02 — The sound engine host](architecture/02-sound-engine-host.md): voices, effects, parameters,
   CPU.
3. [architecture/03 — Content and delivery](architecture/03-content-and-delivery.md): engines, plugs, slots, and
   how a new engine reaches the unit.
4. [architecture/04 — Hidden and unreleased](architecture/04-hidden-and-unreleased.md): what the firmware has that
   the menus don't show.
5. [OPZ_FIRMWARE_MAP.md](OPZ_FIRMWARE_MAP.md): the detailed map, with function addresses.

For engine authors the contract itself is in the SDK: [engine ABI](../sdk/docs/engine-abi.md),
[plug format](../sdk/docs/plug-format.md), [memory slots](../sdk/docs/memory-slots.md).

## Topic notes

| Note | What it answers |
|---|---|
| [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md) | **The index**: every question (Q-NN), what it blocks, how to answer it, the answer and its evidence |
| [crash-safety.md](crash-safety.md) | What crashes the OP-Z, how to catch it offline, how to get back |
| [crash-recovery.md](crash-recovery.md) | The recovery runbook, step by step, up to "it crashes at every start" |
| [cpu-budget.md](cpu-budget.md), [cpu-headroom.md](cpu-headroom.md), [cycle-model.md](cycle-model.md) | The audio block, the overload rule, what each engine costs, where more cycles could come from |
| [ahd-drum-notes-cpu.md](ahd-drum-notes-cpu.md) | The drum envelope, drum-key notes, the clock |
| [param-paths.md](param-paths.md) | What reaches an engine's parameters, in what range, and when |
| [routing.md](routing.md) | Where effects can live; the master track |
| [memory-map.md](memory-map.md) | The DDR memory map and the memory slots |
| [host-tables-flags.md](host-tables-flags.md) | The host's shared DSP tables and the plug `flags` |
| [presets.md](presets.md) | Plug presets and live sound parameters |
| [samples.md](samples.md) | Sample packs and drum kits |
| [patterns.md](patterns.md) | Projects and patterns: format, transport, safe writing |
| [hidden-engines.md](hidden-engines.md) | teenage engineering's unreleased engine classes |
| [manual-notes.md](manual-notes.md) | Our summary of the official user guide, with page numbers |
| [toolchain-setup.md](toolchain-setup.md) | Building the Blackfin cross-compiler |

Hardware checks are written as numbered steps (**H-NN**) in the note they belong to, with the expected result and
the way back.

## Evidence labels

Every factual claim carries a label saying how we know it. Don't upgrade a label without new evidence.

| Label | Meaning |
|---|---|
| **[proven]** | Observed on a real OP-Z or measured by a test |
| **[code]** | Read from the firmware or the desktop app's code; the note gives the function address (`0x…`) and which binary (device OS, bootloader, desktop app) |
| **[notes]** | From the community's earlier research ([opz-firmware-notes](https://github.com/ioma8/opz-firmware-notes)) |
| **[manual]** | Stated in teenage engineering's user guide, with a page number (*p.N*) |
| **[inferred]** | A reasoned conclusion that has not been tested |
| **[idea]** | A proposal, not a fact |

Addresses let you check a claim in your own disassembler against your own copy of the firmware. The notes describe
what the code does **in our own words**: this repository holds no firmware, no decompiled code, no keys and no
files of teenage engineering's. How to get your own copy of the firmware: [extract your
firmware](../guides/extract-your-firmware.md).

## Open questions

[OPEN_QUESTIONS.md](OPEN_QUESTIONS.md) is the to-do list of the research. To work on one:

1. Pick an open row (or open an issue with a new question; it gets the next free number).
2. Answer it with evidence, labelled as above. Prefer offline work (disassembler, the simulator, your own
   backups) and say what still needs a hardware check.
3. Send a pull request that writes the answer into the note it belongs to **and** closes the row.

Questions about firmware modifications go in the same list; see [propose a firmware
mod](../guides/propose-a-firmware-mod.md).
