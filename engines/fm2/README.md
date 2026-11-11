# fm2

Two-operator FM voice for the bass, lead, arp and chord tracks: one sine modulator, with feedback, phase-modulates
one sine carrier. It runs from pure tones through e-pianos, bells and basses to grit, and the OP-Z's own envelope and
velocity shape the brightness as well as the level. The OP-Z's own track filter (the filter and resonance knobs)
shapes the tone after it.

| Control | Function |
|---|---|
| P1 **ratio** | modulator : carrier frequency ratio, in **16 fixed steps of 16 values** (P1 ÷ 16 picks the step; the table below). Inside a step nothing changes; crossing into the next step changes the timbre at once, without a click. 0–191 are harmonic ratios, 192–255 the moving and inharmonic zone |
| P2 **index** | modulation depth. 0 is a pure sine. 0–191: index 0 → 4 rad on a quadratic curve, so the low end is fine-grained (64 ≈ 0.44 rad, 96 ≈ 1.0, 128 ≈ 1.8, 160 ≈ 2.8, 191 ≈ 4.0). 192–255: index 4 → 6.3 rad, and **feedback** on the modulator rises from 0 to 2 rad (224 ≈ 1 rad): first brighter and saw-like, then from ~230 noisy and gritty. These are the values at velocity 127 with the envelope at full level |
| velocity | level (as on `macro`), and brightness two ways: the index is 50 % of P2's value at velocity 0 and 100 % at 127, and a **strike** adds up to another 100 % at the start of each note (velocity 127; 50 % at velocity 64) that dies away with a time constant of 150 ms at C4, half that two octaves up, twice that two octaves down (40–400 ms). Hard notes bark, soft notes stay round. Notes start with the modulator and carrier in phase, the same way every time |
| attack … release | the OP-Z envelope sets the level, **and the index follows the envelope's level** (a block, 1.5 ms, behind): a slow attack swells from a pure tone into brightness, decay and sustain set how far the sound mellows, the release darkens as it fades. Sustain 255 keeps the brightness steady |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |
| pan, level | the track's pan and level (yellow and red dial, orange page), applied by the engine as TE's engines do (`opz_mix.h`): level is linear (128 = −6 dB, 0 = silent), pan a balance (centre: both sides full; hard left: right silent). The FX1/FX2 sends are taken after them. They follow the LFO (targets pan and volume) without zipping |

Anti-aliasing: for each block the index is limited so that the highest strong sideband, carrier + (index + 1) ×
modulator frequency, stays below 16 kHz, and the feedback fades out as that room shrinks from 20 to 4 modulator
frequencies (with ratio 1, full feedback up to about F♯5, 740 Hz, and none from about G♯7). High notes and high
ratios get darker instead of folding back; a C4 at ratio 14 keeps at most 3.3 rad, a C7 at ratio 1 at most 5.6 rad.

### Ratio steps (P1)

| P1 | Ratio | Character |
|---|---|---|
| 0–15 | 0.5 | an octave below the note joins: hollow, sub-like |
| 16–31 | 1 | all harmonics: saw-like, brass, e-piano body |
| 32–47 | 1.5 | harmonics of half the note, hollow |
| 48–63 | 2 | odd harmonics: square-like, clarinet |
| 64–79 | 3 | |
| 80–95 | 4 | |
| 96–111 | 5 | |
| 112–127 | 6 | |
| 128–143 | 7 | |
| 144–159 | 9 | |
| 160–175 | 11 | |
| 176–191 | 14 | high, glassy partials: tines |
| 192–207 | 2 + 0.7 Hz | odd harmonics with a slow shimmer: the modulator runs 0.7 Hz sharp, so the partials beat at the same slow rate on every note |
| 208–223 | 1.414 (√2) | inharmonic: bell, gong |
| 224–239 | 3.5 | tubular bell |
| 240–255 | 3.14 (π) | inharmonic: metal |

