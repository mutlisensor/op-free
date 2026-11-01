# mallet

Modal percussion for the kick, snare, perc and sample tracks: marimba, xylophone, vibes, glockenspiel, steel drum,
bells, gongs, glass and wood blocks, played as two chromatic octaves on the 24 keys. Each hit is ten decaying
partials whose frequency ratios, weights and decay times come from a material (P1, a continuous morph from wood
through metal to glass), struck by a mallet whose hardness follows the velocity. P2 sets how long and bright it
rings, from a dead, muffled thud to a long, bright ring. The OP-Z's own track filter (the filter and resonance knobs)
shapes the tone after it.

## The keys

The 24 keys are **two chromatic octaves**, F3 to E5: each key plays its own note, laid out like a piano keyboard
(the black keys are the sharps). The pitch of a hit is the MIDI note the OS sends, so the **octave buttons** (±24
notes per press on a drum track) move the keyboard by exactly its own span: octave −1 plays F1–E3 (gongs, bass
marimba), +1 F5–E7 (glockenspiel, small bells), and −2 / +2 go further (partials below 15 Hz or above 0.45 fs drop out).

| Key | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Note (octave 0) | F3 | F♯3 | G3 | G♯3 | A3 | A♯3 | B3 | C4 | C♯4 | D4 | D♯4 | E4 |
| MIDI | 53 | 54 | 55 | 56 | 57 | 58 | 59 | 60 | 61 | 62 | 63 | 64 |

| Key | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | 22 | 23 | 24 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Note (octave 0) | F4 | F♯4 | G4 | G♯4 | A4 | A♯4 | B4 | C5 | C♯5 | D5 | D♯5 | E5 |
| MIDI | 65 | 66 | 67 | 68 | 69 | 70 | 71 | 72 | 73 | 74 | 75 | 76 |

Why a scale and not a set of instruments (e.g. 4 materials × 6 notes): P1 already chooses the material
continuously, so every key is free for pitch; two full octaves play melodies, chords (two drum tracks with mallet
give four voices) and tuned percussion parts in any key, and they line up with what the same keys play on a synth
track. Sequenced mallet parts can be transposed with the octave buttons without leaving the 24-key layout.

## Controls

| Control | Function |
|---|---|
| P1 **material** | a continuous morph through nine materials, 32 apart: **0 wood** (wood block / log drum: a thick free bar, very short, a strong click), **32 marimba** (tuned wooden bar 1 : 3.99 : 9.95, soft yarn mallet, round), **64 xylophone** (wooden bar tuned to the twelfth 1 : 3 : 6.1, hard mallet, dry), **96 vibes** (aluminium bar 1 : 4 : 10 with a doublet that beats slowly, long, mellow), **128 glock** (free steel bar 1 : 2.76 : 5.40 : 8.93, brass mallet, bright, long), **159 steel drum** (harmonic fundamental, octave and twelfth plus a beating octave and skirt modes), **191 bell** (church bell: hum 0.5, prime, minor tierce 1.18, quint 1.51, nominal 2 …, very long), **223 gong** (dense near-harmonic cluster with beating pairs, soft beater, longest), **255 glass** (wine glass 1 : 2.32 : 4.25 : 6.63 : 9.38, each a pair 0.3–0.5 % apart that beats). Between two materials every partial glides on a log-frequency line from one ratio to the next and its weight and decay, the material's ring, damping, mallet and click are interpolated: halfway points are hybrids (16 a dry log, 112 metallic vibes, 175 a bell-like pan, 239 a glassy gong). Mode 0 is always the note: the pitch never moves |
| P2 **ring** | from dead to long, three things together. **Ring time**: the fundamental's T60 is the material's natural ring × 1/16 at 0, × 1/4 at 64, × 1 at 128, × 2.8 at 192, × 8 at 255 (exponential, 12 ms … 32 s; natural rings on a low note: wood 0.22 s, xylophone 0.7, marimba 1.4, steel drum 1.5, glock 3.5, vibes 4, glass 5, bell 6, gong 8). **Damping**: each partial's decay is shortened by 1 + h·f (f in kHz, h from the material) and h is × 4 at 0, × 1 at 128, × ¼ at 255, so low P2 is dark and muffled (a hand on the bar), high P2 bright, and high notes always ring shorter than low ones. **Mallet**: the strike's brightness moves one octave down at 0 and up at 255 |
| velocity | level (linear: 64 is −6 dB); the mallet's hardness: a low-pass corner 12 dB/octave from 500 Hz at velocity 0 to 16 kHz at 127, moved by the material's own mallet (marimba −0.8, gong −1.2 octaves; glock +1.5, glass +1.8) and by P2, so soft hits are dark; the contact time over which the hit rises, 4 ms soft → 0.25 ms hard; a short click of noise at the strike (velocity squared; strongest on wood and xylophone); the strike position, 0.22 → 0.15 of the bar (hard hits are thinner) |
| octave | transposes by two octaves per press (see above) |
| note-off, attack, decay, sustain, release | not used: every hit is a one-shot that rings out on its own (as kit) and frees its voice when its partials have decayed below −78 dBFS. Presets leave the envelope at 0, 0, 255, 0 |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |

