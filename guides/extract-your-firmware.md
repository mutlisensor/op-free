# Get your own copy of the OP-Z firmware

> **DRAFT: not ready to publish.** Waiting on the items at the end ([before this guide is
> published](#before-this-guide-is-published)). Nothing in it is needed to install community engines: those use
> the website alone and never touch the firmware.

Firmware mods (planned, see [firmware mods](../firmware-mods/README.md)) will be small patches applied to **your
own** copy of teenage engineering's firmware. OP-FREE does not host firmware, keys, or any file of teenage
engineering's, and never will. This page explains how to back up your OP-Z, how to read the OS off your own unit
(no key needed), where the official update file comes from, and what is inside it.

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

## 3. Read the OS off your OP-Z

Your OP-Z stores its OS unencrypted, as `/yaffs2/os.ldr`. In update mode the bootloader's serial console can send
any file back whole, so you can read your installed OS with **no key** [proven: hardware check H-U1, 2026-11-02, in
[the update console](../research/update-console.md#8-hardware-check-h-u1-read-osldr-through-the-console-passed-2026-11-02)].
This is the copy firmware mods start from.

The same console has commands that erase the unit with a single wrong byte. **Use only `tools/opz_os_read.py`,
never a serial terminal.** The tool can send exactly five fixed frames (a ping, two read-only status queries, and a
read of `os.ldr` or `os.crc32`) and refuses any other byte string; `opz_os_read.py frames` prints all five.

You need Python 3 and pyserial (`python3 -m pip install pyserial`). Then:

1. **Check offline:** `python3 tools/opz_os_read.py frames`, then `python3 tools/opz_os_read.py read os.ldr
   --dry-run`. Both print frames and open no port.
2. **Update mode:** close teenage engineering's app, the OP-FREE site and any other serial tool. Switch the OP-Z
   off, then on while holding *screen*. A disk called "OP-Z Boot" and a serial port appear. Copy nothing onto the
   disk.
3. `python3 tools/opz_os_read.py ping`: expect a pong. If not, stop and start again from step 2.
4. `python3 tools/opz_os_read.py list`: the listing should show `/yaffs2/os.ldr: 828136` for 1.2.45. Another size
   means another OS version: the mods for 1.2.45 won't apply.
5. `python3 tools/opz_os_read.py read os.ldr`: about two seconds. The tool saves the file to `dumps/os-read-<time>/`
   (git-ignored) and compares it with 1.2.45 (SHA-256
   `5be99760c8797bdd4797ed20cfb9a92b6588889d3a81109bae5a45646a9bde07`); it exits with an error if it differs. A
   short read saves nothing: run it again.
6. Switch the OP-Z off with its switch, then on normally.

Nothing in these steps writes to the OP-Z [code; on H-U1 the NAND's page-write counter didn't change across the
reads, proven]. Keep `os.ldr` to yourself: it is teenage engineering's code.

## 2. Get the official update file (your undo)

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
  are made for one exact version.
- If 1.2.45 is ever taken down, a mod made for it can't be applied to another version: it has to be ported and
  checked again.

The same file is also your **undo**: installing it in update mode puts teenage engineering's OS and factory
content back. To install it: switch the OP-Z on while holding *screen*, copy the `.zfw` onto the disk that
appears, eject, and don't switch the OP-Z off until it has restarted [manual, p.63]. Don't type anything into the
serial device the OP-Z shows in update mode: it is a root console that can erase the unit
([SAFETY.md](../SAFETY.md), rule 1).

## 4. What is inside the update file

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

## 5. Decrypting the update file (optional, needs your own key)

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
end up in a commit. OP-FREE doesn't ship the key or say where it is, so this is only for people who already have
it. Firmware mods don't need it: they start from the OS you read off your unit in step 3.

## 6. How the firmware builder will use your copy

When firmware mods exist, the builder will:

1. take the `os.ldr` you read off your OP-Z, with step 3's tool or on the website itself, which will read it over
   Web Serial (Chrome and Edge on a computer) with the same five frames;
2. check its SHA-256 against the version the chosen mods were made for, and stop if it doesn't match;
3. apply each mod's patch records (each one says where, what bytes it expects there, what it writes, and why it
   is safe; see [propose a firmware mod](propose-a-firmware-mod.md));
4. give you the modified OS with the steps to install it and to go back. How a modified OS gets back onto the unit
   is still open (below).

## Before this guide is published

Decided (2026-11-02): the guide links teenage engineering's download page; the cipher and the package layout are
described; decryption tooling is published without a key (`tools/opz_zfw.py`); firmware mods start from the OS
read off the user's own unit, which needs no key; the website may read it over Web Serial. Still open:

- [ ] **Reading in the browser** (decided 2026-11-02: yes, over Web Serial): the website's reader, with the same
      five fixed frames as `opz_os_read.py` and the same refusal of any other byte string, written and tested
      against a fake console before it is offered.
- [ ] **Putting a modified OS back:** whether the OP-Z accepts a changed `os.ldr` (does the bootloader compare it
      with a stored CRC, [Q-48](../research/OPEN_QUESTIONS.md)), and by which route: an update package (encrypted
      again, or a plain ZIP) or the console's own file upload [code; untested].
- [ ] **Legal review** of publishing the description, the tools and the console research in the maintainer's
      jurisdiction, and of the wording above.
- [ ] **Recovery proven**: firmware mods stay unpublished until reinstalling the official file after a deliberately
      bad image is proven on a spare unit.
