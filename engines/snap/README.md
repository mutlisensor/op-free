# snap

Noise-burst percussion for the kick, snare, perc and sample tracks — claps, finger snaps, rimshots, sticks, clave,
woodblock, shakers, maracas, tambourines, cabasa, brushes, a slap, a guiro and a gated clap. No samples: each of the
24 keys plays its own model of filtered noise bursts, a tail and up to two resonant bodies. Put it next to `kit` (or
TE's kick and snare) on a perc or sample track for the hand percussion that kit only has one or two of.

## Keys

The first key is F3 (MIDI 53): key = (note − 53) mod 24, as in kit and TE's drum players.

| Key | Note | Sound | What it is |
|---|---|---|---|
| 1 | F3 | **clap 909** | four bursts 10 ms apart into a 1.1 kHz band-pass, short room tail |
| 2 | F♯3 | **clap 808** | three bursts 11 ms apart, darker (950 Hz), longer tail |
| 3 | G3 | **tight clap** | two close bursts, brighter, short |
| 4 | G♯3 | **big clap** | four bursts and a long, swelling room tail |
| 5 | A3 | **handclaps** | six loose hands spread in time and stereo |
| 6 | A♯3 | **crowd** | twelve hands smeared together, wide, with a tail |
| 7 | B3 | **snap** | finger snap: one sharp band-passed burst and a 2.2 kHz crack |
| 8 | C4 | **bright snap** | a harder, higher snap (3.8 kHz, 3 kHz crack) |
| 9 | C♯4 | **rim 909** | two resonators (720 Hz, 1.7 kHz) and a high-passed click |
| 10 | D4 | **rim 808** | resonators at 455 Hz and 1.67 kHz, the classic rimshot |
| 11 | D♯4 | **side stick** | woody cross-stick: resonators at 380 Hz and 1.05 kHz with a short click and tail |
| 12 | E4 | **sticks** | two drumsticks clicked together (2.4 and 3.9 kHz) |
| 13 | F4 | **clave** | a 2.5 kHz ringing resonator, 30 ms |
| 14 | F♯4 | **woodblock** | 1.05 and 1.65 kHz, hollow |
| 15 | G4 | **shaker** | high-passed noise with a 12 ms swell, 45 ms decay |
| 16 | G♯4 | **maracas** | a shorter, sharper shake |
| 17 | A4 | **tambourine** | three jingle bursts into a resonant 6.8 kHz band, ringing tail |
| 18 | A♯4 | **tamb shake** | five slower bursts, a shaken tambourine |
| 19 | B4 | **cabasa** | very bright, crisp, short |
| 20 | C5 | **brush hit** | a brush slap on a snare: broad 3.2 kHz noise and a soft 190 Hz body |
| 21 | C♯5 | **brush swish** | a 70 ms swell of brush noise, no attack |
| 22 | D5 | **slap** | a hand slap: low-passed noise and a 160 Hz thump |
| 23 | D♯5 | **guiro** | ten bursts 14 ms apart: a scrape |
| 24 | E5 | **gated clap** | four bursts into a tail that holds 120 ms and cuts off (80s gated room) |

## Controls

Knobs are read when a hit starts (like kit): turning them changes the next hit, never one that is sounding.

| Control | 0 | 128 | 255 |
|---|---|---|---|
| P1 **spread** | **tight**: two bursts fewer (never below one), bursts half as far apart, no timing, level or stereo jitter (mono) | the model as listed above | **loose**: two bursts more (single hits become a flam at P1 ≥ 154 and a drag at ≥ 205), bursts twice as far apart, heavy jitter in timing, level and stereo position ("more hands") |
| P2 **decay** | **short, dry, dark**: tail ×¼ (also its hold), tail level ×0.75, bursts ×0.71, bodies ×0.5, noise filter −¾ octave | the model as listed above | **long, roomy, bright**: tail ×4, tail level ×1.25, bursts ×1.41, bodies ×2, filter +¾ octave |
| velocity | — | — | level (linear); softer hits are up to ½ octave darker and their bursts quieter against the tail (×0.5 at velocity 0) |
| octave buttons | | | each press moves the noise filter and the bodies by one octave (−2 … +2 presses; filter 30 Hz … 7.2 kHz, bodies up to 16 kHz) |
| filter, resonance | not used: the OS's own track filter runs after the engine. Presets leave it neutral (128, 0) | | |
| attack, decay, sustain, release | not used: every hit has its own envelopes | | |

In detail, P1 changes the burst count by −2 (P1 0–51), −1 (52–102), 0 (103–153), +1 (154–204) or +2 (205–255); the
spacing by 2^((P1 − 128) / 128); and the jitter to the model's jitter × P1 / 128 plus (P1 / 255)² × 0.375, which sets
the timing spread (± that fraction of the spacing), the level spread (down to −2.5 dB) and the stereo spread (± that
fraction of full balance) of each burst. P2 scales times by 2^(4 (P2 − 128) / 128) (tail and hold),
2^((P2 − 128) / 128) (bursts) and 2^(2 (P2 − 128) / 128) (bodies); the filter moves by 1.5 (P2 − 128) / 128
octaves. Each hit ends itself at −60 dB and frees its voice; when the OS takes a voice back for a new hit, the old one
fades out in ~5 ms.

## Presets

All in slot 0 (indexes 0–4), the only slot drum tracks read; **studio** (index 0) is the sound a drum track gets
when it switches to snap. Not tuned by ear on the device yet.

| Index | Name | P1 | P2 | Level | Idea |
|---|---|---:|---:|---:|---|
| 0 | studio | 128 | 128 | 200 | every model as designed |
| 1 | tight | 50 | 80 | 200 | close bursts, mono, shorter |
| 2 | big room | 140 | 210 | 195 | long bright tails |
| 3 | live hands | 215 | 150 | 200 | loose, wide, flams on the rims and sticks |
| 4 | dry | 100 | 30 | 210 | short and dark, almost no tail |

## What to listen for on the device

1. Install on the snare or perc track (`opz.py install engines/snap --track perc`) and play the 24 keys at P1/P2 128:
   every key sounds, nothing rings on, and the clap and rim sit at about the level of kit's clap and rim.
2. Hold the clap 909 (F3) on a 16th pattern and turn **P1** from 0 to 255: one tight, centred clap → the designed
   clap → a loose, wide group of hands that spreads across the stereo field.
3. On the rims and sticks (C♯4–E4), P1 above ~155 doubles each hit into a flam (two strikes ~20–35 ms apart),
   above ~205 into a drag (three): every strike must be audible.
4. Turn **P2** from 0 to 255 on the big clap and the tambourine: short, dry and dark → long, roomy and bright; at
   255 the tambourine rings for ~4 s.
5. Play soft and hard (velocity): soft hits are quieter and darker, hard hits snap.
6. Press octave up and down on the clave and rim: the pitch of the body follows (two presses each way).
7. **Fast knobs:** program a fast pattern (16ths or 32nds of clap, rim, shaker and crowd), then turn P1 and P2 quickly
   back and forth together, for a minute. No crash, no clicks or stuck voices; every hit takes the knobs as they
   were when it started.
8. Fill a bar with 32nd-note big claps at P2 255 on one track: hits steal voices; each new hit must start at once
   (the stolen one fades out in a few ms) and nothing blocks.

## Building blocks

Per voice: an LCG noise source into one Chamberlin state-variable filter (band-, high- or low-pass; coefficients
from a Taylor series once per hit), a burst envelope restarted at every burst (one-pole decay), a tail envelope with
linear attack, hold and one-pole decay (stages switched per block), and two two-pole resonators (r, ω from the
model, struck with g·sin ω so their amplitude is g). A per-model boost drives the filtered noise into a saturating
stage before the envelopes, which raises loudness without raising peaks; the whole voice saturates once at
OPZ_VOICE_PEAK (−12 dBFS). The burst timer splits each block into runs, so the inner loops have no event branches;
models without a body (or once it has died away) run a cheaper loop.

## Facts

| | |
|---|---|
| Plug | 61025 v2 (engine v2), `trackmask` 0x0F (kick, snare, perc, sample), flags 24: P1 and P2 bipolar, a centre-detent cue at 128 ([host-tables-flags.md](../../research/host-tables-flags.md#recommended-flags-for-our-engines)) |
| Memory slot | 74 (5 KB class): **5,016 of 5,120 bytes** on Blackfin (code and constants, no `.bss`; `onNoteOn` and the per-burst code at `-Os`) |
| State | 152 B of the 280 B usable per voice; no globals but the callback pointer |
| Level | opzsim, velocity 120, P1/P2 128: every hit peaks at −12.0 dBFS or below; loudest 50 ms RMS: claps −19 … −23 dBFS (kit's clap −20.3), rim 909 −20.0 and rim 808 −24.5 (kit's rim −19.6), clave −18.6 (kit −16.7), shaker −20.5 (kit −20.0) |
| CPU | Blackfin simulator (`opz_cycles.py engines/snap`), per voice: **noise-only models 93–102 cycles/sample** (claps, crowd, shakers, tambourines, guiro, brush swish); **models with bodies 153–163** (rims, sticks, clave, woodblock, slap; snaps and brush hit 105–153 as their body dies away first); worst block 169 (a burst with bodies). 24 voices at once (every key, P1/P2 255): 121 mean, 169 worst; retriggering 4 sounding voices every block: 111 mean. Knobs moving every block: 104–106. Note-on 2,100–3,300 cycles. All within the 218/sample 19-voice budget |
| Checks | 6 golden scenarios (all24, spread, decay, octaves, knobs, steal); `tests/fuzz.c` under UBSan + bounds: 10⁶ blocks × 8 voices of random hits, retriggers, NULL params, out-of-range notes, velocities and knob words, host fades and stage jumps, plus a P1 × P2 × note grid (11.3 M blocks): no report, no sample above −12 dBFS, every hit ends (≤ 4,500 blocks at P2 255, ≤ 12 blocks after a fade) |

## Credits

Original code following the classic analog clap and rimshot circuits (TR-808/909: repeated noise bursts into a
band-pass with a reverb tail; bridged-T resonators), as also explored by Mutable Instruments Plaits' and Peaks' noise
and drum models (MIT). No code was copied from them. MIT licence.
