# sync

A hard-sync lead for the bass, lead, arp and chord tracks: a band-limited saw (the slave) hard-synced to a master
oscillator at the note's pitch, with a square sub one octave down and noise. Turning the ratio up sweeps the classic
tearing, vocal sync formant through the sound: screaming sync leads, sync basses, nasal sweeps, brassy chords. The
OP-Z's own track filter (the filter and resonance knobs) shapes it afterwards.

| Control | What it does across 0–255 |
|---|---|
| P1 **ratio** | The slave's frequency over the master's, on 2^(4x + 4x²) with x = P1 / 255: fine steps at the bottom (where the sync character grows), faster at the top. **0: ratio 1, a plain saw.** 33: 1.5 (a gentle bite), 53: 2 (one octave, brassy), 80: 3.1, 93: 4, 110: 5.5, **128: 8 (the classic sync lead)**, 158: 16 (screaming), 185: 32 (thin and whistling), 210: 64, 233: 128, 255: 256 (8 octaves). The pitch you hear never changes: it is always the master's. The slave stops at **10 kHz**, so on high notes the top of the knob holds still: on C2 from P1 239, C3 216, C4 191, C5 165, C6 136, C7 103. Turning it is smooth (the slave's frequency follows the knob every 1.5 ms; no steps, no zipper) |
| P2 **sub noise** | 0: the synced saw alone. **0–128: a square one octave below** comes in on an equal-power curve (64: −7.7 dB under the saw, 128: level with it, the saw 3 dB down). **128–255: white noise** comes in the same way against both, level with the oscillators at 255 (the oscillators 3 dB down). The loudness stays the same over the whole range (within 0.1 dB) |
| velocity | Level (linear), and the ratio: **+1/48 octave per step above 100, −1/48 below** (127: +0.56 octave, brighter and more "sync"; 76: −0.5 octave; 52 and below: −1 octave or more). The ratio never goes below 1, so soft notes with P1 near 0 are a plain saw |
| attack … release | the OP-Z envelope |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |
| pan, level | the track's pan and level (yellow and red dial, orange page), applied by the engine as TE's engines do (`opz_mix.h`): level is linear (128 = −6 dB, 0 = silent), pan a balance (centre: both sides full; hard left: right silent). The FX1/FX2 sends are taken after them. They follow the LFO (targets pan and volume) without zipping |

**Notes.** Any note; the master is the note's pitch (with the OS's portamento, bend and vibrato) and runs free, like an
analog oscillator: its phase carries on from note to note, so retriggered and stolen voices don't click. The LFO can
target P1 for vibrato-like ratio wobble or, with a note-triggered shape, the classic "sync sweep" per note.

**Level.** One voice at velocity 127 is −16.3 … −17.2 dBFS RMS everywhere on the P1 × P2 grid and the keyboard (a plain
saw at the ABI's −12 dBFS peak is −16.8), peaks −10.7 dBFS (P2 0) to −6.8 dBFS (all noise). Two things keep it even:
the synced saw's DC offset (up to −0.17 of the saw near ratio 1.4: the cut-off ramp of each period sits below zero) is
removed per block, so a sweep doesn't push the track filter around; and near the 10 kHz cap the band-limited slave
keeps few harmonics (−4.6 dB RMS at the cap), which the saw's gain makes up (×1 + 0.7 f / 10 kHz).

## How it works

Between discontinuities a synced saw is a straight line. So per block the engine only works out where the
discontinuities fall: the slave's own wraps (a step of −2) and the sync resets at every master wrap (a step from
wherever the slave was down to −1), found from the two phases with one reciprocal per oscillator, no division. The
sub square toggles at the same master wraps, so its steps ride on the reset events for free. Every step is
band-limited with a **4-point BLEP** (the integral of a cubic B-spline: two samples before the step, two after),
written into an event buffer as differences; per sample the output is just integrated (one add), scaled by the
envelope and written. The two samples before a step need one sample of look-ahead, so the output runs one sample
(23 µs) late: the block's last sample and the residuals that fall after the block are carried to the next block.
Every block restarts the integration from the exact phases, so its rounding never builds up.

