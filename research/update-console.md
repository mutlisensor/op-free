# The update-mode console: what each command does, and reading `os.ldr` without a key

Research of 2026-11-02: the code offline, then hardware check H-U1 on the user's OP-Z the same day (§8; the user
approved running it on the main unit, an exception to firmware/CLAUDE.md's spare-unit rule, read-only frames only). Question Q-52: is there a route that reads
the installed OS (`/yaffs2/os.ldr`, unencrypted on the NAND) off an OP-Z, so that a firmware builder never needs the
update's decryption key? Sources: the bootloader read off the user's OP-Z's SPI flash in October
(`dumps/bootloader.ldr`, 257,596 bytes, build date "Mar 24 2020"), and for comparison the bootloader in the 1.2.45
update (`decrypted/…/bootloader.ldr`, version 1.1.35, "Jan 26 2021"), both read with Ghidra and the Blackfin+
extension (ghidra-setup.md). Addresses are device addresses in the device's own bootloader unless
marked 1.1.35. Evidence labels as in [CONTRIBUTING.md](../CONTRIBUTING.md). No TE code is quoted.

## The short answer

1. **Yes.** Console command **`0x19` reads a file from the yaffs2 file system and sends it back whole**. Its
   payload is a file name ending in a zero byte; the bootloader puts `/yaffs2/` in front, opens the file read-only,
   and answers with one frame of type `0x20` whose length is the file size and whose payload is the file [code,
   `0x11a02c04`–`0x11a02d44`; **proven 2026-11-02**: `os.ldr` read off the user's OP-Z in 2 s, byte-identical to
   1.2.45's `opz.ldr`, §8]. `os.ldr` + zero byte → `/yaffs2/os.ldr`. No key, no decryption: the OS is stored
   in the clear [notes, and the bootloader loads it with no decryption step, `nand_boot.cpp`].
2. **What `0x19` does to the file system is what every normal boot already does:** mount yaffs2 (once per update-mode
   session), open `os.ldr` read-only, read it, close it. The normal boot then unmounts; `0x19` instead flushes the
   cache without writing a checkpoint, and the unmount follows when update mode is left by the power-off or reset
   commands [code, see §4]. So a read adds no file-system write path that a normal power-on does not have
   [inferred from the code paths; on the device the page-write counter did not move across the reads, proven §8].
3. **A tool limited to two frame types, `0x00` (ping) and `0x19` (read file), with a fixed list of file names,
   can read `os.ldr` safely** in the sense of SAFETY.md rule 1: neither command can write. Two read-only helpers
   are worth allowing for the check: `0x21` (a file listing with sizes) and `0x27` (yaffs2 statistics and NAND
   geometry). The check and its tool (`tools/opz_os_read.py`) are in §8.
4. **No command reads raw NAND pages.** The only other read path is `0x17` (SPI flash). §7 says what a raw-page
   rebuild would need; with `0x19` it is not needed.
5. **The newer bootloader (1.1.35) has the same console.** Its L1 code differs from the device's only by relocated
   addresses and by the line numbers in log calls of the upgrade code; the parser and the command handler are the
   same [code: byte comparison of the two L1 overlays, §6].

## 1. Where the console lives

The console is the bootloader's second stage, copied to L1 instruction memory at boot (Ghidra's overlay `block8`,
`0x11a00180`–`0x11a04f5b`). The original Ghidra project had not disassembled it, which is why the string
`serial_command_handler.cpp` showed no references; creating functions at every `LINK` instruction in that overlay
finds 35 functions, among them [code]:

| Address | What it is |
|---|---|
| `0x11a03c4e` | per received byte: `0xC0` starts a frame, `0x7D` escapes the next byte (XOR `0x20`), then the byte goes to the parser, or straight to the command handler while a command is collecting its payload |
| `0x11a025b0` | the frame parser (a state machine; state at `0x11806e84`, the frame fields at `0x82852e64`…`0x82852f03`, cleared on every `0xC0`) |
| `0x11a02932` | the command handler: one branch per type |
| `0x11a0452c` | sends a reply header (`0xC0`, routing, type, length) |
| `0x11a046ae` | sends payload bytes in pieces of up to 256, escaped by `0x11a0447c` |
| `0x11a00a8c` | mounts yaffs2 once per session (flag `0x82835c62`) |

String addresses are loaded as 32-bit immediates whose two halves are stored high half first (`02 da 83 82 40 5b`
loads `0x82835b40`), so a byte search for that order finds the users of a string when Ghidra shows none.

