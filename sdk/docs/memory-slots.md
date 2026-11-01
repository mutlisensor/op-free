# Memory slots

Each engine is linked to a fixed address window chosen by its `memoryslot`. The engine area starts right after
the OS image's static memory (which ends at `0x81AEF033`). [code, `FUN_800fb556`]

| Slots | Window | Base address of slot *n* | End of class |
|---|---|---|---|
| 0–7 | 50 KB (`0xC800`) | `0x81B00000 + n·0xC800` | `0x81B64000` |
| 8–57 | 10 KB (`0x2800`) | `0x81B64000 + (n−8)·0x2800` | `0x81BE1000` |
| 58–89 | 5 KB (`0x1400`) | `0x81BE1000 + (n−58)·0x1400` | `0x81C09000` |
| 90–255 | 5 KB (`0x1400`), same formula | `0x81BE1000 + (n−58)·0x1400` | `0x81CD8800` |

The OS keeps `memoryslot` as **one byte**: 256 and up wrap to slot *n* − 256, and a definition without
`memoryslot` gets slot 0 [code, `0x800ff81c`, `0x800ff4a0`]. There is no upper bound check and no fourth class
[code, `FUN_800fb556`]. Slots 90–255 are not cleared for use yet (below). DDR map and evidence:
[memory-map.md](../../research/memory-map.md).

## Slots used by TE

| Class | Used slots (engine) |
|---|---|
| 50 KB | 4 `ep` |
| 10 KB | 9 `tapetwo`, 10 `sampleplayergain`, 13 `fm003b`, 16 `saw013a`, 18 `organ`, 27 `analoga` |
| 5 KB | 59 `chord`, 60 `chorus`, 61 `delay`, 62 `digital`, 66 `reverb`, 68 `synthsampler`, 70 `volt`, 77 `electric`, 78 `dist`, 79 `crush`, 82 `ministringshort`, 83 `cluster`, 88 `chorus2`, 89 `fdnreverb` |

## Free slots

| Class | Free |
|---|---|
| 50 KB | 1, 2, 3, 5, 6, 7 (use 0 last: it is also where a definition without `memoryslot`, or with 256, lands) |
| 10 KB | 8, 11, 12, 14, 15, 17, 19–26, 28–57 |
| 5 KB | 58, 63, 64, 65, 67, 69, 71–76, 80, 81, 84–87 |

### Slots above 89 (Q-16, 2026-10-19)

What lies past `0x81C09000` is neither heap nor sample memory: the heap is `0x8014206C`–`0x80BFFFFB` and the
sample-pack cache starts at `0x82700000` [code]. Nothing in the OS or in TE's engines refers to
`0x81C09000`–`0x826FFFFF` except the content-mode USB disk, which also covers slots 0–89 and ends with a restart
[code, absence of references; manual p.61]. Details: [memory-map.md](../../research/memory-map.md).