**Aliasing** (the hard part of sync: every reset is a step at an arbitrary sub-sample time). Measured on the engine's
own render ([tests/alias.c](tests/alias.c)): held tones with the master on an exact FFT bin, so every harmonic lands on
a bin and everything else is aliasing; P2 0, velocity 100. Alias power below 11 kHz relative to all harmonic power,
this engine's 4-point BLEP vs a 2-point PolyBLEP (what most sync oscillators use) vs naive steps:

| Master | P1 (ratio, slave) | 4-point (sync) | 2-point PolyBLEP | naive |
|---|---|---:|---:|---:|
| 110 Hz | 0 (1, a plain saw) | −83.5 dB | −59.9 dB | −28.8 dB |
| 110 Hz | 120 (6.8, 752 Hz) | −73.9 dB | −50.8 dB | −20.3 dB |
| 110 Hz | 200 (48, 5.3 kHz) | −75.2 dB | −46.3 dB | −11.6 dB |
| 440 Hz | 80 (3.1, 1.4 kHz) | −74.9 dB | −50.4 dB | −18.2 dB |
| 440 Hz | 160 (17, 7.5 kHz) | −72.2 dB | −43.0 dB | −9.4 dB |
| 440 Hz | 200 (at the 10 kHz cap) | −78.6 dB | −46.9 dB | −8.6 dB |
| 1760 Hz | 80 (3.1, 5.5 kHz) | −59.4 dB | −38.1 dB | −10.8 dB |
| 3350 Hz | 40 (1.65, 5.5 kHz) | −58.1 dB | −36.9 dB | −10.7 dB |

Below 5 kHz (where aliasing is least masked) the 4-point BLEP is at −85 … −112 dB (2-point: −51 … −79 dB). Most of
what is left lies above 15 kHz, near the images of the slave's harmonics. The cap was chosen by the same measurement:
8, 10, 12 and 14 kHz gave −58 … −80, −71 … −81, −56 … −59 and −60 … −78 dB below 11 kHz at the top of P1. Run
`clang -O2 -I sdk/include -o /tmp/sync_alias engines/sync/tests/alias.c -lm && /tmp/sync_alias` (add `-DSYNC_BLEP2` or
`-DSYNC_BLEP0` to compare, or `-DSLAVE_MAX=0x…` to move the cap).

## Presets

Five per synth track. `slot` in `presets.json` is the track (0 bass, 1 lead, 2 arp, 3 chord); index 0 of each is the
sound a track gets when it switches to sync (white keys 1–5 on the device recall them).

