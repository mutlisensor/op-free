# swirl

Stereo phaser / flanger for FX1 and FX2, that follows notes sequenced on its own track.

| Control | Function |
|---|---|
| P1 **rate** | LFO 0.03–10 Hz, exponential |
| P2 **character** | one macro over mode, depth and feedback (table below) |
| pan | not reachable on the OP-Z: an FX track has no pan dial, app control or mixer column [proven, H-FX1], and presets can't set it, so it stays where the OS leaves it (127 on the unit checked): **(almost) the full effect**. The law, for anything that does set pan: 0 = dry only, **128 = the full effect** (the classic 50/50 phaser: a phaser *is* the dry plus the all-passes; the SDK's law puts the full effect at the centre too), 255 = the all-passes alone (vibrato in phaser mode, pitch wobble in flanger mode). Below and above the centre: TE's balance law normalised to unity |
| dry level (hold the FX track's key + **red dial**) | the OS's own dry level (byte 12; TE's app shows it as "dry level" on the FX page): how much of each sending track's own sound stays in the mix. 255 = send/return (the source plays on and the effect adds to it), 0 = insert (a track sent at 100 % is heard only through the effect). This is the dry/wet control on the OP-Z [proven, H-FX1 2026-11-11] |
| level | output level (nothing on the OP-Z reaches an FX track's level, neither a dial nor the mixer page [proven, H-FX1], so the preset's value holds) |
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
| Plug | 61001, version 5, `trackmask` 0x300 (FX1, FX2; the master track is a fixed chain), flags 1 (insert-type FX, [host-tables-flags.md](../../research/host-tables-flags.md#recommended-flags-for-our-engines)) |
| Memory slot | 56 (10 KB class), 3.8 KB used at `-O2` |
| Memory request | 8 KB (2 × 2048 int16 delay lines). Cleared 512 samples per block, so `init` stays short |
| State | 108 B of 200 (Blackfin, 32-bit pointers) |
| CPU | **v5 (2026-11-11): 462 cycles per stereo frame in its worst case, worst block 463** (fx-usability.md §4a). Earlier figures: Blackfin (simulator, [cycle-model.md](../../research/cycle-model.md)), v3: **21,000 cycles/block phaser, 26,500 flanger** (329 / 414 per stereo frame; v2: 1,240 / 1,590), under the 600/frame effect target; P1 doesn't change it. Note-on 1,141; a pan change costs ~700 once |

Simulator checks: notes tune the comb (harmonics 18–21 dB above the gaps for A2, E3, A3, median of harmonics
2–15); the phaser sweep moves the 990 Hz band over a 19 dB range; a full P2 sweep stays within 1.4 dB from one
23 ms window to the next.

## Version 5 (2026-11-11): what changed, what to listen for

Testers found our effects hard to use: the mix knob seemed not to work, some effects seemed not to pass the sound, and
loud material clipped. Measured in the simulator (fx-usability.md). Three
faults in swirl:

- **Phase inversion at the ends of pan**: at pan 0 the "dry" came out inverted (the normalised gain 2³¹ wrapped to
  −1), and at 255 the wet did. Against the dry the OS keeps (a send below 100 %, or a dry level above 0) an inverted
  dry cancels: turning pan to "dry" made the sound **vanish** instead. Fixed: both gains are capped at +1.
- **Hard clipping inside the loops**: the phaser ran at half scale and the flanger's delay line saturated at its
  16-bit full scale, so feedback resonance clipped: at P2 255 a −6 dBFS saw had 50,612 flat samples in 4 s. Now the
  loops run at a quarter scale and the flanger's input is turned down as its feedback rises (~√(1 − fb²), so the comb
  keeps its loudness instead of gaining up to 7 dB): 15,710 samples at the soft ceiling in the worst case (P1 0, P2
  255: a static, maximum-feedback comb on a harmonic-rich saw), none at the preset.
- The output passes the SDK's limiter.

In opzsim (no control on the OP-Z reaches an FX track's pan): pan to 0 with a send below 100 %: the dry sound, never
thinner or gone. On the device: P2 at 255 on a loud saw: a deep flanger without crackle.

## Credits

Clean-room design. TE built an unreleased `Swirl` effect for the OP-Z; only its name and that it follows notes are
known, and none of its code is used. MIT licence.
