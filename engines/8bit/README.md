# 8bit

Chiptune drums for the kick, snare, perc and sample tracks — no samples. Each of the 24 keys plays its own recipe on
a small console-style voice: one **tone channel** (a 4-bit triangle or a 12.5 / 25 / 50 % pulse) and one **noise
channel** (a 15-bit shift register in long or short "metallic" mode), each with a stepped 16-level volume envelope.
Everything moves in **frames**, as on an old games console: pitch sweeps and arpeggios jump in steps, volumes fall
one step at a time. The lo-fi is deliberate and on a knob: P2 runs from clean (band-limited) to a 3-bit,
5.5 kHz crunch.

| Key | Sound | Recipe |
|---|---|---|
| 1 (F) | kick tri | triangle falling 3 octaves to G1, most of it in 20 ms, and a noise click; 260 ms |
| 2 | kick pulse | 50 % pulse swept 2 octaves down to A1; 175 ms |
| 3 | boom | triangle from 2½ octaves above E1, slower drop; 435 ms |
| 4 | kick hard | 25 % pulse, very fast 3-octave drop, louder click; 110 ms |
| 5 | snare tri | triangle drop on C3 plus long noise; 175 ms |
| 6 | snare tight | 25 % pulse blip plus bright short noise; 65 ms |
| 7 | snare metallic | triangle drop plus short-mode (metallic) noise at ~670 Hz; 130 ms |
| 8 | rim | 12.5 % pulse at C6 with a noise tick; 20 ms |
| 9 | closed hat | white noise; 35 ms; chokes keys 11 and 12 |
| 10 | closed metallic | short-mode noise at ~2.7 kHz; 35 ms; chokes keys 11 and 12 |
| 11 | open hat | white noise; 390 ms |
| 12 | open metallic | short-mode noise at ~1.9 kHz; 520 ms |
| 13 | low tom | triangle on E2, an octave's drop; 220 ms |
| 14 | mid tom | the same on B2 |
| 15 | high tom | the same on F♯3; 175 ms |
| 16 | pulse tom | 25 % pulse on A3, short drop; 130 ms |
| 17 | blip | 50 % pulse at C6; 65 ms |
| 18 | chirp | 25 % pulse A5, an octave up after 35 ms; 260 ms |
| 19 | coin | 50 % pulse B5, up to E6 after 80 ms (60 Hz frames); 480 ms |
| 20 | zap | 12.5 % pulse, 3-octave fall that keeps sliding; 130 ms |
| 21 | laser | 50 % pulse sliding down from E6, ¾ semitone per frame; 175 ms |
| 22 | power-up | 25 % pulse climbing in eight 4-semitone steps from C4, sliding up between; 260 ms |
| 23 | explosion | low noise whose clock falls with a sweep, plus a low triangle thump; 1 s |
| 24 | jump | 25 % pulse sliding up from E4, ½ semitone per frame; 175 ms |

The first key is F3 (MIDI 53): key = (note − 53) mod 24, octave = floor((note − 53) / 24). Lengths are at P1 128,
P2 128. The closed hats cut the open hats on the same track within 5 blocks (7 ms), like one noise channel.

## Controls

