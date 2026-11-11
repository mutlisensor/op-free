# pd

A phase-distortion voice in the tradition of Casio's CZ synthesizers, for the bass, lead, arp and chord tracks: a
cosine read through a bent phase. Turned down it is a pure sine; turned up it becomes a saw, square, pulse, double
sine, saw-pulse or one of three "resonance" waves that sound like a resonant filter swept over a saw, with no filter
at all. Every note sweeps its distortion down from a brighter start, like the CZ's DCW envelope, so plucks and basses
"bwow" the way CZ patches do. The OP-Z's own track filter (the filter and resonance knobs) shapes the tone after it.

| Control | Function |
|---|---|
| P1 **distortion** | how far the wave is bent, from **0 = a pure sine on every wave** to **255 = the full wave**. The sharpness s (how much faster than the note the fastest part of the wave runs; the resonance frequency in multiples of the note on the resonance waves) grows exponentially with P1: **s = s_top^(P1/255)**, where s_top is the wave's largest sharpness (128 for saw, square and saw-pulse, 12 for pulse, 32 for resonance) or, on higher notes, the pitch limit (below), whichever is lower. So the whole knob always does something, from sine to "as bright as is clean at this pitch". At C3 on the saw: P1 64 → s 2.6, 128 → 6.8, 192 → 17.8, 255 → 46 (the fast half-cycle at 6 kHz); at C1 it reaches 128, at C7 2.9. On the resonance waves the first quarter of the amount (P1 0–64 with no DCW) crossfades from the sine into the resonance. Smooth everywhere: every shape parameter ramps per sample across the block, so P1 can be swept by hand or by the LFO without zipper |
| P2 **wave** | eight waves, each holding over a plateau, with a crossfade between neighbours (both are computed from the same phase, so turning P2 never clicks): **0–9 saw, 28–45 square, 64–81 pulse, 101–118 double sine, 137–154 saw-pulse, 174–191 resonance I, 210–227 resonance II, 246–255 resonance III**; the values between two plateaus crossfade the two neighbours linearly. At P1 0 all eight are the same sine, as on the CZ |
| velocity | level (linear), and the **DCW depth**: a note starts at distortion P1 + (255 − P1) × velocity / 127 and decays to P1 (velocity 127 starts every note at the full wave, velocity 0 doesn't sweep) |
| decay | sets the DCW time as well as the envelope's decay: the distortion decays exponentially with a time constant of **15 ms at decay 0, doubling every 32 steps** (60 ms at 64, 240 ms at 128, 0.96 s at 192, 3.7 s at 255), at C4; **half that two octaves up, twice that two octaves down**. It holds while the envelope is in its attack, so slow attacks swell in bright (brass) and then settle. With sustain 255 the decay knob only sets the brightness sweep |
| notes | each note restarts the phase at ¾ of a turn (a sine starts at its zero crossing). Pitch bend, portamento and the LFO's pitch move the pitch; the pitch limit follows them |
| attack, sustain, release | the OP-Z envelope; the DCW keeps decaying through the release |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |
| pan, level | the track's pan and level (yellow and red dial, orange page), applied by the engine as TE's engines do (`opz_mix.h`): level is linear (128 = −6 dB, 0 = silent), pan a balance (centre: both sides full; hard left: right silent). The FX1/FX2 sends are taken after them. They follow the LFO (targets pan and volume) without zipping |

## The waves

Phase φ runs from 0 to 1 over a period; each wave reads cos(2π θ) through a bent phase θ(φ). The bends of the first
five sit where the cosine is flat (θ a multiple of ½), so the waves are smooth up to the first derivative and their
harmonics fall at 18 dB/octave above s × f0 instead of 6 dB as a raw saw's do.

| P2 | Wave | Full distortion (sharpness s) | Character |
|---|---|---|---|
| 0 | **saw** | the first half-cycle (1 → −1) squeezed into 1/(2s) of the period, the second half-cycle (−1 → 1) spread over the rest | the CZ saw: a falling edge and a slow cosine rise; brass, basses |
| 36 | **square** | in each half period, a half-cycle in 1/(2s) of the period, then holding at −1, then at +1 | hollow, woody; reed leads, organ |
| 73 | **pulse** | a whole cycle in 1/s of the period, then holding at +1 (a narrow dip; the DC is removed) | thin and nasal; 8 % wide at s 12. Quieter at full P1: −9 dB RMS against the saw (its peaks are already at the limit, below) |
| 109 | **double sine** | θ = φ + a (frac(2φ) − φ), a = P1 / 255: the sine bends into two dips per period and, at 255, into the octave | sine → organ-like hollow two-dip wave → octave; P1 sweeps the note up an octave in timbre |
| 146 | **saw-pulse** | the saw's fast half-cycle, the slow half-cycle in half a period, then holding at +1 | a saw that "rests" each period: buzzy, vocal |
| 182 | **resonance I** | 1 − W(φ)(1 − cos(2π k φ)), k = s, under a decaying window W = (1 − φ)² | a resonant low-pass sweep on a saw: the classic CZ resonance |
| 219 | **resonance II** | the same under a bell window W = (4φ(1 − φ))² | a softer, rounder resonance (band-pass-like) |
| 255 | **resonance III** | the same under a window that is flat for the first half and falls smoothly (smoothstep) in the second | the brightest, most "sync"-like resonance |

The resonance waves are a cosine at k times the note, restarted every period under a window that ends each period
at zero (with zero slope), so the restart never clicks: the resonance frequency k f0 sweeps like a filter's cutoff,
in tune or not, without hard-sync aliasing.

**Levels.** Every wave's mean is removed (from a table, so nothing is left for the OS's DC blocker to chase) and its
RMS matched to the sine's, unless that would take its peak more than 3 dB over the sine's (−9 dBFS per voice at
velocity 127): saw, square, double sine, saw-pulse and resonance III play at the sine's level throughout; resonance
II is up to 2 dB quieter, resonance I up to 5.5 dB, the narrowest pulse 9 dB (they are all peaks).
[gen_tables.py](gen_tables.py) computes the tables (`--stats` prints the levels).

