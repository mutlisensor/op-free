# Get your own copy of the OP-Z firmware

> **DRAFT: not ready to publish.** This guide waits for decisions listed at the end ([before this guide is
> published](#before-this-guide-is-published)). Nothing in it is needed to install community engines: those use
> the website alone and never touch the firmware.

Firmware mods (planned, see [firmware mods](../firmware-mods/README.md)) will be small patches applied to **your
own** copy of teenage engineering's official firmware file. OP-FREE does not host firmware, keys, or any file of
teenage engineering's, and never will. This page explains how to back up your OP-Z, where your copy of the firmware
comes from, and what is inside it.

## 1. Back up your OP-Z first

Do this before any firmware work, and keep the backup somewhere safe.

1. **Your music and samples (content mode).** Switch the OP-Z off, then on while holding *track*. It appears on
   your computer as a disk. Copy **the whole disk** to a dated folder: projects, sample packs, bounces and the
   configuration files [manual, p.62–63]. Eject (or press *play*) when you are done; the OP-Z restarts.
2. **Your sound setup (synth mode).** Content mode can't reach the plug list and slot map (`settings/`). Connect
   the OP-Z normally and use *Back up* on the OP-FREE website (or `python3 tools/opz.py backup` from the
   repository), which saves the installed plugs and the slot map over USB MIDI.
3. Optionally, `python3 tools/opz.py projects backup` also reads all ten projects over USB (read-only).
4. Write down which firmware version your OP-Z runs.

Before content mode, make sure every sample of your own sits in a slot: content mode deletes user samples that are
in no slot ([SAFETY.md](../SAFETY.md)).

## 2. Get the official firmware file

The firmware is distributed by teenage engineering as an update file with the extension **`.zfw`**, on the OP-Z
support and download pages of their website.

- Download it **only from teenage engineering**. A file from anywhere else may be modified.
- Keep the file **exactly as downloaded**, with its version in the name, next to your backup. Don't rename it inside
  a ZIP or open and re-save it: mods check the file's SHA-256 hash and refuse anything that differs from the
  version they were made for.
- The research and the planned mods target **firmware 1.2.45**. If teenage engineering no longer offers that
  version, a mod made for it can't be applied to yours: it has to be ported and checked against the new version.

The same file is also your **undo**: installing it in update mode puts teenage engineering's OS and factory
content back. To install it: switch the OP-Z on while holding *screen*, copy the `.zfw` onto the disk that
appears, eject, and don't switch the OP-Z off until it has restarted [manual, p.63]. Don't type anything into the
serial device the OP-Z shows in update mode: it is a root console that can erase the unit
([SAFETY.md](../SAFETY.md), rule 1).

## 3. What is inside

At a high level the `.zfw` is an **encrypted ZIP archive** holding the OS image (`os.ldr`), the bootloader, the
firmware of the Bluetooth and keyboard chips, and the factory content (engines, sample packs, presets). The OP-Z
unpacks it in update mode; the only integrity check during install is each ZIP entry's checksum
([how the firmware works](how-the-firmware-works.md#the-update-package), [research](../research/README.md)).

## 4. How the firmware builder will use your file

When firmware mods exist, the builder on the website will:

1. ask you to choose your own `.zfw` file; it is read **in your browser** and never uploaded anywhere;
2. check its SHA-256 against the version the chosen mods were made for, and stop if it doesn't match;
3. apply each mod's patch records (each one says where, what bytes it expects there, what it writes, and why it
   is safe; see [propose a firmware mod](propose-a-firmware-mod.md));
4. give you the modified update file to save, with the steps to install it and to go back.

## Before this guide is published

Open decisions for the project's maintainer (this section goes when they are made):

- [ ] **How much of the decryption method is published**: nothing, a description, or tooling. The builder in
      step 4 needs to read inside the encrypted file in your browser; whether the site ships that ability, asks
      you to supply something, or waits, follows from this decision. No key, and nothing that says where a key
      can be found, goes into this repository in any case.
- [ ] **Where users get the file**: link to teenage engineering's download page, or describe it only; what to do
      if 1.2.45 is no longer offered.
- [ ] **Whether this guide needs a "read your installed firmware" route** (from the unit itself) or only the
      downloaded file.
- [ ] **Legal review** of publishing firmware research and a patcher in the user's jurisdiction, and of the
      wording above.
- [ ] **Recovery proven**: firmware mods stay unpublished until reinstalling the official file after a deliberately
      bad image is proven on a spare unit.