## 2. Frame format

**To the OP-Z** [code, `0x11a025b0`; the `0x17` form also proven by `dumps/probe.log`]: `C0`, a routing byte, the
type, a 32-bit big-endian length, then the payload. Every byte after `C0` that equals `C0` or `7D` is sent as `7D`,
byte XOR `0x20`.

- **Routing:** the low four bits pick the target: 1 = the Blackfin console, 2 and 3 = pass-through to the keyboard
  and Bluetooth chips (the bytes are forwarded, not interpreted here); anything else is logged as "unexpected
  routing" and the frame is ignored until the next `C0`. Our tools send `0x01`.
- **The length field** is read for every type but only some commands use it (`0x00` echo, `0x10`/`0x11`/`0x31`
  uploads). Commands with a name end at the name's zero byte; commands with fixed fields end after them.
- **Payload by type:**

| Types | Payload after the header |
|---|---|
| `0x00`, `0x02`, `0x07`, `0x15`, `0x27`, `0x35`, `0x39` | none: the command runs **as soon as the header is in** (`0x00` then collects `length` bytes to echo) |
| `0x06`, `0x14`, `0x21`, `0x32`, `0x34` | one argument byte |
| `0x17` | offset (u32, big-endian), length (u32, big-endian) |
| `0x3b` | address (u32, big-endian) |
| `0x10` | address (u32 BE), size (u32 BE), then the data |
| `0x11`, `0x31` | size (u32 BE), then name or data |
| `0x19`, `0x23`, `0x29` | a name, ending in a zero byte |
| `0x37` | two names, each ending in a zero byte |

The parser stores names at `0x82852e74` (second name at `0x82852eb4`) **with no length check**: a name over
63 bytes runs into the following fields (SPI offset and length, the argument byte, the upload buffer's count)
[code]. A tool must keep names short.

**From the OP-Z** [code, `0x11a0452c`; proven for `0x18` by `dumps/probe.log`]: `C0`, routing `0x1E`, type + 1,
length (u32 BE), payload. **The bootloader escapes the header too**, not only the payload, so a length byte of
`C0` or `7D` arrives escaped; `opz_spi_read.py`'s reader un-escapes only the payload and must be changed for
lengths that contain those bytes (`os.ldr`'s 828,136 = `00 0C A2 E8` does not). Log lines arrive in between as
type `0x04` frames. A second header form with a sequence byte and a checksum exists behind a flag (`+0x13e0` of the
port object); the probe's replies show it is off [proven].

## 3. The commands

All types the parser accepts, read from the handler [code]. "Writes" means flash or file-system writes.

| Type | Reply | What it does | Writes? |
|---|---|---|---|
| `0x00` | `0x01` | ping; with a length, echoes that many payload bytes (into a 2 KB buffer, unchecked) | no |
| `0x01` | none | accepted, no branch in the handler | no |
| `0x02` | `0x03` | text: DSP and keyboard hardware revisions, firmware, OS, bootloader (with build date), keyboard and BLE versions, module type and version | no |
| `0x06` | none | with argument `0x14`: unmount, then power off; other arguments do nothing | unmount (see §4) |
| `0x07` | `0x08` | text: serial number and hardware revision, from the factory record in SPI flash and OTP | no |
| `0x10` | `0x13` | write the uploaded data to SPI flash at the address (4 KB pieces) | **SPI flash** |
| `0x11` | `0x13` | upload a file: creates the name's folders, opens `/yaffs2/<name>` with `w+b`, writes the data | **NAND** |
| `0x14` | `0x16` | format SPI flash (argument `0x33` or `0x99`) | **SPI flash** |
| `0x15` | `0x16` | unmount and format the NAND; no payload, runs on the header alone | **NAND, all of it** |
| `0x17` | `0x18` | read SPI flash: `length` bytes from `offset` (SPI command `0x03`, 24-bit address, `0x8281e98e`) | no |
| `0x19` | `0x20` | read the file `/yaffs2/<name>` (§4) | no |
| `0x21` | `0x22` | file listing of `/yaffs2/`, recursive, up to 32 KB of text (§5) | no |
| `0x23` | `0x24` | firmware upgrade from the named file (or `force`) | **NAND, OS** |
| `0x27` | `0x28` | yaffs2 statistics and parameters of `/yaffs2` as text (§5) | no |
| `0x29` | `0x30` | delete `/yaffs2/<name>` | **NAND** |
| `0x31` | `0x13` | load the data into RAM at `0x82000000` and mark it to run | runs code |
| `0x32` | `0x33` | factory data: argument `0x34` the Blackfin's, `0x87` the keyboard chip's, `0x67` the BLE chip's (sends a command to that chip) | no [inferred: it sends command 7 to the other chip; not read] |
| `0x34` | none | reset (argument `0x78` → reboot reason 0x14, `0x96` → reason 10), after an unmount | unmount |
| `0x35` | `0x36` | text: OS, bootloader, keyboard and BLE versions and four words from a boot-ROM call (`0x0400004c` with 0x13) | no |
| `0x37` | `0x38` | rename `/yaffs2/<name1>` to `/yaffs2/<name2>` | **NAND** |
| `0x39` | `0x3a` | returns the 32-bit word at `0x8283dfa0` (0 unless `0x3b` ran) | no |
| `0x3b` | `0x3c` | stores the address at `0x8283dfa0` and hands a record built from it to `0x82827e4c` (not read) | unknown: treat as unsafe |

The notes' read-only list (`0x00 0x02 0x07 0x17 0x19 0x21 0x27 0x35 0x39`, [OPZ_FIRMWARE_MAP.md
§8](OPZ_FIRMWARE_MAP.md)) matches the code. Two warnings the list does not carry: `0x15` needs no payload, so a
single wrong type byte formats the NAND; and `0x21` with its CRC flag reads every file on the NAND (slow, but still
read-only).