**Anti-aliasing.** The sharpness is limited so that s × f0 stays at or below 6 kHz (pulse: 4 kHz; resonance: k × f0
≤ 6 kHz): high notes get gentler waves instead of aliasing. Double sine's kink in the middle of the period gets a
two-sample polyBLAMP (an integrated polynomial BLEP), which lowers its aliasing by 14–21 dB from C6 up. Measured on
the engine's own code ([tests/alias.c](tests/alias.c), held notes, Blackman-Harris window), the strongest component
that is not a harmonic, relative to the strongest harmonic, below 11 kHz, at P1 255 (double sine at its worst, P1
150):

| Note | saw | square | pulse | double sine | saw-pulse | res I | res II | res III |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| C1 (24) | −98 | −96 | −95 | −93 | −97 | −95 | −94 | −94 |
| C3 (48) | −82 | −79 | −90 | −94 | −81 | −84 | −95 | −91 |
| A4 (69) | −71 | −69 | −66 | −93 | −71 | −68 | −96 | −73 |
| C6 (84) | −64 | −61 | −65 | −78 | −64 | −60 | −91 | −66 |
| C7 (96) | −59 | −55 | −64 | −65 | −59 | −55 | −81 | −61 |
| C8 (108) | −56 | −51 | −95 | −51 | −56 | −51 | −56 | −62 |

(dB; without the polyBLAMP double sine measures −44 at C7 and −32 at C8.)

## How it works

Per block: the distortion (P1 plus the DCW), the P2 position and the pitch give each of the (at most two) waves its
shape at the start and the end of the block: knee, slope, resonance k, from `opz_exp2_q16` and a per-wave pitch
limit in log2. Each wave then has its own short loop that computes the bent phase from the block's phase, reads a
512-point cosine table (linear interpolation, within 2·10⁻⁵) and writes 64 16-bit samples, ramping only its shape
parameters. One output loop mixes the one to three buffers (two waves, and the sine the resonance waves start from)
with their gains (table gain × crossfade weight × envelope × velocity, ramped), subtracts the DC (ramped) and adds
the result into both channels. The host envelope is advanced once per block in closed form, as in `vowel`, and
applied as a ramp. The saw's slow half-cycle slope is computed from the fast half's quantised slope, so the saw
always lands on exactly one turn at the end of the period. No division, no 64-bit maths, no floating point; one
32-bit multiply per sample at most.

## Presets

Five per synth track. `slot` in `presets.json` is the track (0 bass, 1 lead, 2 arp, 3 chord); index 0 of each is the
sound a track gets when it switches to pd.