The knobs are read when a hit starts (as kit does): turning P1 or P2 changes the next hits, a ringing hit keeps its
sound. An LFO on P1 or P2 therefore gives every hit of a pattern its own material or ring. Each drum track has two
voices: a third hit on the same track takes the older voice, which fades out in 2.9 ms (the OS starts the new hit
when it is free).

How it works: each partial is a sine (a 256-entry table, linearly interpolated, peak error −80 dBFS) with its own
phase increment (the note's increment × its ratio) and amplitude. The amplitude decays exactly per block (a
multiplier 2^−x per block, computed from the T60 with a table and a short series, to 2·10⁻⁷) and ramps linearly
across the block. The strike sets each partial's level once: weight × strike position × mallet low-pass
(1 / (1 + (f/fc)²)) × velocity, normalised so the levels sum to a fixed total per material (0.32 of full scale ×
a level trim of −0.6 … +2.8 dB that evens out the materials' peaks). There is **no recursion**, so no setting can make
a partial grow, and the amplitudes sum to at most 0.48 of full scale, the click adds at most 0.09: the output is
bounded by construction, no limiter is needed. Partials above 0.40 fs fade out and are silent from 0.45 fs, partials
below 30 Hz fade and are silent below 15 Hz; partials that never reach −90 dBFS are never started, those that decay
below it are skipped and cost nothing.

## Presets

All in slot 0 (indexes 0–4), the only slot drum tracks read; **marimba** (index 0) is the sound a drum track gets
when it switches to mallet ([presets.md](../../research/presets.md)). Hold the track button and press the
first five white keys to recall them.

| Index | Preset | P1 | P2 | Level | Sound |
|---|---|---|---|---|---|
| 0 | **marimba** | 32 | 128 | 200 | round, woody marimba with its natural ring |
| 1 | vibes | 96 | 170 | 195 | vibraphone, long and mellow, a slow beat |
| 2 | steel drum | 159 | 135 | 200 | steel pan: fundamental, octave and twelfth |
| 3 | bells | 191 | 150 | 190 | struck bells that ring on (try octave +1) |
| 4 | wood block | 0 | 110 | 205 | short wood block / log drum with a click |

Every preset: octave 0, envelope 0/0/255/0, filter 128, resonance 0, pan 128, sends 0, no LFO (depth 128, speed 100,
target 255, shape 0). Glock (P1 128, octave +1), xylophone (64), gong (223, octave −1) and glass (255) are a P1 turn
away. Names fit the OP-Z's 11 characters. `python3 tools/opz.py presets check engines/mallet` reports 5 presets,
0 errors, 0 warnings. Not yet tuned by ear on the device.

## What to listen for on the device

1. Put mallet on the perc track (marimba preset). Play every key from left to right: a chromatic two-octave scale,
   in tune with a synth track playing the same keys, each hit woody and round, ringing about a second.
2. Play softly and hard: soft hits darker, quieter and a little slower to speak; hard hits bright with a small click.
3. Turn P1 slowly while playing a repeated note: wood → marimba → xylophone → vibes → glock → steel drum → bell →
   gong → glass, each hit a different material, the pitch steady. Listen for the slow beating at vibes, gong and glass.
4. Turn P2 from 0 to 255 while playing: dead, muffled thuds at 0, the natural ring at 128, long bright rings at 255.
5. Press the octave buttons: −1 (F1–E3, try gong and bell), +1 (F5–E7, try glock and glass); at −2 / +2 the
   extreme notes thin out but must not alias or whistle.
6. Fast pattern: program 16ths (or 32nds with step components) on two drum tracks with mallet, then **turn P1 and P2
   fast over their whole range** while it plays, and set an LFO on P1 or P2: every hit should sound clean, no
   crackle, no stuck voice, no crash (the macro v3 situation).
7. Long rings and steals: bells or gong at P2 255 played fast on one track: each new hit should start at once (the
   older ring fades out quickly), never wait for the old one to end.
8. CPU: mallet on all four drum tracks with fast patterns and long rings, plus full chords on the synth tracks
   (runbook step 11): listen for crackles or dropped hits.

## Facts

| | |
|---|---|
| Plug | 61022, `trackmask` 0x0F (kick, snare, perc, sample), engine and plug version 2. The device does not enforce the mask. flags 16: P2 (ring, natural at 128) bipolar, a centre-detent cue ([host-tables-flags.md](../../research/host-tables-flags.md#recommended-flags-for-our-engines)) |
| Memory slot | 39 (10 KB class): 6,324 of 10,240 bytes on Blackfin (6,064 of code, tables and data, 260 of `.bss`: the shared 65-word partial buffer and the callback pointer). Tables: 1 KB sine, 1.1 KB materials, 260 B decay table, 48 B note increments |
| State | 200 B of 280 per voice: ten partials × (phase, increment, amplitude, decay), the click, the output level and the hit's knobs. No voice pool, `memRequest` 0 |
| Tuning | exact: the note's increment comes from a 12-entry table (within 1 ppm of 440 · 2^((n − 69)/12) for every note); the ratios are exact Q16 values at the material anchors and within 0.01 % between them |
| Level | peak −11.5 … −12.4 dBFS per hit at velocity 127 for the nine materials over the two octaves, −11.0 … −13.4 at the halfway points (kit's hits peak at −12); velocity 64: −7.6 dB, 20: −21 dB, 1: −50 dB. Two tracks of fast hits with the knobs moving peak at −6.9 dBFS |
| CPU | Blackfin simulator ([cycle-model.md](../../research/cycle-model.md), `python3 tools/opz_cycles.py engines/mallet`), cycles per sample per sounding voice: **114 with all ten partials ringing** (any material, P2 128/255), 32–111 at P2 0–128 as partials die and are skipped (marimba 64, wood 44); retriggers every block (every block a new hit) 149; 8 hits in one block (every drum voice) 116; P1/P2 moving every block with a hit every two blocks 126. **The worst block, a hit's first (strike and click), is 11,400–12,830 cycles (179–201 per sample)**, the second (the decays) a little less; 8 hits starting in one block ~102,000. Note-on 78 cycles. Every case is under the 218 budget in its worst block |

The partial loop (10 cycles per partial per sample) and the output loop (4 per frame) are engines/modal's
hand-scheduled Blackfin assembly; the C versions next to them are the same arithmetic: building the device engine
with `MALLET_C_LOOP` defined (a `#define` at the top of `mallet.c`) gives bit-identical output in all 63 benchmark
cases (`opz_cycles.py --dump` / `--compare`), and the host build (opzsim) uses the C. A hit's work is split over its
first two blocks: the ratios, increments and strike levels in the first, the decays in the second (1.45 ms with no
decay), which keeps the first block under the budget. `gen_tables.py` writes `mallet_tables.h`;
`tools/tests/test_mallet.py` fails if it is stale and checks the tuning.

Simulator checks (opzsim, `tests/`): seven golden scenarios (every key; the nine materials and the halfway points; P2
from dead to long; velocities on glock and marimba; the octaves on all four drum tracks with notes 0 and 127; P1 and
P2 swept fast during a 16th-note pattern on two tracks; fast retriggers of long rings that steal voices); all pass the
host's state guard and "adds into out" probe and free every voice. `tests/fuzz.c` (UBSan with array bounds; how to
run it is in its header) drives eight voices for 10⁶ blocks with random note-ons (notes −300 … 427, velocity 0–255,
params NULL), steals at any moment, note-offs, re-inits, stages set at random, NULL params in render, random pitch
words, P1/P2 words beyond the host's range and knob jumps every block, then plays a grid of P1 × P2 × note ×
velocity until every hit ends (10.5 million blocks; the longest hit 54 s, at P2 255): no sanitizer report,
no voice above 0.6 of full scale, the amplitudes non-negative and within their bound, stolen voices freed within
three blocks, every hit ends, and the tuning of every note within 1 ppm.

## Credits

Original code. The design follows the modal synthesis literature (Adrien's modal representation; Cook's modal bar
models; the mode ratios of the free bar, the tuned marimba, xylophone and vibraphone bars, the steel pan, the church
bell and the wine glass after Fletcher and Rossing, *The Physics of Musical Instruments*; the wood block and gong
sets are our own), in the spirit of the modal resonators of Mutable Instruments' Rings, Elements and Plaits (MIT
licence). No code from them is used: they run banks of resonant filters, this engine runs decaying partials. The
partial and output loops, the sine table and the exp/reciprocal helpers come from engines/modal (same project). No
TE code. MIT licence.