## 4. `0x19` in detail

What the handler does, in order [code, `0x11a02c04`–`0x11a02d44`]:

1. Mounts yaffs2 if this update-mode session has not yet (`0x11a00a8c` → `0x8281da12` → `0x82820668`, which mounts
   `/yaffs2` read-write; the NAND's on-die ECC must be on or it stops).
2. Builds the path with `"%s%s"` from `/yaffs2/` and the received name, into a 256-byte buffer.
3. Opens it through `0x82822516`, which maps the mode string `rb` to read-only flags (0) and permission 0x100.
   **If the open fails, no reply is sent at all**; the tool sees silence.
4. Gets the size by seeking to the end, then sends the reply header: type `0x20`, length = file size.
5. Reads the file in 2 KB pieces (`0x828224f2`) and sends each piece (escaped, in 256-byte writes); the LEDs show a
   progress bar (`0x11a0210c`, 16 steps).
6. Closes the file and calls yaffs2's sync on `/yaffs2` with the checkpoint flag off (`0x8280e3be` →
   `0x8280e316`): it flushes the write cache, which a read leaves clean, and writes no checkpoint.

Quirks, all harmless for a reader that checks the result:

- The seek inside the loop passes the running offset as the "whence" argument and 0 as the offset (`0x11a02cba`).
  The first one rewinds; the later ones fail with an invalid-argument error and change nothing, and the reads carry on
  in sequence. [code]
- Read errors are not checked; a failed read would still send 2 KB (stale buffer contents). The hash comparison
  catches that. [code]
- The header's length field is ignored; the name ends at its zero byte. Set the length to the name's length + 1.

**Compared with a normal boot.** `nand_boot.cpp` mounts the same way (the same flag and functions), opens
`/yaffs2/os.ldr` read-only, reads it, then unmounts (`0x8281da68`, in the jump-to-OS path at `0x828010fa`) [code].
Leaving update mode through `0x06`/`0x14` (power off) or `0x34` (reset) also unmounts first (`0x828202f0`) [code].
yaffs2 writes during mount (a scan may erase blocks found fully deleted) and during unmount (it saves a checkpoint):
both happen at every normal power-on too [inferred: yaffs2 behaviour, the mount and unmount calls are the boot's
own]. Switching the unit off without either command leaves the NAND unmounted without a checkpoint; the next boot
then scans instead of restoring, which is slower but normal for yaffs2 [inferred].

So a `0x19` read of `os.ldr` stays inside what the bootloader does on every boot. The real risk is the frame
itself: a wrong type byte can format the NAND. A tool must make that impossible by construction (§8).

## 5. `0x21` and `0x27`: cheap checks before the read