| Control | Function |
|---|---|
| P1 **tune** | Transposes the tone and the noise clock in **semitone steps**, −12 at 0 … 0 at 123–133 … +12 at 246–255 (10.24 values per semitone; an LFO on P1 arpeggiates). It also scales the depth of the pitch sweep (the kick's drop, the zap's fall, the laser's and jump's slide) by 2^((P1 − 128) / 128): ×0.5 at 0, ×1 at 128, ×2 at 255. Arpeggio steps (chirp, coin, power-up) stay musical intervals |
| P2 **crunch** | **0–63 clean:** band-limited (PolyBLEP) pulses, a smooth triangle, a 16-bit output. **64–255 raw chip:** naive pulses, the 4-bit (16-step) triangle, and a DAC of 8 bits at 64–95, 7 at 96–127, 6 at 128–159, 5 at 160–191, 4 at 192–223, **3 at 224–255** (rounded; quiet tails drop out a step earlier). **144 up, sample-and-hold:** each output value is held 2 samples at 144–159 (22 kHz), 3 at 160 (14.7 kHz), 4 at 176 (11 kHz), 5 at 192 (8.8 kHz), 6 at 208 (7.4 kHz), 7 at 224 (6.3 kHz), **8 at 240–255 (5.5 kHz)**. **208 up:** every noise channel switches to the short, metallic mode (hats and snares ring at a pitch; the explosion growls) |
| velocity | level, linear (64 is −6 dB). Crunch is applied before the level, so soft hits are as crunchy as loud ones |
| octave | each press transposes tone and noise by 12 semitones (also beyond the OP-Z's range: notes far outside are folded into a key and an octave) |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |
| attack, decay, sustain, release | not used: each recipe makes its own stepped envelope and ends the voice itself |

Pitch, noise clock and crunch are recomputed every block (1.45 ms) from the knobs, so P1 and P2 act on a ringing
hit (try P2 on the explosion). A frame is 1, 2, 4 or 11 blocks by recipe (1.45, 2.9, 5.8 or 16 ms).

## Presets

All in slot 0 (indexes 0–4), the only slot drum tracks read; **chip** (index 0) is the sound a drum track gets when
it switches to 8bit. Every preset: no LFO (depth 128, speed 100, target 255, shape 0), filter 128, level 200.

| Index | Preset | P1 | P2 | Sound |
|---|---|---|---|---|
| 0 | **chip** | 128 | 128 | nominal pitch, 4-bit triangle and raw pulses through a 6-bit DAC |
| 1 | clean | 128 | 32 | band-limited, 16-bit: the recipes without the grit |
| 2 | handheld | 140 | 176 | a semitone up, 5-bit DAC held over 4 samples (11 kHz) |
| 3 | crushed | 110 | 240 | two semitones down, 3-bit at 5.5 kHz, metallic noise everywhere |
| 4 | high score | 170 | 96 | four semitones up with deeper sweeps, 7-bit |

`python3 tools/opz.py presets check engines/8bit`: 5 presets, 0 errors, 0 warnings. Not yet tuned by ear on the
device.

## What to listen for on the device

1. Install on the kick track (`python3 tools/opz.py install engines/8bit --track kick`), restart, pick 8bit on the
   kick, perc and sample tracks.
2. Play the 24 keys at the **chip** preset: each sound must be different and end on its own; keys 1–4 should hit
   about as hard as `kit`'s kicks.
3. Hold a hat pattern on one track: an open hat (key 11) followed by a closed hat (key 9) is cut at once.
4. Play the explosion (key 23) and turn P2 from 0 to 255 while it rings: clean → stepped → gritty → held, metallic
   from 208; no clicks other than the steps, no dropouts.
5. Play the coin (key 19) and power-up (key 22) and turn P1: the pitch moves in semitone steps.
6. Program a fast pattern (16ths on kick, snare, perc) and turn **P1 and P2 fast, both ways, while it plays**, on
   every track at once: no crash, no hang, no stuck voice.
7. Press the octave buttons down and up twice on a kick and a blip: transposed, still ending on their own.
8. Load the five presets (shift + preset keys) and check that each applies without LFO wobble.

## Facts

| | |
|---|---|
| Plug | 61024, `trackmask` 0x0F (kick, snare, perc, sample), engine and plug version 2. The device does not enforce the mask. flags 8: P1 (tune) bipolar, a centre-detent cue at 128 ([host-tables-flags.md](../../research/host-tables-flags.md#recommended-flags-for-our-engines)) |
| Memory slot | 75 (5 KB class): **3,452 of 5,120 bytes** on Blackfin, 3,180 of code, tables and data and 272 of `.bss` (one 64-word tone buffer shared by all voices, the per-track choke counters, the callback pointer) |
| State | 40 B of 280 per voice. `memRequest` 0, no voice pool |
| Level | Peaks −12.0 to −19.9 dBFS per hit at velocity 127 (`OPZ_VOICE_PEAK` −12); RMS over the first 250 ms −16 (explosion) to −34 dBFS (rim), kicks −19 to −26, toms −22, hats −22 to −27 (kit: −15 to −32). Pulses are scaled by duty so their larger half peaks like the triangle |
| CPU | Blackfin simulator ([cycle-model.md](../../research/cycle-model.md), `python3 tools/opz_cycles.py engines/8bit`), cycles per sample per voice, mean over the hit: **58–80 at P2 128 and 255**, 64–115 at P2 0 (band-limited pulses); worst block 81 / 74 / 137. With P1 and P2 moving every block 70 (worst 118). 8 voices hit in one block 68–82; 8 explosions 78. The heaviest block is a clean (P2 < 64) pulse two octaves up at P1 255 (every sample near an edge): **214**, and a new hit every block at P1 255, P2 0: 213 — under the 218 budget. Note-on 170–181 cycles |
| Maths | 32-bit only: no division, no 64-bit, no float anywhere; the frame logic runs once per block. The noise register advances up to 8 steps per sample in one go (the k new feedback bits are (s ^ (s >> tap)) & (2^k − 1)), a clock up to 353 kHz |

Simulator checks: 8 golden scenarios (all 24 keys at P2 128 and at P2 0 / P1 255, a P2 sweep under explosions and
lasers, a fast four-track pattern with P1 and P2 jumping, the choke, steals on the kick track, octaves −3…+3 with
MIDI 0 and 127, a groove). A UBSan + bounds fuzz ([tests/fuzz.c](tests/fuzz.c): random notes over the whole `int` range, velocities 0–255, wild
or NULL parameter words, steals, chokes) found no fault: the output never passes −12 dBFS per voice, a stolen
voice is silent within 3 blocks (4 ms), every voice ends.

## Credits

Original code. The voice follows the classic games-console sound chips (pulse channels with duty cycles, a 4-bit
triangle, a 15-bit LFSR noise channel with a 93-step short mode, frame-rate envelopes and sweeps), as also explored
by Mutable Instruments Plaits' chiptune engine (MIT); no code was taken from it. Band-limited pulses use the SDK's
PolyBLEP saw. MIT licence.