| Slots | Status | Use |
|---|---|---|
| 0 (50 KB) | safe [code]; catches definitions without a slot | last 50 KB slot, only when 1–7 are gone |
| 1–89 | safe [code]; TE and we run engines across all three classes [proven] | as allocated below |
| 90–255 (5 KB) | very likely safe [code + inferred], **not proven** | **closed until hardware test H-16 passes** ([memory-map.md](../../research/memory-map.md#hardware-test-h-16-prove-a-slot-above-89)); then ours, allocated upward from 90 |
| 256 and up | never: they alias 0–255 [code] | tools must reject them |

**Stay at or below 89** until H-16 passes. Slots 90–255 add 166 slots of 5 KB only: a 10 KB engine does not fit
them. They help engines that fit 5 KB, which is a reason to aim new engines at 5 KB.

## Allocation policy for this project

Allocate **from the top of each class downward**, leaving the low free slots for anything TE or other third
parties might ship:

| Class | Our range | Count |
|---|---|---|
| 50 KB | 7, 6, 5, 3, 2 | 5 |
| 10 KB | 57 → 28 | 30 |
| 5 KB | 87 → 84, 81, 80, 76 → 71, 69, 67, 65 → 63 | 17 |

Extended on 2026-10-18 for the engine backlog (was 50 KB 7–5, 10 KB 57–40, 5 KB 87–84/81/80; 5 KB 69, 67, 65–63 added 2026-10-27 when 87–71 had filled). TE's OP-Z firmware
is mature, so the risk of a future TE engine landing in these slots is low; slots 1 (50 KB), 8–27 (10 KB) and the
low 5 KB slots stay free for anyone else.

**Plug ids:** effects 61000–61009, synths 61010–61019 (full since batch 3: synths continue at 61200), drums 61020–61029; effects batch 5 took 61100–61104 (61000 and 61009 stay free); when a block fills, effects continue at
61100, synths at 61200, drums at 61300. Test plugs stay in 60900–60999. **Sample plugs made by our tools** (F-09,
`opz.py samples install`): 61400–61499, one per pack, no engine slot (they play on TE's `sampleplayergain` or
`synthsampler`; [samples.md](../../research/samples.md)).

Record every allocation in the table below in the same pull request that adds the engine.

**Simulator-only engines** (`"sim_only": true` in plug.json, e.g. `opz_test_macro_unbound`) are never installed:
`opz.py install` refuses them and the golden tests skip their device build. They carry slot 0 as a placeholder and
take no row in this table.

| Slot | Engine | Plug id | Status |
|---|---|---|---|
| 87 | `opz_test_passthrough` (P3 proof) | 60900 | planned |
| 86 | `filter` (3.2 KB) | 61002 | built |
| 85 | `opz_test_gain` (P3 proof) | 60902 | planned |
| 84 | `glue` (3.1 KB; no buffer: FX1, FX2) | 61004 | built |
| 57 | `opz_test_sine` (P3 proof) | 60901 | planned |
| 56 | `swirl` (4.7 KB at `-O2`, incl. libgcc division) | 61001 | built |
| 55 | `macro` (6.5 KB) | 61010 | built |
| 54 | `kit` (7.9 KB) | 61020 | built |
| 53 | `resonator` (6.4 KB; asks for 66 KB of FX buffer: FX1, FX2) | 61003 | built |
| 49 | `super` (5.0 KB) | 61015 | built |
| 48 | `echo` (8.2 KB; asks for 1 MB of FX buffer: FX1) | 61005 | built |
| 47 | `plate` (8.0 KB; asks for 224 KB of FX buffer: FX1, FX2) | 61006 | built |
| 51 | `fm2` (4.0 KB) | 61011 | built |
| 50 | `fold` (3.4 KB) | 61012 | built |
| 7 | `dx` (23.7 KB: 8 KB sine table, 4 KB DX7-format patch bank) | 61013 | built |
| 6 | `pluck` (42.5 KB: 14.9 KB code and data, 27.6 KB `.bss` with the string pool of 13 × 2 KB lines; a 10 KB window would leave ~6 KB of lines, too short for the bass range) | 61014 | built |
| 5 | `table` (46.7 KB: 40 KB of wave tables, 42 waves × 4 mip levels, and 1.2 KB of harmonic coefficients; 3.4 KB code; 1.6 KB `.bss`) | 61016 | built |
| 52 | `modal` (6.9 KB: S-11 struck/bowed modal synth; no voice pool, all in the 268 B voice state) | 61017 | built |
| 46 | `vowel` (6.1 KB: 5.4 KB code and tables, 0.7 KB `.bss` block buffers) | 61018 | built |
| 45 | `clouds` (5.7 KB; asks for 1 MB of FX buffer, an 11.9 s grain ring: FX1; 130 KB elsewhere) — engine batch 2 | 61007 | built |
| 44 | `shimmer` (9.0 KB; X-07 reverb with a pitch shifter in its loop; asks for 243 KB of FX buffer: FX1, FX2) — engine batch 2 | 61008 | built |
| 43 | `sync` (4.4 KB: S-02 hard-sync lead; 0.3 KB `.bss` event buffer, no tables) — engine batch 3 | 61019 | built |
| 42 | `pd` (8.7 KB: S-04 phase distortion; 6.1 KB code, 2.2 KB tables, 0.4 KB `.bss` block buffers; 28 B of state) — engine batch 3 | 61200 | built |
| 41 | `ensemble` (5.2 KB: S-13 string machine; no voice pool: the ensemble is four phases of each voice's oscillator, 76 B of state) — engine batch 3 | 61201 | built |
| 40 | `swarm` (9.1 KB: S-15 oscillator/particle swarm; 8.3 KB code and tables, 1.0 KB `.bss` block buffers) — engine batch 3 | 61202 | built |
| 81 | `chip` (3.4 KB: 3.0 KB code and data, 0.35 KB `.bss` wave tables; S-14 8-bit voice) — engine batch 3 | 61203 | built |
| 80 | `fmperc` (3.2 KB: D-02 2-op FM drums, 24 recipes, 514 B sine table; 5 KB class) — engine batch 4 | 61021 | built |
| 39 | `mallet` (D-03 modal percussion, 6.3 KB) — engine batch 4 | 61022 | built |
| 76 | `hats` (4.2 KB: D-04 metallic hats and cymbals, 380 B `.bss`) — engine batch 4 | 61023 | built |
| 75 | `8bit` (3.4 KB: D-05 chip drums; no voice pool, 40 B of state) — engine batch 4 | 61024 | built |
| 74 | `snap` (4.9 KB: D-06 claps, rims, shakers; 5,016 of 5,120 B, no `.bss`) — engine batch 4 | 61025 | built |
| 73 | `opz_test_tempo` (hardware check H-7, Q-18 sequencer keys; 1.4 KB) | 60903 | built |
| 72 | `opz_test_sampler` (hardware check H-S2, Q-27 samples: plays `getSample`'s slices; 1.0 KB; its plug names `samplepacks/opzk_testkit.aif`) | 60905 | built |
| 71 | `repeat` (1.8 KB: X-04 beat repeat; asks for 240 KB of FX buffer: FX1, FX2; 64 B of state) — engine batch 5 | 61100 | built |
| 38 | `spring` (2.5 KB: X-08 dispersive spring reverb; asks for 41 KB of FX buffer: FX1, FX2) — engine batch 5 | 61101 | built |
| 37 | `lofi` (3.6 KB: X-12 tape and converter wear, tape stop; asks for 128 KB of FX buffer: FX1, FX2) — engine batch 5 | 61102 | built |
| 36 | `shift` (1.6 KB: X-10 ring modulator / frequency shifter; no buffer, 180 B of state) — engine batch 5 | 61103 | built |
| 35 | `harmonizer` (2.6 KB: X-11 two-voice pitch shifter over a moving delay network; asks for 64 KB of FX buffer: FX1, FX2) — engine batch 5 | 61104 | built |
| 69 | `opz_test_param` (hardware check H-23, Q-23 parameter words; 1.1 KB). Was linked for 72 together with `opz_test_sampler` until 2026-10-27 (never installed) | 60904 | built |
| 67 | `opz_test_load` (hardware check H-C1 and Q-46: times the OS around its own render calls, reads the idle counter; 3.2 KB) | 60907 | built |
| 65 | `opz_test_l2` (hardware check H-L2, Q-45: scans and times L2 SRAM above the OS image; 2.8 KB) | 60908 | built |
| 34 | `opz_test_macro_full` (4.0 KB: test plug, `opz_test_macro_unbound` in fixed point; ~7x a voice's CPU budget, lead track only) | 60909 | built |

## Consequences

- An engine binary is valid for **one** slot. Moving it means re-linking.
- **Never put two engines in one slot**, even if they are never used at the same time: the OS loads every engine
  listed in `plugs.json` at boot, in alphabetical order of the path, not when a track picks it. The later one
  overwrites the earlier one, whose function pointers then point into foreign code [code, `FUN_800fff1c`;
  [memory-map.md](../../research/memory-map.md#when-engines-load-and-what-two-engines-in-one-slot-do)].
- **Removing a plug does not free its slot**: the engine definition stays in `plugs.json` and still loads at boot.
  A slot we have installed into is taken for good on that unit.
- Static data counts against the window. Wavetables and big lookup tables push an engine into the 50 KB class;
  prefer the host's sine/filter tables and compute the rest.
- [idea] Big buffers could live outside the window, at fixed addresses in the unreferenced memory past slot 255
  (`0x81CD8800`–`0x826FFFFF`), if the linker emits no load block for them and `init` clears them. Not allowed until
  H-16's canary step passes and the SDK and an address table support it ([memory-map.md](../../research/memory-map.md#reclaimable-memory)).
- A 50 KB slot is scarce (three for us). Reserve them for engines that really need tables: wavetable, speech,
  6-operator FM with a preset bank.