| Track | Preset | Octave | P1 (ratio) | P2 | A | D | S | R | Level | Sound |
|---|---|---|---|---|---|---|---|---|---|---|
| bass | **sync bass** | −2 | 70 (2.6) | 100 | 0 | 70 | 150 | 25 | 205 | punchy sync bass with a strong sub |
| bass | sub sync | −2 | 45 (1.8) | 128 | 0 | 90 | 200 | 30 | 205 | gentle bite over a full square sub |
| bass | nasal bass | −2 | 140 (11) | 60 | 0 | 60 | 120 | 25 | 200 | vowel-like, nasal bass |
| bass | pluck bass | −2 | 100 (4.5) | 80 | 0 | 40 | 0 | 20 | 210 | short plucked sync bass |
| bass | growl bass | −2 | 120 (6.8) | 170 | 0 | 80 | 180 | 25 | 200 | sub and some noise: a dirty growl |
| lead | **sync lead** | 0 | 110 (5.5) | 40 | 0 | 90 | 210 | 50 | 195 | the classic sync lead |
| lead | scream | 0 | 170 (22) | 20 | 0 | 100 | 230 | 60 | 190 | high ratio: a screaming, whistling lead |
| lead | sub lead | 0 | 90 (3.8) | 90 | 0 | 90 | 220 | 50 | 195 | fatter lead with the sub under it |
| lead | nasal lead | 0 | 140 (11) | 0 | 0 | 90 | 200 | 45 | 190 | thin and nasal, no sub |
| lead | breath lead | 0 | 80 (3.1) | 190 | 4 | 90 | 220 | 55 | 195 | breathy: noise over the sync |
| arp | **sync arp** | 0 | 100 (4.5) | 40 | 0 | 40 | 0 | 30 | 200 | bright plucked arp |
| arp | zap arp | 0 | 160 (17) | 0 | 0 | 30 | 0 | 20 | 195 | short, zappy, high ratio |
| arp | bell arp | +1 | 128 (8) | 0 | 0 | 50 | 0 | 60 | 195 | ratio 8: the 8th harmonic rings, bell-like |
| arp | sub arp | 0 | 60 (2.2) | 120 | 0 | 35 | 0 | 25 | 200 | round arp with the sub |
| arp | noise arp | 0 | 120 (6.8) | 210 | 0 | 30 | 0 | 25 | 200 | percussive, noisy arp |
| chord | **sync pad** | 0 | 70 (2.6) | 60 | 100 | 150 | 220 | 110 | 160 | slow-swelling sync pad |
| chord | sync stab | 0 | 120 (6.8) | 30 | 0 | 50 | 0 | 40 | 165 | short chord stabs |
| chord | brass | 0 | 50 (1.9) | 50 | 10 | 100 | 200 | 50 | 160 | ratio near 2: brassy chords |
| chord | glass keys | 0 | 140 (11) | 0 | 0 | 90 | 90 | 70 | 160 | glassy keys |
| chord | air pad | 0 | 90 (3.8) | 200 | 110 | 140 | 220 | 120 | 155 | airy pad, lots of noise |

