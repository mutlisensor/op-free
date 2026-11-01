# DDR memory map and memory slots above 89 (Q-16)

Findings for **Q-16**: what lies past `0x81C09000` (the end of slot 89), whether engines can use slots above 89 or
other memory, and how the OS loads engines that share a slot. Offline work on the device image `opz.ldr` of
firmware 1.2.45 and TE's 13 engine files. No hardware. Decompiled TE code stays in `build/` and the session
scratchpad (git-ignored); this page gives our description and the addresses to check it.

Evidence labels: **[code]** = `opz.ldr` with the Blackfin+ extension in Ghidra (`tools/ghidra/os_project.sh`), plus
raw scans of every loaded LDR block for 48-bit immediate loads (`0xDA0r hi lo`), 16-bit half loads (`0xE1..`) and
aligned data words. **[code, absence]** = no reference found by those scans; it rules out constant addresses, not
pointers computed at run time. **[manual]** = TE's user guide, page given.

## Answer

- **The loader takes any slot from 0 to 255 and has no upper bound check.** The 5 KB class formula simply goes on:
  slot *n* ≥ 58 is `0x81BE1000 + (n − 58) × 0x1400`, so slots 90–255 cover `0x81C09000`–`0x81CD8800` (166 slots,
  830 KB) [code, `FUN_800fb556`]. `memoryslot` is cut to **8 bits** when `plugs.json` is parsed, so 256 lands on
  slot 0, 300 on slot 44, and so on [code, `0x800ff81c`].
- **Nothing in the OS or in TE's engines refers to memory between the end of slot 89 and the sample cache**
  (`0x81C09000`–`0x826FFFFF`, 11 MB) [code, absence]. The only code that touches it is the content-mode USB disk,
  which also covers slots 0–89 and runs only in content mode, which ends with a restart [code, manual p.61].
- So slots 90–255 are **very likely safe** [code + inferred], but not proven: a pointer computed at run time cannot
  be ruled out statically. Hardware test H-16 below settles it.
