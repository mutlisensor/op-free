# hats

Metallic hi-hats and cymbals for the kick, snare, perc and sample tracks, synthesized the TR-808 way: six detuned
square waves plus a little noise, through a band-pass/high-pass filter and a two-stage decay. Each of the 24 keys
plays its own hat or cymbal — closed, pedal, half-open and open hats in four colours, a tick, rides, crashes, a
splash, chinas, a sizzle, a reversed-cymbal swell and a bell — and the closing hats choke the open ones, as on a real
hi-hat. Put it on the perc track next to `kit`, or on two drum tracks for hats and cymbals with their own levels.

## Keys

The first key is F3 (MIDI 53): key = (note − 53) mod 24. White keys in **bold**.

| Key | Note | Sound | Squares | Choke | Length at P2 128 (0 / 255) |
|---|---|---|---|---|---|
| 1 | **F3** | closed hat — the TR-808 one | 808 | closes | 0.13 s (0.04 / 0.41) |
| 2 | G♭3 | tight hat — short, noisier, brighter | 808 | closes | 0.06 s (0.02 / 0.19) |
| 3 | **G3** | pedal hat — the foot "chick": a 4 ms soft attack, darker | 808 | closes | 0.13 s (0.05 / 0.42) |
| 4 | A♭3 | half-open hat | 808 | closes, is cut | 0.36 s (0.12 / 1.2) |
| 5 | **A3** | open hat | 808 | is cut | 0.94 s (0.28 / 3.4) |
| 6 | B♭3 | open hat, long (loose) | 808 | is cut | 1.9 s (0.57 / 6.8) |
| 7 | **B3** | dark closed hat (lower squares and band, more noise) | 808 | closes | 0.14 s |
| 8 | **C4** | dark open hat | 808 | is cut | 0.99 s |
| 9 | D♭4 | glass closed hat (higher squares, resonant band at 11 kHz) | 808 | closes | 0.11 s |
| 10 | **D4** | glass open hat | 808 | is cut | 0.89 s |
| 11 | E♭4 | ring closed hat — squares multiplied in pairs: clangier | 808, ring | closes | 0.13 s |
| 12 | **E4** | ring open hat | 808, ring | is cut | 0.94 s |
| 13 | **F4** | tick — a 40 ms click of a hat | 808 | closes | 0.04 s (0.01 / 0.16) |
| 14 | G♭4 | ride — a ping, then the wash | cymbal | — | 3.8 s (1.9 / 7.7) |
| 15 | **G4** | ride bell | bell | — | 2.5 s (0.6 / 9.9) |
| 16 | A♭4 | dark ride (jazz) | cymbal, ring | — | 3.1 s |
| 17 | **A4** | crash | cymbal | — | 2.6 s (1.3 / 5.2) |
| 18 | B♭4 | big crash | cymbal, ring | — | 3.9 s (2.0 / 8.0) |
| 19 | **B4** | splash | cymbal | — | 0.84 s |
| 20 | **C5** | china — trashy, low band | cymbal, ring | — | 1.9 s |
| 21 | D♭5 | china, short (stack) | 808, ring | — | 0.58 s |
| 22 | **D5** | sizzle cymbal (rivets: lots of noise) | cymbal | — | 3.7 s |
| 23 | E♭5 | swell — a reversed cymbal: rises for 0.9 s, then stops | cymbal | — | 0.96 s (0.24 / 3.8) |
| 24 | **E5** | bell (agogo/cowbell-like, ringing) | bell, ring | — | 1.2 s (0.3 / 4.8) |

Lengths are to −60 dB below the hit's peak. Square sets (frequency ratios at P1 128): **808** 1, 1.48, 1.80, 2.55,
2.63, 3.90 (the TR-808's 205, 304, 370, 523, 540 and 800 Hz); **cymbal** 1, 1.17, 1.61, 2.09, 2.83, 3.37 (wider,
more inharmonic); **bell** 1, 2, 2.41, 2.76, 3, 4.07 (near-harmonic). **Octave presses** on the drum track move the
squares and the band by ⅓ octave per press (two presses each way; note numbers beyond are clamped to ±2).

## Controls