**`0x21`**, argument byte bit 0 = add CRC, bit 1 = add size [code, `0x828225e8`]: walks `/yaffs2/` with
opendir/readdir/stat; a folder is printed as `/yaffs2/<path>/` and walked; a file as `/yaffs2/<path>`, then with
flags `: `, the size (`%u `) and `CRC: 0x%08x`, then a newline. The text is capped at 32 KB and then ends in
`.....`. **Use argument `0x02` (sizes only):** `/yaffs2/os.ldr: 828136` shows the file is there and has the 1.2.45
size before 828 KB are moved. Argument `0x01` reads every file on the NAND to checksum it.

**`0x27`** [code, `0x8280f410`]: the yaffs2 statistics block for `/yaffs2` (page reads and writes, erases, ECC
fixes, garbage collection, block and chunk counts) and its parameters: bytes per chunk, chunks per block, spare
bytes, start and end block, yaffs2 mode, in-band tags. It is a formatted copy of the device structure; nothing is
read from flash for it. Worth saving once: it is the NAND geometry a raw-page reader would need (§7), and the
before/after page-write counts show whether the session wrote anything.

## 6. The two bootloader versions

The device runs the bootloader from March 2020 (read off its SPI flash, [proven: `dumps/`]); the 1.2.45 update
carries 1.1.35 from January 2021, which the update evidently did not install on this unit [inferred from the dump's
build date]. Other units may run either. Compared block by block after loading both in Ghidra: the L1 overlay that
holds the console is the same size; 430 bytes differ, and all but 9 of them are 32-bit addresses moved by up to
0x30 (the main code block grew by 48 bytes). The 9 others are small immediates in log calls of the upgrade code
(source line numbers +8) and one offset at `0x11a00d28`, none inside the parser or the handler [code]. So the
command set and the `0x19` behaviour are the same on both versions.

## 7. Raw NAND pages

No console command reads NAND pages: the only flash reads are `0x17` (SPI) and the file reads `0x19`/`0x21`
[code: every handler branch]. If raw pages were all one could get, rebuilding `os.ldr` would need the geometry from
`0x27` (chunk size, chunks per block, spare size, block range, in-band tags or not), the packed-tags layout in the
spare area (yaffs2 `packedtags2`: sequence number, object id, chunk id, byte count, with or without tag ECC) and the
chip's on-die ECC layout; then a scan that keeps the newest chunk per object and chunk id by block sequence
number, finds the object header named `os.ldr` in the root folder, and joins its data chunks up to the header's
file size. That is what yaffs2 extractors do [idea]. With `0x19` none of it is needed.

## 8. Hardware check H-U1: read `os.ldr` through the console (passed 2026-11-02)

Read-only. For the device session only, with the user's approval per step (SAFETY.md). Nothing in this check writes
to the OP-Z; the mount and unmount are the bootloader's own (§4).

**Tool:** [`tools/opz_os_read.py`](../tools/opz_os_read.py) (tests: `tools/tests/test_opz_os_read.py`,
against a fake console). Needs pyserial on the device machine (`python3 -m pip install pyserial`).

- It can send exactly five frames, built once at import, and `send()` refuses any other byte string:
  `C0 01 00 00000000` (ping); `C0 01 27 00000000` (statistics); `C0 01 21 00000001 02` (listing with sizes);
  `C0 01 19 00000007 "os.ldr" 00` and `C0 01 19 00000009 "os.crc32" 00` (read). `opz_os_read.py frames` prints
  them. The encoder refuses any type outside `{0x00, 0x19, 0x21, 0x27}`, any other name and any other `0x21`
  argument; a test tries all 252 other type bytes.
- Each command first sends a warm-up ping, clears the input, and sends nothing else unless the second ping is
  answered.
- The reader un-escapes header and payload, keeps type `0x04` log lines apart, and accepts only the expected reply
  type from routing `0x1E`.
- `read`: no reply within 5 s means the file is missing; 3 s of silence mid-file is a short read and nothing is
  saved. A complete file is saved to `dumps/os-read-<UTC time>/` (git-ignored, as is `*.ldr`) and compared with
  the values below; the exit code is 1 when it differs.
- `--dry-run` prints the frames and opens no port.