- **Two engines in the same slot are never safe**, even if they are never used at the same time: the OS loads
  **every engine listed in `plugs.json` at boot**, not when a track picks it [code]. See
  [Loading](#when-engines-load-and-what-two-engines-in-one-slot-do).

## DDR map

DDR is 64 MB at `0x80000000`–`0x83FFFFFF`: the OS's address check accepts DDR addresses only below `0x84000000`
[code, `FUN_8002834a`, constant at `0x80028394`]. The cache table at `0x815C97C0` maps two 64 MB pages from
`0x80000000`, which looks like a generic board template; the address check is the stronger evidence.

| Range | Size | What | Evidence |
|---|---|---|---|
| `0x80000000`–`0x8014206B` | 1.3 MB | OS code and initialised data | [code] LDR blocks |
| `0x8014206C`–`0x80BFFFFB` | 10.7 MB | **The C heap** (`malloc`). One heap only: the heap table at `0x815C9888` holds one entry `{0x8014206C, 0xABDF90, 0}`, installed on the first allocation (`FUN_80137d1c` → `FUN_80137fbc`). The engine loader reads each `.engine` file into a heap buffer (`FUN_80138134`) | [code] |
| `0x80C00000`–`0x80FFFFFF` | 4 MB | no reference found | [code, absence] |
| `0x81000000`–`0x815CAE9F` | 5.8 MB | OS data and zeroed data: the 256 KB `plugs.json` read buffer at `0x81019260`, the FX pools at `0x8112B220`–`0x8156F21F` (4.27 MB = FX1 2 MB, FX2 256 KB, tape 2 MB, master 16 KB; base `0x8112B21C`, Q-07), the heap table, the cache table | [code] |
| `0x815CAEA0`–`0x81AEF033` | 5.1 MB | zeroed data (the image's last fill block): large OS buffers, among them ~2.3 MB from `0x815CAF6C` and ~1 MB from `0x819D6EFC` | [code] LDR block; contents not traced |
| `0x81AEF034`–`0x81AFFFFF` | 68 KB | gap; no slot number reaches it | [code] |
| `0x81B00000`–`0x81C08FFF` | 1 MB | **Engine slots 0–89** | [code, `FUN_800fb556`] |
| `0x81C09000`–`0x81CD87FF` | 830 KB | **Slots 90–255**: the 5 KB formula continues; nothing else refers here | [code] + [code, absence] |
| `0x81CD8800`–`0x826FFFFF` | 10.2 MB | no reference found; no slot number reaches it | [code, absence] |
| `0x82700000`–`0x8352193F` | 14.1 MB | **Sample-pack cache**: 14 buffers of `0x102660` B (1,058,400 B = 529,200 16-bit samples = 12.0 s at 44.1 kHz), set up at boot (`FUN_80105010`) | [code] |
| `0x83521940`–`0x83FFFFFF` | 10.9 MB | no reference found | [code, absence] |
| `0x81B00000`–`0x83BFFFFF` | 33 MB | **Content mode only**: the USB disk in RAM, overlapping everything above from slot 0 up | [code], see below |

The system stack is in L1 (`SP = 0x11801FFC` at `0x11a00034`) [code].

### Sample-pack cache [code]

`SampleLoader` setup (`FUN_80105094` → `FUN_80105010`) gives each of 14 cache entries (records of `0x48` B at
`0x0802DE3C`) a buffer at `0x82700000 + i × 0x102660`. The pack loader (`FUN_80105e94`) rejects a pack longer
than `0x81330` samples ("Samplepack is too big"), and caps synth-sampler packs at `0x40998` samples (6.0 s). The
cache manager (`FUN_80106e2c`) holds at most 14 packs and unloads one "to give room" for the next. Content mode
limits the total of imported sample data to 24 MB (`FUN_8003377c`: running total + one pack < `0x1800000`), as the
manual says [manual p.62–63].

### Content-mode USB disk [code]

The word at `0x81004528` holds `0x81B00000`, the base of a RAM disk. The SCSI capacity handler (`0x11a00a54`)
reports `0x10800` sectors of 512 B (33 MB, so the disk ends at `0x83C00000`); the read and write handlers
(`0x11a00a80`, `0x11a00ad8`) copy sectors to and from that base. Content-mode setup (`FUN_8003760c`) formats it
as FAT ("create and mount FAT", `FUN_8002ede0`, size `0x2100000`) and copies projects, bounces and sample packs
into it. The disk overwrites slots 0–89 and TE's own engines just as much as slots 90–255. Ejecting restarts the
OP-Z [manual p.61], and every boot reloads the engines from flash, so this does not make high slots any riskier
than the ones TE uses [inferred].

### What the scans covered

- Every 48-bit immediate load in every loaded block of `opz.ldr` (code in DDR, L1 and L2): the only values in
  `0x81AEF034`–`0x83FFFFFF` are the six slot constants of `FUN_800fb556`, `0x82700000` (sample cache), and the
  numbers `0x81DA9A67`, `0x81DA1ED1` (inside an L2 data table, not code). `0x84000000` appears once, in the
  address check.
- 16-bit half loads with a high half in `0x80C0`–`0x87FF`: only small zero-extended constants (`0x8200`, `0x8444`,
  `0x8371`), none part of an address.
- Aligned data words in every initialised block: values in the range are fixed-point tables (L2 at `0x08011xxx`,
  coefficient tables at `0x81128E20`…) plus the RAM-disk base at `0x81004528` and the cache table entry.
- TE's 13 engines: no immediate load outside their own window except the constants `0x80000000`/`0x80000001`
  (saturation limits) [code, `.engine` files].

## The slot formula and its limits [code]

`FUN_800fb556(slot, &start, &end)` compares the slot (signed) with 8 and 58 and applies one of three formulas
(50 KB from `0x81B00000`, 10 KB from `0x81B64000`, 5 KB from `0x81BE1000`). There is no upper bound and no error
path. The classes are contiguous: slot 7 ends where slot 8 starts (`0x81B64000`), slot 57 where 58 starts
(`0x81BE1000`). There is no fourth, larger class.

The slot reaches this function as a byte:

- **Boot parse of `plugs.json`** (`FUN_800ff454`): `memoryslot` and `version` are read as integers into a
  zero-initialised local (`R4 = 0` at `0x800ff4a0`, stored at `0x800ff5ca`), then only their low bytes are kept
  (`0x800ff81c`–`0x800ff868`) and stored as bytes `+0x1c` (version) and `+0x1d` (slot) of the definition
  (`FUN_800fc980`). A definition without `memoryslot` therefore gets **slot 0**, and 256 + *n* becomes *n*.
- **Patch import** (`FUN_800ff26c`): the same 16-bit `(memoryslot << 8) | version` value.
- **Load** (`FUN_800fff1c`): reads the slot byte at `+0x29` of the map node and calls `FUN_800fb556`.

The loader (`FUN_8002ac00`) then accepts a block only if `start ≤ target` and `target + count ≤ end`, so an engine
must fit its window; it cannot spill into the next slot.

## When engines load, and what two engines in one slot do

- **At boot** the OS init (`FUN_80112cc4`) calls `loadRequestedEngines` (`FUN_801001c4` → `FUN_800fff1c`) right
  before the sample cache setup. `FUN_800fff1c` walks **every engine definition** read from `plugs.json` (a map
  keyed by path string, so in byte-wise alphabetical order of the path), and loads each one that is not already
  loaded (keyed by a hash of the path) [code]. There is no check that a plug uses the engine, and no check that another
  engine already sits in the slot. The app's `PlugManager::loadRequestedEngines` (`0x45744`) does the same.
- **After a patch import** (`FUN_801001d8`, end of import) the same function runs again and loads only
  definitions whose path is new. This is why a brand-new engine plays without a restart and a replaced one needs
  a restart [proven, 2026-10-17].
- **Choosing the plug on a track** only calls the engine's `init`; it does not load anything.

Consequences:

1. **Two definitions with the same slot both load at boot.** The one whose path sorts later overwrites the
   other's code and data; the earlier one keeps the function pointers it looked up when its own code was there.
   Playing it then jumps into the other engine's code: a crash or garbage. Slot sharing is unsafe even if the two
   plugs are never on a track at the same time.
2. **Removing a plug does not free its slot.** `opz.py remove` leaves the engine definition in `plugs.json` (the
   "orphan engines" line), and orphan definitions still load at boot. We know of no way to delete a definition,
   so treat every slot we have ever installed into as taken for good on that unit.
3. Installing a new engine into a slot whose engine is loaded overwrites the running code at once. `opz.py`
   already refuses a slot that another path holds.

## Reclaimable memory

| Candidate | Verdict |
|---|---|
| **50 KB slot 0** | Memory-wise identical to slots 1–7 [code]. Its only hazard: a definition without `memoryslot`, or with `memoryslot` 256, also lands there [code, `0x800ff4a0`, `0x800ff81c`]. TE's definitions all carry a slot; our tools always write one. Usable as the last 50 KB slot |
| **Slots 90–255** | 166 × 5 KB, nothing else refers to them [code, absence]. Very likely safe; prove with H-16 first |
| **Slots ≥ 256** | Never: they alias slots 0–255 |
| Class boundaries, a larger class | None: the classes are contiguous and there are only three |
| The 68 KB gap below slot 0 | Not reachable by any slot |
| Sharing a slot between engines | Never (above) |
| **Run-time scratch** in `0x81CD8800`–`0x826FFFFF` (10 MB, past slot 255) | [idea] An engine could keep large buffers (string pools, delay lines, computed tables) at a fixed address there, outside its window, as long as the linker emits no load or fill block for them (the loader would reject it) and `init` clears them. This would let today's 50 KB engines fit 10 KB slots. Needs an SDK `NOLOAD` section, a project-wide address table, and the canary part of H-16 |

## Hardware test H-16: prove a slot above 89

**For the lead to run with the user later. Not run.** It follows [SAFETY.md](../SAFETY.md): back up first,
`opz_verify` every engine, disposable project, verbose log, one change per step.

**Test engine `opz_test_probe`** (to be written; proposed plug id 60903, 5 KB class, slot 90; a second build
`opz_test_probe255`, plug id 60904, slot 255, window `0x81CD7400`–`0x81CD8800`). Both slots stay taken for good
afterwards (orphan definitions load at boot), which is fine for test slots.

- `initModule`: records a checksum of its own loaded code and constants. Touches nothing outside its window, so
  booting only does what the loader does anyway.
- `init` (a track picks the plug): fills its own unused `.bss` tail up to the end of the window with an
  address-derived pattern (for example `addr × 0x9E3779B1`). Build option `PROBE_SCRATCH`: also fills 64 KB at
  `0x82000000` (the run-time scratch idea).
- `render`: each block re-checks a slice of the pattern and of the code checksum (a full sweep every few hundred
  blocks keeps it cheap). **Read-back is audible**: a plain sine at the played key while everything matches; on
  the first mismatch it latches an alarm, a 2-tone siren at a fixed pitch whatever key is played. The note-off
  path stays normal so the alarm stops with the note.

**Steps**

1. Offline: build both variants (needs a test-only override of `MAX_SLOT`/`MAX_CHECKED_SLOT` in `opz.py`,
   `opz_verify.py` and the web app), run `opz_verify` and the simulator tests, then `opz.py --fake install`.
2. Install `opz_test_probe` (slot 90) on the lead track of a disposable project. Pull `plugs.json` and check that
   the definition reads back `memoryslot: 90`. Check the log: no `Bad address`, `Could not load engine`.
3. Play a held note: the sine must sound at the right pitch.
4. Keep it playing in a loop for ~10 min while you: load different sample packs on all four drum tracks and the
   synth sampler (fills the 14-pack cache at `0x82700000`), switch projects and back, sample from the mic, run long
   delays and reverbs on FX1 and FX2 (FX pools), and run `opz.py backup` (file service). No alarm, no glitch.
5. Power-cycle. Boot must be clean; play again: sine, no alarm.
6. Repeat 2–5 with `opz_test_probe255`.
7. Optional, for the scratch idea: install a `PROBE_SCRATCH` build (another path and plug id) and repeat 3–5.

**Pass:** no alarm, no crash, no loader errors, for both slots. Then slots 90–255 become [proven]: raise
`MAX_SLOT` and `MAX_CHECKED_SLOT` to **255, never more**, and open the range in
[memory-slots.md](../sdk/docs/memory-slots.md). **Fail:** note which activity raised the alarm; that memory is in use;
stay at or below 89. **Crash at boot** (should not happen, since the probe writes only after a track picks it):
the recovery ladder in SAFETY.md.