| Control | Function |
|---|---|
| P1 **tone** | dark/trashy at 0 → bright/glassy at 255, continuous; 128 is each key's nominal sound. Across 0 → 255: the squares' pitch ×0.59 → ×1.68 (±¾ octave, exponential), their spread from ratio^0.5 (clustered: beating, trashy) to ratio^1.5 (spread wide: glassy), the band's centre −1 → +1 octave, its resonance −0.2 → +0.2 (more ring at the bright end). Example: the closed hat's spectral centroid is 9.2 kHz at 0, 10.0 kHz at 128, 15.0 kHz at 255 |
| P2 **decay** | the length, and with it how open a hat sounds. Each sound has two decay stages (a first rate down to a knee, then a second): P2 scales the first stage ×¼ at 0 → ×1 at 128 → ×4 at 255 and the second ×½ → ×1 → ×2 (both exponential), so low P2 gives a tight, choked ending and high P2 a loose hat and long cymbal washes. On the swell and pedal hat it also scales the rise ×¼ … ×4 |
| velocity | level (linear: 64 is −6 dB) and brightness: the band moves down by up to ¾ octave on soft hits (closed hat centroid 10.0 kHz at 127, 8.6 kHz at 60, 8.1 kHz at 30) |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |
| attack … release, LFO | not used by the engine (the presets turn the LFO off: depth 128) |

Knobs are read when a hit starts, as in `kit`: turning P1 or P2 changes the next hits, not the one ringing.

**Choke.** Keys 1–4, 7, 9, 11 and 13 (closed, tight, pedal, half-open, dark, glass, ring, tick) close the hi-hat:
they cut every open or half-open hat (keys 4–6, 8, 10, 12) ringing **on the same track**, which fades to silence in
~30 ms. Cymbals are never choked, and hats on another track ring on (put hats and cymbals on different tracks to
keep them apart; drum tracks have two voices each).

## How it works

- **Squares as edges, not phases.** Each square is a list of edge times half a period apart (Q16 samples, random
  start phase per hit, like the 808's free-running oscillators). Every edge drops its ±step into a per-block
  difference buffer through a quadratic B-spline of its fractional time (weights (1−f)²/2, the rest, f²/2 over
  three samples), and one running sum turns the steps into the six squares' sum. That is the ideal square filtered
  by a two-sample triangle: images are attenuated by sinc², deepest near multiples of fs, so the low aliases that
  whistle are gone. [`tests/alias.c`](tests/alias.c), one square at 2–11 kHz: strongest non-harmonic component
  below 4 kHz −57 … −127 dB (a naive square: −14 … −36 dB), below 11 kHz −43 … −59 dB (naive −14 … −24 dB); the
  remaining −43 dB images fold to 8–11 kHz where the hats' band is full of real harmonics ~30 dB louder. Cost: a
  few cycles per edge instead of per sample per square. In **ring** keys the squares are multiplied in pairs
  (each edge flips its pair's product), the TR-808 cymbal's clang.
- **Filter.** Squares + white noise → one zero-delay-feedback state-variable filter at the key's band centre; the
  output mixes its band-pass and high-pass outputs, then a one-pole high-pass an octave below the band steepens the
  low skirt (the squares' fundamentals) to 12 dB/octave. Coefficients are computed once per hit (two 64-bit
  divisions; the last four settings are cached, so steady knobs cost ~2,450 cycles per hit instead of ~5,000).
- **Envelope.** Exact per block (a per-sample one-pole coefficient from `opz_decay_coef`, raised to the 64th power)
  and ramped linearly across the block; stage 1 hands over to stage 2 at the key's knee (−6 … −30 dB, checked per
  block). A swell rises as (t/T)⁴, a reversed decay's slow start. Steals (the host's stage 5/6) fade in 2.5 blocks;
  a voice frees itself at −60 dB.
