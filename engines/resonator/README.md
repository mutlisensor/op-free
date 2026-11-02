# resonator

Tuned string resonator for FX1 and FX2. Whatever is sent to the FX track excites up to four strings: drums ring at
the strings' pitch, pads get a stringy or metallic halo. Notes sequenced on the FX track tune the strings, and notes
held together form a chord.

| Control | Function |
|---|---|
| P1 **decay** | how long the strings ring: 40 ms (a short knock) to 16 s, exponential (64 ≈ 0.18 s, 128 ≈ 0.8 s, 192 ≈ 3.6 s). Exact for the fundamental at any P2, except that dark settings (P2 below ~64) cut high notes short: at P2 0, C5 rings at most ~2 s |
| P2 **structure** | one macro over tone and partials. 0–127: dark, muted string → bright string (the damping opens; dark settings get up to +6 dB make-up). 128–255: harmonic → inharmonic, bell- and metal-like (dispersion stretches the partials: at 255 a C3 has partials at 1, 2.26, 3.74, 5.33, 6.95 × the fundamental instead of 1, 2, 3, 4, 5), with a little more damping towards 255. The pitch stays put over the whole range |
| pan | dry/wet balance (TE's law, normalised to unity gain): 0 dry only, 128 half and half, 255 wet only |
| level | output level |
| filter, resonance | not used: they are the OS's own track filter, also on FX tracks. Presets leave it neutral (128, 0) |
| pitch bend | bends every string, ±2 semitones at full scale (the simulator's bend scale; the device's is not confirmed yet) |
| **notes on the FX track** | each note tunes a string (up to four). Notes held together form a chord, and the chord **stays after the keys are released** (latch); the next note after all keys are up starts a new chord. The old chord's strings stop being fed and ring out at their own pitch, like a sustain pedal. A new note takes a silent string if there is one, else the oldest ringing-out string, else the oldest string of the chord (a fifth held note). A string that is still ringing glides to its new note (~30 ms, S-curve); a silent one just starts. Notes fold into C1–C7 by octaves. Velocity is ignored |
| **no notes** | one string at C3, so the effect works when nothing is sequenced on the FX track |

Strings hear the mono sum of the input (L + R). The four strings have fixed places in the stereo field, two in the
centre, one left, one right (the far side at −6 dB); which note lands where depends on which string was free. A
sustained input right at a string's pitch with a long decay builds up: above −6 dBFS of wet signal the input ducks,
and a look-ahead limiter holds the wet peak at −1.5 dBFS, so it never clips.

## Preset and settings to try

FX tracks have no preset keys: the OP-Z applies one preset, slot 0 index 0, when resonator is chosen
([presets.md](../../research/presets.md)). That preset is **string**. The other sounds are settings to dial in:

| Name | P1 | P2 | pan | Sound |
|---|---|---|---|---|
| **string** (preset) | 150 | 105 | 128 | bright harmonic string, ~1.4 s |
| knock | 55 | 35 | 150 | short dark knock, for drums: a tonal thump under each hit |
| halo | 215 | 80 | 100 | warm, ~6 s ring behind a pad, mostly dry |
| bell | 185 | 215 | 140 | inharmonic bell, ~3 s |
| metal | 120 | 255 | 170 | short clangy metal, mostly wet |

All at filter 128, resonance 0, level 200.

## Facts

| | |
|---|---|
| Plug | 61003, version 2, `trackmask` 0x300 (FX1, FX2), flags 1 (insert-type FX, [host-tables-flags.md](../../research/host-tables-flags.md#recommended-flags-for-our-engines)) |
| Memory slot | 53 (10 KB class), 6.4 KB used on Blackfin |
| Memory request | 67,760 B: 4 strings × 2048 int32 samples, each written twice so reads never wrap. Fits FX2's 256 KB, not master's 16 KB. Cleared 2048 words per block, so `init` stays short; for the first 8 blocks (12 ms) only the dry signal plays |
| State | 88 B of 200 (Blackfin, 32-bit pointers) |
| CPU | Blackfin simulator (`bfin-elf-run`, the cycle harness in progress): ~262 cycles/sample with one string (no notes), ~411 with a 2-note chord, ~710 with 4 notes, worst block 867. Strings that rang out stop costing anything. Host: 8–19 ns/sample |
| Notes on FX tracks | the OS has no voice limit there: `FxManager` passes every note-on and note-off to the effect, which decides (here: four strings, oldest stolen). When a note is released while another is held, the OS may re-send the held note as a note-on with velocity 0 and no parameters (see [engine-abi.md §3](../../sdk/docs/engine-abi.md#onnoteon--onnoteoff)); resonator treats that as "this note alone" |

Each string is a fractional delay line (4-point Hermite) with a one-pole damping low-pass, two first-order all-passes
for dispersion, a loop gain from P1 and a soft clip in the feedback path. The delay is corrected for the loop
filters' phase delay at the fundamental, and the loop gain for their loss there.

Simulator checks: tuning within 0.1 cent from C1 to C7 at every P2; decay (T60 of the fundamental) within ~5 % of
P1's setting from C2 to C7 for P2 ≥ 64; noise in comes out at about the same level at every P2; full-scale input at
the string's pitch with P1 at 255 stays at −1.5 dBFS; note changes, chord changes, steals and bends leave no clicks
(second-difference spikes ≤ 15× the local median, against ~40,000× before the fixes).

## What to listen for on hardware

Install on FX1, raise a drum track's FX1 send: every hit rings at C3. Sequence a note, then a three-note chord on
FX1: the ringing follows the notes and the chord stays after the step ends. Turn P1 from a short knock to a long
ring and P2 from dark through bright to metallic; the pitch should not move. No clicks when notes change, no clipping
with P1 at 255.

## Credits

Clean-room design in the spirit of Mutable Instruments Rings and Elements (Karplus–Strong strings with damping and
dispersion); no code from them. MIT licence.