The ratio is the modulator's frequency over the note's. The carrier always plays the note, so its frequency never
moves with P1, but at 0.5 and 1.5 a partial an octave below the note joins (and 3.5 puts partials between the
note's harmonics), which can make the sound seem an octave lower. The OS LFO aimed at P1 steps through the
ratios.

## Presets

Five per synth track, as TE's synths have: a track that switches to `fm2` gets the first of its row (in **bold**), the
others are on the preset keys. Ratio in brackets; A/D/S/R are the OP-Z envelope knobs.

| Track | Preset | P1 | P2 | A | D | S | R | Octave | Level | Sound |
|---|---|---|---|---|---|---|---|---|---|---|
| bass | **fm bass** | 24 (1) | 150 | 0 | 90 | 110 | 30 | −2 | 210 | punchy bass with a quick "wow" as the index falls to the sustain |
| bass | sub bass | 8 (0.5) | 100 | 0 | 100 | 180 | 30 | −2 | 210 | hollow, with a partial an octave down |
| bass | slap bass | 56 (2) | 160 | 0 | 60 | 60 | 25 | −2 | 210 | square-ish, short and snappy |
| bass | grit bass | 24 (1) | 235 | 0 | 110 | 170 | 30 | −2 | 200 | feedback grit on top of a saw-like bass |
| bass | wood bass | 72 (3) | 90 | 0 | 70 | 0 | 40 | −2 | 210 | plucked, woody, decays to a pure tone |
| lead | **fm lead** | 24 (1) | 175 | 0 | 120 | 200 | 50 | 0 | 190 | bright, brassy lead |
| lead | clarinet | 56 (2) | 120 | 20 | 80 | 230 | 40 | 0 | 190 | odd harmonics, reedy |
| lead | brass | 24 (1) | 160 | 40 | 100 | 180 | 50 | 0 | 190 | swells into brightness with the attack |
| lead | grit lead | 24 (1) | 245 | 0 | 0 | 255 | 40 | 0 | 180 | noisy, distorted |
| lead | glass lead | 184 (14) | 100 | 0 | 140 | 120 | 80 | 0 | 190 | glassy high partials over a sine |
| arp | **pluck** | 72 (3) | 140 | 0 | 80 | 0 | 50 | 0 | 200 | bright metallic pluck that decays to a pure tone |
| arp | bell | 216 (√2) | 170 | 0 | 225 | 0 | 150 | +1 | 190 | struck bell, darkening as it rings for several seconds |
| arp | marimba | 88 (4) | 100 | 0 | 60 | 0 | 40 | +1 | 200 | short, woody mallet |
| arp | chime | 232 (3.5) | 130 | 0 | 160 | 0 | 100 | +1 | 190 | tubular chime |
| arp | blip | 56 (2) | 150 | 0 | 40 | 0 | 20 | 0 | 200 | very short square-ish blip |
| chord | **e-piano** | 24 (1) | 110 | 0 | 170 | 50 | 70 | 0 | 160 | round electric piano: velocity sets the bark, then it mellows |
| chord | organ | 200 (2 + 0.7 Hz) | 90 | 10 | 0 | 255 | 40 | 0 | 150 | hollow, slowly shimmering organ with a little key chiff |
| chord | fm pad | 24 (1) | 140 | 150 | 0 | 255 | 150 | 0 | 150 | slow swell from a sine into brightness; needs long notes |
| chord | tine piano | 184 (14) | 70 | 0 | 150 | 40 | 70 | 0 | 160 | glassy, DX-like tines |
| chord | bell pad | 216 (√2) | 110 | 90 | 200 | 150 | 160 | 0 | 150 | soft inharmonic pad |

Every preset: filter 128, resonance 0, pan 128, LFO off. Chord presets sit at a lower level because four voices sum
(sustained four-note chords reach 0 dBFS in the track buffer at level 255). `python3 tools/opz.py presets
check engines/fm2` passes with no warnings. Not tuned by ear on the device yet.

## Facts

| | |
|---|---|
| Plug | 61011, `trackmask` 0xF0 (bass, lead, arp, chord), version 2 (v2: applies the track's level and pan) |
| Memory slot | 51 (10 KB class), 4.4 KB used on Blackfin (4,556 bytes: 2 KB of it the sine table) |
| State | 172 B of 280 per voice (128 of them render's block buffer) |
| CPU | Blackfin simulator ([cycle-model.md](../../research/cycle-model.md)), `python3 tools/opz_cycles.py engines/fm2`: **~6,540 cycles/block/voice in sustain, 7,800 worst** (102 / **122 per sample**; level and pan cost ~5 of them with the hand-scheduled `opz_mix.h` loops, ~14 with the C loops), the same at every ratio and index, a little less in decay and release. 160 per note-on. 19 voices at the worst settings: 111,300 cycles per block on average, 130,300 worst, of the OS's 265,000 (under the 218/sample 19-voice budget, before the OS's own share) |
| Level | −12 dBFS per voice at velocity 127 and level 255 (`OPZ_VOICE_PEAK`), × the level dial; FM keeps the peak of a sine at every index, so the level does not change with P2. Four voices of a chord started together can reach 0 dBFS in the track buffer (the simulator's e-piano chords peak at −0.4 dBFS) |

Each sample: the modulator (a 1,024-point sine table, linearly interpolated, Q15) with feedback from the average
of its last two outputs, as on the classic chips; then the carrier, phase-modulated by it; then level and envelope.
Index and feedback ramp across each block from where the last one ended, so knob moves, velocity and the strike
never zip.

Simulator checks: P1 swept through all 16 steps under a held note leaves no clicks (worst block 3× its neighbours'
second difference, a timbre change). Without feedback (P2 191, velocity 127) a held note's non-harmonic energy
(aliases) stays below −68 dB at ratio 1 from C4 to C7 and at ratio 2 up to C6; where a high ratio or a high note
pushes sidebands just past Nyquist (C4 at ratio 14, C7 at ratio 2, C6–C7 at ratio 3.5) it is −34 to −37 dB, folded
back above ~18 kHz. With feedback, P2 215 stays at −54 dB or better up to C6; P2 235 adds noise at −28 to −40 dB
from C4 up (less on low notes) and P2 255 at −10 to −22 dB, by design. Velocity 20 → 127 at P2 100, ratio 3 moves the spectral centroid of C4 from 410 to 1,020 Hz at
the start of the note (540 Hz once the strike has gone); a slow attack (150) starts as a pure sine and brightens.
Six golden scenarios (`tests/`). With feedback at ratio 0.5 or 1 the waveform turns asymmetric (a DC offset up to
~25 % of the peak at P2 255 on high notes); the OS's DC blocker after the engine removes it.

## What to listen for on the device

Install on a synth track and play the e-piano preset: step velocities (or MIDI velocity) should clearly change how
much each note barks. Turn P1 slowly: the timbre should jump in 16 clean steps, with no clicks and no pitch change,
and 192–207 should shimmer slowly. Sweep P2 from 0 (pure sine) to 255: brighter and brighter, then gritty in the
last quarter. Lower sustain and the sound should mellow after each attack; a long attack should swell from a sine.
Play high notes (C6 and up) at P2 255 with ratio 14: they should get darker, not harsh or out of tune. Hold a chord
while new notes steal voices: no clicks.

## Credits

Original code; the design follows John Chowning's FM synthesis and the classic two-operator chips and their averaged
operator feedback, with the OP-1's FM engine and TE's unreleased `Fm` as inspiration (no TE code). MIT licence.