- **Level.** Calibrated in opzsim to `kit`'s hats: RMS over the first 30 ms at velocity 127 is −25 dBFS for the
  hats (kit −24 … −25), −26 … −27 for the cymbals (kit's cymbal −29); peaks −12.1 … −17.7 dBFS. The output is
  capped by construction at OPZ_VOICE_PEAK (−12 dBFS): a saturating ×16 on the filter mix, then the envelope and a
  shift by 2. At P1 255 a few onset samples (≤ 8 per hit) of four keys touch the cap.

## Presets

All in slot 0, the only slot drum tracks read; index 0 (**808**) is what a drum track gets when it switches to hats.
LFO off (depth 128, speed 100, target 255, shape 0), filter neutral, level 200.

| Index | Preset | P1 | P2 | Sound |
|---|---|---|---|---|
| 0 | **808** | 128 | 128 | the nominal kit: TR-808 hats, rides and crashes |
| 1 | tight | 150 | 64 | short, crisp, slightly brighter: fast closed patterns |
| 2 | trashy | 40 | 150 | dark, clustered, beating: lo-fi hats and dirty chinas |
| 3 | glassy | 224 | 112 | bright, resonant, spread squares |
| 4 | washy | 104 | 216 | long, loose hats and big cymbal washes |

## What to listen for on the device

1. Install on the perc track, preset **808**. Play every key once (white keys, then black): 24 different hats and
   cymbals, none silent, none much louder than the rest; nothing whistles or hums underneath.
2. Hold a 16th pattern of key 1 (closed hat) with key 5 (open hat) on the off-beats: each closed hat must cut the
   open hat at once (a real hi-hat), not let it wash under the next hits.
3. Play key 5 on perc and key 1 on another drum track: the open hat must ring on (the choke is per track).
4. Turn P1 slowly from 0 to 255 under that pattern: from dark and trashy (beating, lower) to bright and glassy, with
   no step where the sound jumps or drops out; at 255 listen for an aliasing whistle (there should be none).
5. Turn P2 from 0 to 255: from choked ticks to loose, sloshy hats; the crash (key 17) from a short splash to a long
   wash.
6. Play soft and hard (velocity): soft hits quieter and darker.
7. **Fast pattern, both knobs:** 16ths (or 32nds) at a high tempo across keys 1, 5, 13, 14, turning P1 and P2 fast
   and together, back and forth for 30 s — no crash, no click, no hang (the macro v3 situation); hits keep starting
   at once (two voices per drum track: older hits are stolen and fade in ~3 ms).
8. Key 23 (swell) at P2 255: a 3.6 s rise that stops dead; key 15 (ride bell) at P2 255 rings ~10 s and then ends.
9. Octave presses up and down on keys 1 and 17: the squares and band move by a third of an octave per press.

## CPU and size

Blackfin, GNU simulator (`tools/opz_cycles.py engines/hats`, [cycle-model.md](../../research/cycle-model.md)),
cycles per sample per sounding voice, mean (worst block):

| Case | Mean | Worst |
|---|---:|---:|
| any key, P1 128, P2 128 | 116–131 | 133 |
| any key, P1 255, P2 255 (most edges) | 126–168 (ride bell, bell) | 171 |
| any key, P1 0, P2 0 | 112–117 | 120 |
| 24 voices, every key, octave +2, P1 255, all struck in one block | 159 | 204 |
| 19 voices, sizzle, octave +2, P1 255 | 162 | 163 |
| 8 voices (four drum tracks × 2), glass open, P1 255 | 145 | 147 |
| choke (open hat cut by a closed hat) | 119 | 122 |
| knobs moving every block (`knob_cases`, period 8 / 40) | 119 / 119 | 122 |

Every case within the 218/sample budget (19 voices); a hit's first block costs the same as the others (all per-hit
work is in `onNoteOn`). Note-on ~5,000 cycles (~2,450 when the filter setting is cached). 4,316 of 5,120 bytes in
slot 76 (380 B `.bss`: the 66-word difference buffer, the filter cache, the choke counters); 144 of 280 B of voice
state; libgcc's `__muldi3` only (per note). No division, float or 64-bit math in `render`.

## Checks

- `make -C sim test`: 5 golden scenarios ([tests/](tests/)) — all24 (every key, steals), choke, fast (16ths and 32nds
  at 400 BPM over two tracks), knobs (P1/P2 swept through a 16th pattern, and jumping 0 ↔ 255 every step), extremes
  (knob corners, MIDI 0 / 127, octaves ±2, velocity 1, swell at P2 255). Peaks −8.5 … −11.9 dBFS, all voices freed.
- [`tests/fuzz.c`](tests/fuzz.c) under UBSan with bounds checks: 2,000,000 random blocks × 8 voices (2 million
  note-ons with any int note, velocities 0–255, NULL params, many hits per block, steals, stage jumps, re-inits,
  knob words beyond the host's range), then every key × 10 knob corners × 5 octaves until it frees its voice
  (longest 10 s), then every key under a fast knob sweep (edge-time, running-sum, peak and freeing invariants
  checked after every call), then 40 million blocks of torn states (below): 0 failures.
- **Torn states** (`tools/opz_crashcheck.py engines/hats`, and `--full`: 0 errors, 0 warnings; stack: render 100 B,
  onNoteOn 160 B). The OS starts a voice before it calls onNoteOn from a task, so render can run on a half-written
  state. The first build hung there: a half period of 0 (the cleared state) next to `active = 1` (the new hit) made
  the edge loop spin forever, which on the device freezes the OP-Z. render now clamps every half period to 2 samples
  … 2^14 samples and every edge time into its half period (the loop ends within 33 edges per square), indexes only
  with checked fields, sums the squares with saturation (the difference buffer is unsigned), floors the swell's rise
  rate and ends any hit after 16 s (the longest is 10 s). Normal hits render bit-identically (goldens unchanged).
- [`tests/alias.c`](tests/alias.c): the aliasing figures above (exit 1 on a regression).

## Credits

Original code (MIT licence, see [CONTRIBUTING.md](../../CONTRIBUTING.md)) following the TR-808 hi-hat and cymbal
circuits — six square oscillators, band-pass, a decay that changes rate part-way — as also explored by the hi-hat
model of Plaits (Emilie Gillet, MIT licence). No code is copied.
