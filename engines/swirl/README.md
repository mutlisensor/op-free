# swirl

Stereo phaser / flanger for FX1 and FX2, that follows notes sequenced on its own track.

| Control | Function |
|---|---|
| P1 **rate** | LFO 0.03–10 Hz, exponential |
| P2 **character** | one macro over mode, depth and feedback (table below) |
| pan | dry/wet balance (TE's law, normalised to unity gain; centre = classic 50/50 phaser) |
| level | output level |
| filter, resonance | not used: they are the OS's own track filter, also on FX tracks. Presets leave it neutral (128, 0) |
| **notes on the FX track** | the sweep centre moves to the note's pitch; in flanger mode the delay becomes one period of the note — a comb tuned to it — and swings only ±1.5 % around it so it stays in tune. The LFO restarts on each note; note off returns to the free-running sweep (700 Hz / 3 ms) |

| P2 | Mode | Depth | Feedback |
|---|---|---|---|
| 0–111 | 6-stage phaser | 25 % → 100 % (±1.5 octaves) | 0 → 0.7 |
| 112–143 | crossfade phaser → flanger | eases back to 25 % | eases back to 0.1 |
| 144–255 | flanger | 25 % → 100 % of the base delay | 0.1 → 0.9 |

The curve is piecewise linear, so an LFO on P2 glides between the modes without steps.

Right channel's LFO runs 90° ahead of the left for width.

## Preset and settings to try

FX tracks have no preset keys: the OP-Z applies one preset, slot 0 index 0, when swirl is chosen
([presets.md](../../research/presets.md)). That preset is **slow phase**. The other sounds are settings to dial in:

| Name | P1 | P2 | pan | Sound |
|---|---|---|---|---|
| **slow phase** (preset) | 60 | 70 | 128 | classic slow 6-stage phaser, half wet |
| deep phase | 40 | 108 | 128 | slower, deeper, more feedback, just before the flanger crossover |
| wobble | 200 | 85 | 128 | fast phaser wobble |
| subtle | 70 | 20 | 90 | light, mostly dry movement |
| jet | 30 | 245 | 128 | slow, heavy-feedback jet flanger |
| doubler | 120 | 150 | 110 | light flanger, short-delay doubling |
| shimmer | 160 | 128 | 140 | halfway between phaser and flanger, fast |
| note comb | 20 | 235 | 200 | resonant comb, mostly wet: sequence notes on the FX track to tune it |

All at level 200, filter 128 / resonance 0.

## Facts

| | |
|---|---|
| Plug | 61001, version 4, `trackmask` 0x300 (FX1, FX2; the master track is a fixed chain), flags 1 (insert-type FX, [host-tables-flags.md](../../research/host-tables-flags.md#recommended-flags-for-our-engines)) |
| Memory slot | 56 (10 KB class), 3.8 KB used at `-O2` |
| Memory request | 8 KB (2 × 2048 int16 delay lines). Cleared 512 samples per block, so `init` stays short |
| State | 108 B of 200 (Blackfin, 32-bit pointers) |
| CPU | Blackfin (simulator, [cycle-model.md](../../research/cycle-model.md)), v3: **21,000 cycles/block phaser, 26,500 flanger** (329 / 414 per stereo frame; v2: 1,240 / 1,590), under the 600/frame effect target; P1 doesn't change it. Note-on 1,141; a pan change costs ~700 once |

Simulator checks: notes tune the comb (harmonics 18–21 dB above the gaps for A2, E3, A3, median of harmonics
2–15); the phaser sweep moves the 990 Hz band over a 19 dB range; a full P2 sweep stays within 1.4 dB from one
23 ms window to the next.

## Credits

Clean-room design. TE built an unreleased `Swirl` effect for the OP-Z; only its name and that it follows notes are
known, and none of its code is used. MIT licence.