Every preset: filter 128, resonance 0, pan 128, sends 0, portamento 0, LFO off (depth 128, speed 100, target 255,
shape 0; the arp track keeps its own LFO values when a preset applies). Ratios are at velocity 100. Chord presets sit
lower in level because four voices add up. Names fit the OP-Z's 11 characters. `python3 tools/opz.py presets check
engines/sync` reports 20 presets, no warnings. Not yet tuned by ear on the device.

## What to listen for on the device

Install it on the lead track (`--slot N --replace`, the lead slots are full) or the chord track; it starts on that
track's index-0 preset. Then:

1. **Plain saw at P1 0.** Turn P1 to 0 and play soft notes (velocity ≤ 100): a plain, clean saw. It should sound like
   TE's saw at the same level knob.
2. **The sweep.** Hold one note around C4 and turn P1 slowly from 0 to 255 and back: a smooth, continuous "tearing"
   sweep through octaves of the slave; no steps, clicks or zipper noise; the pitch stays the same and the loudness
   stays about even. On C4 the top quarter of the knob (from ~190) should hold still (the slave's 10 kHz cap).
3. **Fast turning.** Turn P1 fast back and forth, then P2: still no clicks, no crackle, no stuck notes.
4. **P2.** From 0 to 128 a square sub one octave down comes in (a fatter, rounder low end); from 128 to 255 noise comes
   in until it is as loud as the oscillators. Loudness about the same throughout.
5. **High notes.** Play C6–C8 with P1 high: thin and bright, but no inharmonic whistles or "birdies" that move against
   the pitch when you bend (that would be aliasing).
6. **Bass.** "sync bass" and "nasal bass" on the bass track, C1–C2: punchy, with a solid sub; the ratio sweep on a low
   note should sound like a talking, nasal vowel.
7. **Velocity.** On a sequence with varied step velocities, louder steps are brighter (more sync), soft steps darker.
8. **Chords and knobs together (the macro v3 crash situation).** On the chord track, play or sequence four-note chords,
   and while they play turn P1 and P2 fast over their whole range for 10–20 seconds, including while notes start and
   release. Nothing should click, crackle or crash; voices should keep releasing normally.
9. **LFO.** Set the LFO to P1 (any shape): the ratio should wobble smoothly. A note-triggered shape on P1 gives the
   classic per-note sync sweep.
10. **CPU.** With all four synth tracks on sync and full chords, nothing should drop out.

## Facts

| | |
|---|---|
| Plug | 61019, `trackmask` 0xF0 (bass, lead, arp, chord), version 2, engine version 2 (v2: applies the track's level and pan) |
| Memory slot | 43 (10 KB class): **5,012 of 10,240 bytes** on Blackfin (4,728 payload: code, constants, exports; 284 B `.bss`: the shared event buffer) |
| State | 44 B of 280 per voice; `memRequest` 0; no libgcc, no division, no 64-bit maths, no tables |
| Latency | one sample (the BLEP's look-ahead) |

**CPU** (GNU Blackfin simulator, `tools/opz_cycles.py engines/sync`, [cycle-model.md](../../research/cycle-model.md);
4-note chord C3 G3 C4 E4 on the lead track, velocity 127, cycles per sample per voice, mean (worst block)). The cost
follows the number of discontinuities per block (slave wraps + resets) plus 13 per sample for the noise; the track's
level and pan (v2) add nothing measurable: the hand-scheduled `opz_mix.h` loops cost what v1's own output loop did
(with the C loops v2 was 8 higher throughout):

| Case | mean | worst |
|---|---:|---:|
| P1 0 (velocity 127: ratio 1.5), P2 0 / 128–255 | 38 / 51 | 45 / 58 |
| P1 80, P2 0 / 255 | 42 / 55 | 49 / 61 |
| P1 160, P2 0 / 255 | 66 / 79 | 84 / 97 |
| P1 255, P2 0 / 255 | 88 / 101 | 94 / 107 |
| low notes C0 C1 F1 C2, P1 255, P2 255 | 90 | 107 |
| high notes C6 G6 C7 E7, P1 255, P2 255 (slave at the cap) | 108 | 115 |
| top notes 124–127 (master at its 11 kHz limit: a reset every 4 samples) | 167 | 188 |
| glide C2 up 5 octaves, P1 160, P2 255 | 94 | 129 |
| release from block 20 | 79 | 97 |
| **knobs moving every block, period 8 / 40 (`knob_cases`)** | 65 / 66 | 107 / 107 |
| 19 voices started in one block, P1 160, P2 255 | 77 (~94,000 cycles/block) | 107 |
| 19 voices C5–F♯6, P1 255, P2 255 | 105 (~128,000 cycles/block) | 113 |
| 19 voices C5–F♯6, knobs moving every block | 82 | 113 |

Note-on 70 cycles, `initModule` 23. Every case is under the ABI's 150 target except the top four MIDI notes (only
reachable with octave and transpose at their limits), and under the ~218 budget for 19 voices everywhere.

**Checks.** `make -C sim test` (6 golden scenarios: lead, bass, chords, knobs, extremes, noise);
[tests/fuzz.c](tests/fuzz.c) with UBSan and bounds checks: the engine against a double-precision model of the same
oscillator (exact wrap and reset times, the BLEP from its continuous formula) over 200 runs × 200 blocks with random
pitches, slopes, P1 jumps and velocities: within 4.7e-5 of full scale (the Q15 residual polynomials; 4e-6 with exact
residuals); every P1 × velocity × pitch ratio within 0.05 % of the formula; P2 loudness within 0.02 dB; 300,000 fuzz
blocks (random notes, velocity 0–255, params NULL at note-on, knob jumps, pitch 0 / 0xffffffff / random with random
slopes, random envelope states, voice re-use without init): every event inside its block, adds into `out`,
deterministic, shared buffer left zeroed, no wraps, no full-scale samples, state never overrun. Sweeps (lead, knobs):
no steps at block boundaries (third difference flat by position in the block, within ±4 %).

## Credits

Original code, MIT licence (see CONTRIBUTING.md). The 4-point BLEP is the integrated cubic B-spline step described by
Välimäki, Pekonen and Nam, "Perceptually informed synthesis of bandlimited classical waveforms using integrated
polynomial interpolation" (J. Acoust. Soc. Am. 131(1), 2012); the polynomials and the event-buffer formulation are our
own. Inspired by the hard-sync voices of the OP-XY and of open-source virtual-analog oscillators; no code from any
synthesizer or library was used.