| Track | Preset | Octave | P1 | P2 (wave) | A | D | S | R | Level | Sound |
|---|---|---|---|---|---|---|---|---|---|---|
| bass | **cz bass** | −2 | 70 | 0 saw | 0 | 45 | 120 | 25 | 205 | plucky saw bass that sweeps down on every note |
| bass | reso bass | −2 | 90 | 182 res I | 0 | 55 | 90 | 25 | 200 | resonant "filter" bass, wow on hard notes |
| bass | square bass | −2 | 60 | 36 square | 0 | 60 | 150 | 30 | 200 | woody, hollow |
| bass | pulse bass | −2 | 110 | 73 pulse | 0 | 35 | 110 | 25 | 210 | nasal, punchy |
| bass | sine bass | −2 | 15 | 0 saw | 0 | 70 | 220 | 30 | 210 | almost a sine, a click of saw on hard notes |
| lead | **cz lead** | 0 | 140 | 146 saw-pulse | 0 | 90 | 210 | 50 | 190 | buzzy, vocal lead |
| lead | reso lead | 0 | 150 | 219 res II | 0 | 100 | 200 | 50 | 190 | singing resonance |
| lead | brass | 0 | 110 | 0 saw | 30 | 80 | 200 | 40 | 190 | swells in bright, settles |
| lead | double lead | 0 | 130 | 109 double sine | 0 | 90 | 210 | 45 | 195 | hollow, organ-like |
| lead | hollow lead | 0 | 170 | 36 square | 0 | 85 | 200 | 45 | 185 | reedy square |
| arp | **pd pluck** | 0 | 40 | 0 saw | 0 | 30 | 0 | 30 | 200 | short saw pluck, mellow tail |
| arp | reso pluck | 0 | 60 | 255 res III | 0 | 35 | 0 | 30 | 195 | resonant zap |
| arp | pulse arp | 0 | 80 | 73 pulse | 0 | 30 | 0 | 30 | 205 | thin, clicky |
| arp | glass arp | +1 | 60 | 109 double sine | 0 | 40 | 30 | 40 | 200 | bell-like octave ping |
| arp | sync arp | 0 | 120 | 182 res I | 0 | 30 | 40 | 30 | 195 | sync-like sweep |
| chord | **cz strings** | 0 | 60 | 0 saw | 90 | 140 | 220 | 110 | 160 | slow saw strings |
| chord | pd piano | 0 | 40 | 109 double sine | 0 | 110 | 60 | 60 | 165 | CZ-style e-piano |
| chord | reso pad | 0 | 80 | 219 res II | 100 | 160 | 220 | 120 | 160 | slowly breathing resonance pad |
| chord | pd organ | 0 | 30 | 36 square | 0 | 60 | 255 | 20 | 155 | soft organ, velocity sets the brightness |
| chord | brass pad | 0 | 100 | 146 saw-pulse | 60 | 120 | 200 | 90 | 160 | brassy pad |

Every preset: filter 128, resonance 0, pan 128, sends 0, no LFO (depth 128, speed 100, target 255, shape 0; an LFO
on P1 makes any of them wah, on P2 morph between waves). Chord presets sit lower in level because four voices add
up. Names fit the OP-Z's 11 characters. `opz.py presets check engines/pd`: 20 presets, 0 warnings. Not yet tuned by
ear on the device.

## Facts

