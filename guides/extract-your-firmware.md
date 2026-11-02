# Get your own copy of the OP-Z firmware

> **DRAFT: not ready to publish.** Waiting on the items at the end ([before this guide is
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

teenage engineering offers every OP-Z firmware on its download page:
**<https://teenage.engineering/downloads/op-z>**. The research and the planned mods target **firmware 1.2.45**,
which is listed there (checked 2026-11-02) [proven].

| File | Size | SHA-256 |
|---|---|---|
| `z_firmware_1_2_45.zfw` | 1,491,600 bytes | `ae76c1f3d7a3eb666dc7c29bdfee6dfa49150175929cfff28a57ca9c6f209fd5` |

[proven: the project's copy; its size and date match the file on teenage engineering's server, 2026-11-02]

- Download it **only from teenage engineering**. A file from anywhere else may be modified.
- Check the hash (`shasum -a 256 z_firmware_1_2_45.zfw` on macOS and Linux, `certutil -hashfile
  z_firmware_1_2_45.zfw SHA256` on Windows) and keep the file **exactly as downloaded** next to your backup. Mods
  refuse any file that differs from the version they were made for.
- If 1.2.45 is ever taken down, a mod made for it can't be applied to another version: it has to be ported and
  checked again.

The same file is also your **undo**: installing it in update mode puts teenage engineering's OS and factory
content back. To install it: switch the OP-Z on while holding *screen*, copy the `.zfw` onto the disk that
appears, eject, and don't switch the OP-Z off until it has restarted [manual, p.63]. Don't type anything into the
serial device the OP-Z shows in update mode: it is a root console that can erase the unit
([SAFETY.md](../SAFETY.md), rule 1).

## 3. What is inside

The `.zfw` is an ordinary ZIP archive, encrypted with **AES-256 in CBC mode** [code]:

| Bytes of the `.zfw` | What they hold |
|---|---|
| `0x070`–`0x07F` | the IV (initialisation vector) for the CBC stream |
| `0x080`–`0x083` | the ZIP archive's length in bytes (little-endian) |
| `0x300`–`0x3FF` | a file-name field, the start of the CBC stream |
| from `0x400` to the end | the ZIP archive, the rest of the same CBC stream, padded to a whole 16-byte block |

The key is 256 bits [code]. **OP-FREE does not publish the key, or where it can be found**, and never will.

Inside the ZIP [proven: listed from the 1.2.45 package]:

| Path | What it is |
|---|---|
| `firmware/blackfin/bin/opz.ldr` | the OS image, installed as `/yaffs2/os.ldr` (828,136 bytes in 1.2.45, SHA-256 `5be99760c8797bdd4797ed20cfb9a92b6588889d3a81109bae5a45646a9bde07`) |
| `firmware/blackfin/bin/opz.crc32` | the CRC-32 of `opz.ldr`, in decimal [proven: recomputed] |
| `firmware/blackfin/bin/bootloader.ldr` | the main bootloader |
| `firmware/blackfin/system/content/` | factory content: 13 engines, plug settings and presets, readmes, a sample pack |
| `firmware/psoc_keyboard/bin/` | the keyboard chip's firmware (two hardware revisions) |
| `firmware/nordic/bin/` | the Bluetooth chip's firmware |
| `firmware/versions.txt`, `*.version` | version strings |

In update mode the OP-Z unpacks the archive onto its internal storage. The only integrity check during install is
each ZIP entry's CRC: there is no signature [code]. Whether the bootloader also checks `os.ldr` against
`opz.crc32` at every start is open ([Q-48](../research/OPEN_QUESTIONS.md)).

## 4. Decrypting your copy

`tools/opz_zfw.py` decrypts your own `.zfw` on your computer, offline, with Python 3 alone (no packages to
install). It checks the file's SHA-256 against the table in step 2, decrypts it with a key **you supply**
(`--key-file`: 32 bytes, or 64 hex digits on the first line of a text file), checks every ZIP entry's CRC and the
SHA-256 of `opz.ldr`, and writes the ZIP to a folder you choose. It stops at the first check that fails, and it
never prints the key or talks to an OP-Z.

```bash
python3 tools/opz_zfw.py z_firmware_1_2_45.zfw --info
python3 tools/opz_zfw.py z_firmware_1_2_45.zfw --key-file my-key.txt --check
python3 tools/opz_zfw.py z_firmware_1_2_45.zfw --key-file my-key.txt --out ~/opz-firmware
```

The folder must be outside any git repository, or inside one's git-ignored `build/` folder, so the firmware can't
end up in a commit. The firmware builder on the website (planned) will do the same in your browser with the Web
Crypto API; your file and key never leave your computer. Neither ships a key.

## 5. Reading the firmware off your OP-Z (not possible yet)

Your unit holds the OS unencrypted, as `/yaffs2/os.ldr` on its internal storage, so reading it from the unit would
need no key at all. No route is known yet:

- the normal-mode file service serves only a short list of files, and the OS is not on it [notes, proven];
- the update-mode disk holds logs and system information (`systeminfo/`) [code], not the OS [inferred];
- the update-mode serial console can read the SPI flash, where the bootloader lives [proven], but the OS sits on
  the NAND, and no console command that reads a NAND file is known. The same console has one-byte commands that
  erase the unit, so no OP-FREE tool will use it until a read command has been found in the bootloader's code and
  tested read-only.

## 6. How the firmware builder will use your file

When firmware mods exist, the builder on the website will:

1. ask you to choose your own `.zfw` file (and your key, see step 4); both are read **in your browser** and never
   uploaded;
2. check its SHA-256 against the version the chosen mods were made for, and stop if it doesn't match;
3. apply each mod's patch records (each one says where, what bytes it expects there, what it writes, and why it
   is safe; see [propose a firmware mod](propose-a-firmware-mod.md));
4. give you the modified update file to save, with the steps to install it and to go back.

## Before this guide is published

Decided (2026-11-02): the guide links teenage engineering's download page; the cipher and the package layout are
described; tooling is published, without a key. Still open:

- [ ] **The key.** The tool and the builder need a key that OP-FREE won't publish or point to. Decide whether users
      are expected to find it themselves, or whether the guide waits for the route in step 5.
- [x] **The tool** (`tools/opz_zfw.py`) written, tested against the 1.2.45 hashes, and added to the sync
      (2026-11-02).
- [ ] **Packaging**: whether the OP-Z installs a modified package only if it is encrypted again, or also as a plain
      ZIP (the boot-time `firmware_auto.zip` path) [notes; open].
- [ ] **Legal review** of publishing the description and the tool in the maintainer's jurisdiction, and of the
      wording above.
- [ ] **Recovery proven**: firmware mods stay unpublished until reinstalling the official file after a deliberately
      bad image is proven on a spare unit.
