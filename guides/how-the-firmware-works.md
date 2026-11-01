# How the OP-Z firmware works

A plain-language tour of what runs inside an OP-Z, and why new synths can be added without changing the firmware.
Every statement here is backed by the [research](../research/README.md), where each claim carries an evidence label
and an address you can check; this page leaves the labels out to stay readable. It describes firmware 1.2.45.

## Three small computers

The OP-Z has three processors [research: [01 — System overview](../research/architecture/01-system-overview.md)]:

- an **Analog Devices Blackfin+** DSP that does nearly everything: the sequencer, every synth and effect, USB, files;
- a **Nordic nRF52** for Bluetooth MIDI and the app link;
- a **Cypress PSoC** that runs power, charging, the keys and the LEDs.

Everything OP-FREE does happens on the Blackfin. It has no floating-point unit, so all audio code is fixed-point
arithmetic (numbers between −1 and 1 stored as 32-bit integers).

## Three ways to start

| How you switch it on | Mode | What it is for |
|---|---|---|
| Normally | **Synth mode** | Playing. USB MIDI and the file service (how engines are installed) work here |
| Holding *track* | **Content mode** (*OP-Z Disk*) | A USB disk for samples, projects and settings |
| Holding *screen* | **Update mode** (*OP-Z Boot*) | Installing a firmware update; also the way back if everything else fails |

Installing community engines uses only synth mode, and content mode for presets. Update mode matters for recovery
and, later, for firmware mods.

## The OS

The Blackfin runs one program, the OS (a file called `os.ldr` on the internal storage), started by a small
bootloader. Inside it a real-time scheduler runs a few tasks side by side: the sequencer, the audio engine, the file
manager, USB. Storage is a NAND chip with a flash file system: the OS, the engines, settings, samples and the ten
projects are all files on it [research: [01](../research/architecture/01-system-overview.md#storage)].

## Engines are loadable modules

This is the key fact. The OP-Z's own synths and effects are **not** built into the OS. Each one is a separate file
in an `engines/` folder, a small block of DSP code that the OS loads into memory when it starts and calls through a
few functions [research: [02 — The sound engine host](../research/architecture/02-sound-engine-host.md)]:

- **instruments** get called once per voice and audio block to *add* their sound into the mix, and are told when a
  note starts and stops;
- **effects** get called once per block to transform the audio of the FX1 or FX2 track;
- both read the OP-Z's 18 sound parameters (the two P1/P2 knobs, envelope, filter, LFO and so on), already
  smoothed by the OS.

The OS also lends engines shared tools: sine and filter tables, the tempo, the sample packs. Because the OS loads
engines from files, anyone who writes a module in the same shape can add a new sound. That shape (the *engine ABI*)
is what our [SDK](../sdk/docs/engine-abi.md) documents and what its simulator imitates.

Engines run with no memory protection and a fixed CPU budget per audio block. A bug, or an engine that takes too
long, can crash or freeze the unit; that is why every engine here passes a crash check and a CPU measurement first
[research: [crash safety](../research/crash-safety.md), [CPU budget](../research/cpu-budget.md)].

## Plugs, slots and memory slots

Three ideas connect an engine to a sound you can pick
[research: [03 — Content and delivery](../research/architecture/03-content-and-delivery.md)]:

- an **engine** is the code file;
- a **plug** is a playable sound built on an engine: an id number, a name, which tracks may use it, the names of
  its two knobs, its presets. teenage engineering's plugs have small ids; user plugs start at 60000;
- a **sound slot** is one of the ten sounds per track you choose with the black keys. The slot configuration says
  which plug sits in each.

Separately, each engine is linked to run at one fixed place in memory, its **memory slot**. There are 90 usable
memory slots in three sizes (50 KB, 10 KB, 5 KB), and teenage engineering's engines use 21 of them. Two engines
must never share a memory slot, so every engine in this project gets its own
[research: [memory slots](../sdk/docs/memory-slots.md)].

## How a new engine gets in

The OP-Z app talks to the OP-Z over USB MIDI with a **file service** (SysEx messages that list, upload and download
files). OP-FREE uses the same service: it uploads the engine file, adds the plug to the OP-Z's plug list and puts it
in a slot. The OS checks the file's structure before loading it. Nothing is flashed and the firmware is unchanged;
a restore puts the slot map back [research: [03](../research/architecture/03-content-and-delivery.md#delivery-path-1-primary-sysex-file-service-in-synth-mode)].

## The update package

teenage engineering ships firmware as a single update file (`.zfw`). At a high level it is an **encrypted ZIP
archive**: inside are the OS image, the bootloader, the Bluetooth and keyboard chips' firmware, and the factory
content (engines, sample packs, presets). In update mode the OP-Z unpacks it onto its internal storage;
the only integrity check during install is each ZIP entry's checksum
[research: [01](../research/architecture/01-system-overview.md#storage), [firmware map](../research/OPZ_FIRMWARE_MAP.md)].

That has two consequences. Firmware mods are technically possible, because there is no signature to forge. And
they are risky, because a bad image can leave a unit that only starts in update mode. OP-FREE will therefore never
host firmware: future mods will be small patches applied to **your own** copy of the official file, in your browser
([extract your firmware](extract-your-firmware.md), [firmware mods](../firmware-mods/README.md)).

## Where to go next

- Build an engine: [SDK quickstart](../sdk/docs/QUICKSTART.md).
- The full picture with addresses: [research reading order](../research/README.md#reading-order).
- What nobody knows yet: [open questions](../research/OPEN_QUESTIONS.md).
