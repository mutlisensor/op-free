# dx

Six-operator FM voice for the bass, lead, arp and chord tracks, with the 32 DX7 algorithms and a bank of 32 patches of
our own: e-pianos, basses, brass, winds, strings, pads, organs, bells, mallets, plucks and percussion. P1 picks the
patch, P2 makes the whole bank darker or brighter. Each patch keeps its own velocity response and operator
envelopes; the OP-Z's envelope sits on top as an overall amplifier. The OP-Z's own track filter (the filter and
resonance knobs) shapes the tone after it.

| Control | Function |
|---|---|
| P1 **patch** | picks one of the **32 patches**, 8 values each (P1 ÷ 8, table below; presets sit in the middle of a step, 8 n + 4). **Read when a note starts**: a held note keeps its patch while P1 moves (no jump, no click) and the next note plays the new one. A P1 change on the note's own step (a step lock, a preset, a quick turn) already applies to that note: the OS smooths P1 over ~0.3 s, and dx works out where P1 is heading from its first move after the note starts, before anything sounds. The OS LFO aimed at P1 therefore gives each note a different patch |
| P2 **bright** | scales **every modulator** of the patch (every operator that feeds another one; a modulator's feedback follows its level). **128 plays the patch as written.** Below 128 the modulators go down, linearly in dB, to **−24 dB at 0** (64 ≈ −12 dB): rounder, towards pure carriers. Above 128 they go up to **+12 dB at 255** (192 ≈ +6 dB): brighter and harder; a modulator can rise to at most twice the level of output level 99, and a feedback operator past its saw-like setting turns gritty and noisy. Carriers never change (nor the feedback of a carrier, as in algorithm 32), so the loudness stays put. Smooth at block rate: sweeping P2 under a held note does not click |
| velocity | each operator's own **velocity sensitivity** (0–7, as on the DX7): attenuation = sensitivity × 12 dB × (1 − ∛(velocity ÷ 127)), i.e. at sensitivity 7 −6.4 dB at velocity 100, −17 dB at 64, −39 dB at 20; nothing at 127. On carriers it sets loudness, on modulators brightness: most bank patches bark at high velocity and stay round at low velocity |
| attack, decay, sustain, release | the **OP-Z envelope is an overall amplifier on top of the patch** (the operators' own envelopes shape the timbre and the patch's own amplitude contour). Presets set attack 0 and sustain 255, so the OP-Z envelope is transparent while a note is held and the patch plays exactly as written. **Attack** fades the whole voice in. **Sustain** below 255 lets the whole voice fall to that level after the attack, at the **decay** rate (with sustain 255 decay does nothing). **Release**: a note-off starts the operators' own release (their R4 → L4) and the OP-Z release at the same time; the shorter one ends the sound. The voice is freed when the OP-Z envelope ends **or earlier, as soon as every carrier is silent and finished** (in sustain at level 0, or released) — so a decayed piano note frees its voice even while the key is held. Voice steals (stage 5/6) fade through the OP-Z envelope as on every engine |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |
| pan, level | the track's pan and level (yellow and red dial, orange page), applied by the engine as TE's engines do (`opz_mix.h`): level is linear (128 = −6 dB, 0 = silent), pan a balance (centre: both sides full; hard left: right silent). The FX1/FX2 sends are taken after them. They follow the LFO (targets pan and volume) without zipping |
| portamento, pitch bend, OS LFO on pitch | the operators follow the OP-Z's pitch per sample (fixed-frequency operators ignore it) |

The patch's own LFO (vibrato, tremolo; 6 waves, delay as a fade-in, per voice) and pitch envelope run in addition to
the OP-Z's LFO page.

### Patches (P1)

All patches are original, written for this engine (no Yamaha or third-party patch data). Ratios are operator
frequency ÷ note frequency.

| P1 | # | Patch | Alg. | Sound |
|---|---|---|---|---|
| 0–7 | 0 | TINE EP | 5 | tine e-piano: velocity sets the bark, a ratio-14 tine rings on the attack, slow decay |
| 8–15 | 1 | REED EP | 5 | reedy e-piano: strong bark at high velocity, a ratio-3 reed edge |
| 16–23 | 2 | BELL EP | 5 | e-piano with a bright bell partial two octaves up on the attack |
| 24–31 | 3 | CLAV | 3 | bright, buzzy clavinet (feedback 7), short |
| 32–39 | 4 | HARPSI | 5 | harpsichord-like pluck, 2nd and 5th harmonic brightness |
| 40–47 | 5 | SOLID BASS | 16 | round, punchy bass; velocity opens the attack |
| 48–55 | 6 | PICK BASS | 16 | picked bass with a click (ratio 7) on the attack |
| 56–63 | 7 | WOW BASS | 5 | brightness swells and falls on every note ("wow"), sub-octave layer |
| 64–71 | 8 | SLAP BASS | 1 | slap with a short 3rd-harmonic pop |
| 72–79 | 9 | SUB BASS | 5 | nearly pure, a touch of 2nd harmonic (3 operators) |
| 80–87 | 10 | FRETLESS | 5 | slow "mwah" swell into brightness, delayed vibrato |
| 88–95 | 11 | BRASS | 22 | brass section: the modulators swell after the carriers, each note scoops up ~1 semitone in 20 ms, delayed vibrato |
| 96–103 | 12 | SOFT HORNS | 5 | mellow horn ensemble, three detuned pairs |
| 104–111 | 13 | SYNTH LEAD | 5 | bright lead, square and saw layers (feedback 7), vibrato |
| 112–119 | 14 | FLUTE | 5 | near-sine flute with an attack chiff, delayed vibrato |
| 120–127 | 15 | CLARINET | 5 | odd harmonics (ratio-2 modulator), vibrato |
| 128–135 | 16 | OBOE | 3 | nasal double reed, 4th-harmonic formant, vibrato |
| 136–143 | 17 | STRINGS | 2 | string section: feedback saw, soft attack (~0.1 s), delayed vibrato |
| 144–151 | 18 | WARM PAD | 5 | soft pad, slow attack (~0.2 s), sub-octave layer, triangle vibrato |
| 152–159 | 19 | GLASS PAD | 5 | glassy pad with inharmonic shimmer (ratios 3.5 and 7) |
| 160–167 | 20 | CHOIR | 5 | "aah": 3rd and 5th harmonic formants, vibrato |
| 168–175 | 21 | ORGAN | 32 | drawbar organ, 16′ 8′ 5⅓′ 4′ 2⅔′ 2′, slight vibrato |
| 176–183 | 22 | PERC ORGAN | 32 | organ with decaying 3rd/4th harmonic percussion |
| 184–191 | 23 | TUBE BELL | 5 | tubular bell (ratio 3.5 and √2 modulators), long ring |
| 192–199 | 24 | GLOCK | 5 | glockenspiel, partials at 2.7 and 5.4 |
| 200–207 | 25 | MARIMBA | 5 | wooden bar with a 4th partial and a mallet click (ratio 10) |
| 208–215 | 26 | VIBES | 5 | vibraphone with a 5.5 Hz motor tremolo |
| 216–223 | 27 | KALIMBA | 5 | thumb piano with a 6th-partial ping |
| 224–231 | 28 | HARP | 5 | plucked harp with an octave layer |
| 232–239 | 29 | KOTO | 5 | twangy pluck, starts ¾ semitone sharp and settles in 20 ms |
| 240–247 | 30 | STEEL DRUM | 5 | steel pan, partials at 2, 3 and 3.5 |
| 248–255 | 31 | FM TOM | 5 | pitched tom: pitch drops ~4½ semitones in 50 ms, bright clangy attack |

The patches are first drafts checked in the simulator (spectra, envelopes, levels), not yet tuned by ear on the device.
Sources: [patches.py](patches.py) (readable), compiled by [gen_dx.py](gen_dx.py) into [dx_bank.h](dx_bank.h) and
[dx_bank.syx](dx_bank.syx), a standard DX7 32-voice bulk dump that any DX7 editor (Dexed, for example) can open to
audition and edit them.

## Presets

Five per synth track, like TE's synths: a track that switches to `dx` gets the first of its row (in **bold**), the
others are on the preset keys. Every preset: P2 128, attack 0, decay 0, sustain 255 (the patch plays as written),
filter 128, resonance 0, pan 128, LFO off. Release is set a little longer than the patch's own release, so the patch
decides how a note ends.

| Track | Preset | P1 (patch) | Release | Octave | Level |
|---|---|---|---|---|---|
| bass | **dx bass** | 44 (SOLID BASS) | 50 | −2 | 210 |
| bass | pick bass | 52 (PICK BASS) | 50 | −2 | 210 |
| bass | slap bass | 68 (SLAP BASS) | 50 | −2 | 210 |
| bass | wow bass | 60 (WOW BASS) | 50 | −2 | 205 |
| bass | fretless | 84 (FRETLESS) | 60 | −2 | 210 |
| lead | **dx brass** | 92 (BRASS) | 70 | 0 | 190 |
| lead | synth lead | 108 (SYNTH LEAD) | 60 | 0 | 185 |
| lead | flute | 116 (FLUTE) | 70 | 0 | 195 |
| lead | clarinet | 124 (CLARINET) | 70 | 0 | 190 |
| lead | oboe | 132 (OBOE) | 70 | 0 | 185 |
| arp | **marimba** | 204 (MARIMBA) | 70 | 0 | 205 |
| arp | kalimba | 220 (KALIMBA) | 70 | 0 | 205 |
| arp | harp | 228 (HARP) | 80 | 0 | 200 |
| arp | glock | 196 (GLOCK) | 80 | +1 | 195 |
| arp | steel drum | 244 (STEEL DRUM) | 70 | 0 | 200 |
| chord | **tine ep** | 4 (TINE EP) | 80 | 0 | 160 |
| chord | strings | 140 (STRINGS) | 100 | 0 | 150 |
| chord | organ | 172 (ORGAN) | 40 | 0 | 145 |
| chord | warm pad | 148 (WARM PAD) | 110 | 0 | 150 |
| chord | clav | 28 (CLAV) | 60 | 0 | 160 |

Chord presets sit lower because four voices sum (the simulator's tine ep chords at velocity 127 peak at −1.5 dBFS in
the track buffer at level 255). `python3 tools/opz.py presets check engines/dx` passes with no warnings. Not tuned by ear yet.

## Facts

| | |
|---|---|
| Plug | 61013, `trackmask` 0xF0 (bass, lead, arp, chord), version 2 (v2: applies the track's level and pan) |
| Memory slot | 7 (50 KB class): 24.4 KB used on Blackfin — 8.1 KB code, 8 KB sine table, 4 KB patch bank, 2.9 KB other tables, 0.8 KB of block buffers (`.bss`); `memRequest` 0 |
| State | 260 B of 280 per voice |
| CPU | Blackfin simulator ([cycle-model.md](../../research/cycle-model.md)), `python3 tools/opz_cycles.py engines/dx`: **171–178 cycles/sample/voice with all six operators running, 184 in the worst block** (11,780 cycles per block). Patches with fewer or decaying operators cost less: in the benchmark SUB BASS 111, ORGAN 136, BRASS 178 on average. Operators that are silent for a whole block are skipped; the rare algorithms 4 and 6 (a feedback loop over 3 or 2 operators) cost the same. **The track's level and pan (`opz_mix.h`) are ~5 of these cycles per sample** with the hand-scheduled `opz_mix.h` loops (~13 with the C loops; v1, without them: 166–173, worst block 179). 19 voices of BRASS: ~215,200 cycles per block (worst 221,500), more than the ~203,500 the OS leaves after its own share of its 265,000 limit, so a full layout of six-operator dx patches needs fewer voices. Note-on 2,640–2,950 cycles (twice when a P1 step lock restarts the note on another patch) |
| Voices that fit | Per voice 11,470 cycles per block at worst, against the OS's 265,000 for all voices (~13,900 per voice with 19). **All 11 synth-track voices on `dx` at once** (bass 2, lead 3, arp 2, chord 4): 126,000 cycles per block; with 8 `kit` hats on the drum tracks 209,000 (79 %), with 8 `kit` snares 256,000 (97 %, on the line). 19 voices of `dx` would take 218,000. All before the OS's own per-track work, and within the simulator's ×0.7–1.3 margin, so the 11 + 8-snare case can tip over on the device: runbook step 11 decides. No polyphony limit is built in |
| Level | at track level 255 (the level dial scales it linearly, 128 = −6 dB): −12 dBFS per voice: every patch's carriers are normalised together (from their output levels) so that at full envelope and velocity 127 they peak at `OPZ_VOICE_PEAK`. Measured single notes at velocity 110: −12.1 to −16 dBFS (several detuned carriers rarely line up; tremolo and slow attacks peak lower). A quiet single carrier is raised by at most 12 dB |
| Sound | Sine from a 4,096-entry table without interpolation (the DX7's own resolution): a pure sine has −67 dB of non-harmonic energy, a modulated note −55 to −62 dB up to C6. Operators above 16 kHz fade out (−45 dB at 20 kHz), above 20 kHz they are muted. High modulation indices on high notes alias, as on the DX7: a ratio-1 modulator at output level 99 (12.6 rad) is clean to C6 and folds back at C7; ratio 3 at level 90 folds from C6. Feedback 7 gives a clean saw (−59 dB at C4, aliasing rising to −34 dB at C7); P2 above 128 can push it into noise. Feedback FM carries a DC offset, which the OS's DC blocker removes |

### How a block is made

Once per block (64 frames, 1.45 ms): every operator's envelope, the LFO and the pitch envelope advance; each
operator's gain for the end of the block comes from its envelope, output level, keyboard scaling, velocity, P2 (on
modulators), tremolo, carrier normalisation (on carriers) and the 16–20 kHz fade, all added up in octaves and turned
into a gain with one `2^x`; gains ramp linearly across the block, so nothing zips. Then the operators run as one
pass each over the block, from operator 6 down to 1: an operator reads its phase modulation from bus A or B, from
its own last two outputs (feedback, averaged), or from nothing, and writes into a bus (replacing or adding) or into
the carrier sum. Every algorithm is a fixed program of those passes (`k_alg` in [dx_tables.h](dx_tables.h));
[gen_dx.py](gen_dx.py) checks each program against the DX7's algorithm chart by running it symbolically. The carrier
sum then goes through the OP-Z envelope and the track's level and pan into the track buffer. Each operator pass is ~14 instructions per sample;
the OP-Z envelope's per-sample step is ~28 of the rest.

### DX7 behaviour, and where dx differs

From the DX7: operator frequency (ratio 0.5–31 with fine steps of 1 %, or fixed 1 Hz–9.8 kHz, detune), output level
and envelope level curves (0.75 dB steps), the 4-rate/4-level envelope with rate scaling, its fast-rising attack that
starts at about −53 dB and its timed plateaus, keyboard level scaling with four curves, velocity sensitivity,
amplitude modulation sensitivity, oscillator key sync, LFO (speed, delay, pitch and amplitude depth, 6 waves, key
sync), pitch modulation sensitivity, pitch envelope, transpose. The unit conversions follow the DX7's published
behaviour as modelled by an MIT-licensed 6-op FM implementation (Credits).

Differences, on purpose or for lack of data:

- **Modulation depth**: output level 99 with the envelope at 99 swings the modulated phase by 4π rad (12.6); each
  0.75 dB step down scales it. **Feedback 7** is π/2 rad on the average of the operator's last two outputs, the
  cleanest bright saw; the stronger setting first tried (π) turned the loop into noise.
- Envelopes advance once per block with linear gain ramps in between (1.45 ms resolution), envelope time is
  quantised to blocks.
- The exponential keyboard-scaling curve, the velocity curve, the LFO delay (a fade-in, 0–5 s) and the pitch
  envelope's rates (0.01–100 octaves per second) are our own curves.
- One LFO per voice (the DX7 has one for the whole instrument); with LFO key sync off each voice's LFO runs on.
- Carriers are normalised per patch (the DX7 adds them up raw), so patches play at the same loudness.
- No mod wheel, breath, aftertouch or pitch-bend range (those are DX7 function settings, not part of a voice); the
  OP-Z's own pitch bend and LFO apply.

### Patch format and the editor (F-04)

The bank `s_bank` is **32 patches in the DX7 32-voice packed format, 128 bytes each: 4,096 bytes**, exactly the
largest file `getPresetData` (callback `+0x28`) reads (`engines/fmedit.preset`, see
[engine-abi.md §5](../../sdk/docs/engine-abi.md) and Q-12). It is writable on purpose, and dx reads it live: rates,
levels, LFO, pitch envelope, feedback and algorithm per block, frequencies, output levels, scaling and velocity at the
next note. Every byte is range-checked where it is used, so any 4,096 bytes play without crashing (fuzzed: 200,000
blocks of random banks, notes, velocities and knob moves under the undefined-behaviour sanitiser, peak −6 dBFS). A
later version can load a bank from an editor with one call in `initModule` (`cb->getPresetData(0, 0, s_bank)`, after
checking on the device that the entry is not null), and the same bytes open in DX7 editors. Not built yet: v1 never
calls `+0x28`.

## What to listen for on the device

Install on the chord track and play the tine ep preset: step velocities (or MIDI velocity) should change how much
each note barks, and a held chord should ring on and fade over several seconds. Turn P1 while holding a chord: the
chord must not change or click; the next notes play the new patch. Put a P1 step lock on a step: that step's note
should already play the locked patch. Sweep P2 on a held brass note: 0 rounder (almost sine-like), 128 as written, 255
brighter and harder, never louder or quieter overall. On the bass track, check that a long release preset still frees
voices quickly (notes stop when the patch's own release ends). Hold a pad chord while new notes steal voices: no clicks.
Play the arp presets high up (glock, tube bell from the bank at C6–C7): bright but not harsh or out of tune; the very
top of bright patches may alias, as on the original. CPU: all four synth tracks on `dx` with drums running should not
crackle (runbook step 11 is the real test).

## Credits

Original code and patches. The design follows Yamaha's DX7 (its algorithms, parameters and voice format, which are
documented publicly) and Chowning's FM synthesis, with TE's unreleased `DxFm` as inspiration only (no TE code). The
unit conversions (operator level, LFO rate, pitch-envelope level, envelope qrates and attack shape) are derived from
the MIT-licensed 6-op FM code of an open-source eurorack macro oscillator, Copyright 2021 Emilie Gillet, whose
notice is reproduced in [dx.c](dx.c). MIT licence.