**Expected values (1.2.45):** `opz.ldr` in the update is 828,136 bytes, CRC-32 2403806141 (`0x8F472BBD`, the same
number as the update's `opz.crc32`), SHA-256
`5be99760c8797bdd4797ed20cfb9a92b6588889d3a81109bae5a45646a9bde07` [proven: computed from the decrypted update].
The upgrade copies it to `/yaffs2/os.ldr` with its CRC as `/yaffs2/os.crc32` [inferred: the upgrade code's paths
`/yaffs2/upgrade/firmware/blackfin/bin/opz.ldr` and `/yaffs2/os.crc32`; not read in detail].

| Step | Do | Expect | If not |
|---|---|---|---|
| 0 | Offline: `python3 tools/opz_os_read.py frames`, then `read os.ldr --dry-run`; check the frames against the list above | the five frames, nothing else | stop, fix the tool |
| 1 | OP-Z off. TE's app, the web app and other serial tools closed. Hold SCREEN while switching on (update mode). Copy nothing to the update disk | "OP-Z Boot" serial port and the update disk appear | stop |
| 2 | `opz_os_read.py ping` (the first frame after opening the port may be lost; the tool sends one warm-up ping) | pong `C0 1E 01 00000000` | stop; power-cycle into update mode again |
| 3 | `opz_os_read.py stats` (`0x27`; saves `stats.txt`) | yaffs2 parameters and statistics; note `n_page_writes` | save the reply; stop |
| 4 | `opz_os_read.py list` (`0x21`, sizes; saves `listing.txt`) | a line `/yaffs2/os.ldr: 828136` and one for `os.crc32` | if `os.ldr` is missing or another size, stop and report the listing (another OS version is installed) |
| 5 | `opz_os_read.py read os.crc32` | 10 bytes, `2403806141` | report the bytes; continue only if the user agrees |
| 6 | `opz_os_read.py read os.ldr` | 828,136 bytes in a few seconds, LEDs show progress; SHA-256 as above | a short read or another hash: keep the file, report; read it once more to tell a transfer error from a different OS |
| 7 | `opz_os_read.py stats` again | `n_page_writes` unchanged from step 3 | report the difference (a write we did not expect) |
| 8 | Switch the OP-Z off with its switch, then on normally | boots as before | crash-recovery.md |

Report: the step-3/7 statistics, the listing line for `os.ldr`, the size and hashes from step 6. If the hash
matches, Q-52's hardware part is closed: the installed OS can be read off the unit with no key.

**Result (2026-11-02, the user's OP-Z, device bootloader of March 2020): all steps passed** [proven]. Files in
`dumps/os-read-20261102/` (git-ignored).

| Step | Result |
|---|---|
| 0 | the five frames, nothing else |
| 2 | warm-up ping unanswered (as expected after the port opens), second ping answered |
| 3 | statistics as text; `n_page_writes` 163, `n_erasures` 5, no ECC errors, no retired blocks. Geometry: 2,048-byte chunks, 64 per block, blocks 1–4095 (512 MB), `spareBytesPerChunk` 0, `noTagsECC` 1, `useNANDECC` 1, checkpoints on. The counters run from the mount, so the 163 writes are the bootloader's own update-mode start-up (it rotates the logs in `/yaffs2/tmp/`) [inferred], not the statistics command |
| 4 | 300 lines, not cut; `/yaffs2/os.ldr: 828136`, `/yaffs2/os.crc32: 10`; `tmp/` holds `system.log`, `upgrade.log`, `import.log`, `battery.log` |
| 5 | `os.crc32` = `2403806141`, as 1.2.45 |
| 6 | 828,136 bytes in ~2 s; CRC-32 2403806141, SHA-256 `5be99760…9bde07`: **identical to 1.2.45's `opz.ldr`** |
| 7 | `n_page_writes` still 163, `n_erasures` still 5; `n_page_reads` 238 → 1002 (the reads); `n_ecc_fixed` 0 → 1 |
| 8 | the user switched off and on: normal boot |

One page read during steps 4–6 had a bit error that the NAND ECC corrected (`n_ecc_fixed` 1); the data came out
exact. yaffs2 flags such a block for garbage collection, which may move its data to a fresh block on a later write
[inferred: yaffs2's chunk-error handling]. A normal boot reads `os.ldr` whole too, so it meets the same pages.

## 9. What this means for OP-FREE

The read side of a firmware builder needs no key: a user's own OP-Z hands over its installed OS through `0x19`
[proven: H-U1, §8]. The builder must then check that the OS is the version a mod targets (the SHA-256
above for 1.2.45) before applying anything. A browser version would talk to the update-mode serial port through
Web Serial with the same frozen frame list as §8; whether that is acceptable next to a console that formats the NAND
on one wrong byte is a design decision for the OP-FREE side [idea]. The write side (putting a modified OS back) is a
separate question: `0x11` writes files and `0x23` runs the upgrade, and whether the bootloader then accepts a
changed `os.ldr` is Q-48.