| | |
|---|---|
| Plug | 61200, `trackmask` 0xF0 (bass, lead, arp, chord), version 2 (v2: applies the track's level and pan) |
| Memory slot | 42 (10 KB class): 9,488 of 10,240 bytes on Blackfin (6,878 B code, 2,166 B tables, 56 B exports, 388 B `.bss`: three 64-sample block buffers shared by all voices) |
| State | 32 B of 280 per voice; `memRequest` 0; no libgcc |
| CPU | Blackfin simulator ([cycle-model.md](../../research/cycle-model.md), `tools/opz_cycles.py`), 4-note chord C3 G3 C4 E4 at velocity 127, mean / worst block per voice in cycles per sample. **v2: level and pan add ~1.5 to every case** with the hand-scheduled `opz_mix.h` loops (its 11 cases: 74–137 mean, 159 worst block with P1 and P2 moving every block; v1 73–136 / 158; 9 more with the C loops); the per-case figures that follow are v1's: **one wave 59–83** (pulse 59, square 64, res I 63, saw 73, saw-pulse 71, res II 73, res III 77, double sine 82 / 84); **two waves crossfading (P2 between plateaus) 114–132 / 137**; the dearest case, resonance II/III crossfading while the sine fades in (low P1, no DCW), **158 / 159**; **P1 and P2 moving every block 98–112 / 159**; release 130; low notes C0–C2 120; high notes C6–E7 130; a 5-octave glide 128 / 143. **19 voices**: 120–130 per voice crossfading, ~158,000 cycles per block in all (19 × 8,331), 97 / 141 with P1 and P2 moving every block. Note-on 53 cycles |
| Level | per voice, velocity 127, steady: saw −12.0 dBFS peak, −15.3 RMS (a sine's level); every wave ≤ −8.8 dBFS peak; RMS −15.3 except pulse (down to −24.2 at full P1), resonance I (−20.7) and II (−17.5). A note's start at velocity 127 (the DCW at the full wave) peaks at most −8.7 dBFS. Four-voice chords at velocity 127 peak −1.2 dBFS in the track buffer at level 255 (the engine applies the level dial) |

Simulator checks: six golden scenarios (waves: P2 through all eight under a held note and P1 down to the sine and
back; dcw: plucks at four velocities and three decays, square and resonance plucks up to C7; reso: P1 sweeps on the
three resonance waves at C2 and C4, bends; bass: the "cz bass" line; chords: steals and releases while P1 and P2
are swept and jumped; extremes: every knob at both ends, C0, C8, G8 with ±2 semitone bends), all voices freed.
[tests/fuzz.c](tests/fuzz.c) (build line in the file, UBSan with bounds checks) compares every wave's fixed-point
loop with its floating-point definition (within 2.7·10⁻⁴ of full scale), checks every wave × amount × pitch for
parameter ranges and that the saw lands on one turn, checks that P1/P2 sweeps and jumps never step at a block
boundary (the second difference across a boundary is never larger than inside a block), and fuzzes 500,000 blocks
of random notes, knob jumps, NULL params, extreme pitches and slopes and envelope states for add-into-out,
determinism, a bounded level and state overruns. [tests/alias.c](tests/alias.c) measures the aliasing above.
[tools/tests/test_pd.py](../../tools/tests/test_pd.py) checks that `pd_tables.h` is up to date and that every wave
starts as a sine, is continuous and stays within its level limits.

## What to listen for on the device

Install it on the bass track (it starts on "cz bass") or the chord track ("cz strings"):

1. **P1 at 0** with a held note: a pure sine on every P2 position. Turning P2 at P1 0 changes nothing.
2. **P1 slowly up**: the sine opens into the wave without steps or zipper, like a filter opening; at 255 the saw
   should sound like a bright saw at low notes and gentler, but never gritty, at high notes.
3. **P2 slowly through 0 → 255** at P1 ~200: saw, square, pulse, double sine, saw-pulse, then the three resonance
   waves, each holding for a moment, sliding smoothly between them. Is any wave much louder or quieter than the
   others (the pulse and resonance I are meant to be a little quieter)?
4. **The DCW**: on "cz bass" or "pd pluck", play soft and hard notes: hard notes should start bright and "bwow"
   down, soft notes stay mellow. Turn the decay knob: the sweep gets slower (and the note longer).
5. **Resonance waves** (P2 182, 219, 255) with P1 swept: a resonant "filter sweep" without a filter; on a held low
   note it should sound like a resonant low-pass sweeping up. No clicks at the note's pitch.
6. **High notes**: C6–C8 on the saw and resonance waves at P1 255: duller than low notes on purpose, but no
   whistling or inharmonic tones; bend the pitch and listen for tones moving the wrong way (aliasing).
7. **Turn P1 and P2 fast, end to end, while chords play** on the chord track (the situation that crashed macro v3):
   no glitches, clicks or crashes; then the same with the LFO on P1 and on P2.
8. **CPU**: pd on all four synth tracks with chords (P2 between two waves is the dearest setting) plus drums: no
   dropped notes.
9. **Level**: compare with `macro`'s saw and `super`; say if it should be louder.

## Credits

Original code, MIT licence. Phase distortion as introduced in Casio's CZ synthesizers (1984): the wave names follow
the CZ's eight waveforms, but every wave here is our own definition (written so that its bends fall where the
cosine is flat), not a copy of Casio's. The polyBLAMP follows Esqueda, Välimäki and Bilbao, "Rounding Corners with
BLAMP" (DAFx 2016). The closed-form block envelope follows `engines/vowel`. No code from any synthesizer or library
was used.
