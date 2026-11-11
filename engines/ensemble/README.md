# ensemble

A 70s string machine for the chord track (and bass, lead and arp): divide-down style string registrations — dark
cello, viola, bright violin with the 4' octave — through the lush three-phase ensemble chorus that made those
machines swim. P1 picks the registration, P2 takes the ensemble from dry through lush to seasick. Made for slow
attacks and long releases; the OP-Z's own track filter (the filter and resonance knobs) shapes the tone after it.

| Control | Function |
|---|---|
| P1 **registration** | a smooth morph through five registrations, each a mix of three footages (8' saw, 8' square, 4' saw = an octave up) through a two-pole tone low-pass: **0 cello** (8' saw + 8' square at equal level: hollow and woody, tone at 600 Hz), **64 dark viola** (saw 0.7 + square 0.3, 1.3 kHz), **128 viola** (8' saw alone, 2.8 kHz), **192 violin** (saw 0.7 + 4' saw 0.3, 6 kHz), **255 full** (8' and 4' saws at equal level, tone open at 16 kHz). Footages, tone and level are interpolated between the five points, so a sweep is smooth (no steps or zipper); the level stays within ~2 dB across the range (the cello is quieter above C5: its tone stays dark there). The tone corners are for C4 at velocity 100; they follow the keyboard by half (an octave every two octaves) |
| P2 **ensemble** | **0 dry**: the bare registration. **0–76**: the three ensemble voices fade in as the dry oscillator fades out; from 76 on the sound is all ensemble, like the classic machines. **0–153: dry → lush**: the modulation depth grows to the classic setting at 153: a slow sweep at 0.63 Hz (±6 cents per ensemble voice) and a fast shimmer at 6.3 Hz (±4 cents), the three voices 120° apart, so the chord swims and spreads in stereo. **153–255: lush → seasick**: a slow wow common to all voices grows to ±18 cents at 0.42 Hz and the shimmer deepens to ±7 cents at 5.6 Hz (each voice ±22–30 cents in all): warped-tape strings. Turning P2 never makes the pitch jump: the depth scales the voices' speed of change, not their position. Level per channel within about ±2 dB over the range (some notes dip ~3 dB at P2 ~100–150: see below) |
| velocity | level, gently: 0.4 at velocity 0 to 1.0 at 127 (string machines barely respond to touch); and the tone corner, −0.47 octave at velocity 0 to +0.13 at 127 |
| notes | free-running oscillators: every note starts at a random point of its waveform, as on a divide-down machine where the oscillators never stop. Pitch bend and portamento move every footage and ensemble voice together |
| attack … release | the OP-Z envelope; slow attacks (60–120) give the classic swell |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |
| pan, level | the track's pan and level (yellow and red dial, orange page), applied by the engine as TE's engines do (`opz_mix.h`): level is linear (128 = −6 dB, 0 = silent), pan a balance on the stereo voice (centre: both channels full, the whole width; hard left: only the left channel, the right silent). The FX1/FX2 sends are taken after them. They follow the LFO (targets pan and volume) without zipping |

**The ensemble.** A string machine's ensemble passes the signal through three short delay lines (bucket brigades)
whose delays are swept by a slow and a fast LFO, the three 120° apart, and mixes them. Instruments on the OP-Z get
no large buffer and each voice has only 280 bytes of state, so a delay line per voice (a few milliseconds is
~200 samples, 400 bytes) does not fit, and a static pool shared by all voices would cost ~6 KB of the 10 KB slot
and a pool steal for every note past the pool size. ensemble uses an exact equivalent instead: delaying a periodic
waveform by d(t) is the same as running its oscillator with the phase moved back by f0·d(t), and the ensemble is
linear (the chorus of a chord is the sum of each note's chorus). So every voice runs **four phases of the same
oscillator** — the dry one and three ensemble voices — and each ensemble voice's phase follows the delay its
bucket-brigade line would have, d_k(t) = 40 samples · sin(slow + k·120°) + 2.5 samples · sin(fast + k·120°) at the
classic setting. The phase offsets are applied as the delay's change per block (a small frequency offset), which is
why turning P2 never jumps. For held notes the result is the delay-line ensemble itself, with the same
frequency-dependent comb filtering and stereo spread, and it is alias-free and needs no memory. The LFO phases are
shared by all voices of a track (a new voice takes them over at note-on), so a whole chord swims together as
through one ensemble. What it does not model: the bucket brigades' own noise and darkening, and the delay of a
note's onset in the wet path (a few milliseconds, inaudible under string attacks).

**Why seasick is a common wow.** Three copies 120° apart whose phase swings approach a turn cancel each other's
fundamental on some notes (on average the three add up to 3·J0(β), β the phase swing): a deep three-phase sweep
made some notes hollow and 3–4 dB quieter. A wow moves all four copies together and cannot cancel anything, so
seasick deepens the wow (and the fast shimmer) and keeps the three-phase sweep near the classic depth.

**Stereo.** The dry phase sits in the middle, ensemble voice 1 left (L 1, R 0.25), voice 2 in the middle (0.625
each), voice 3 right (0.25, 1). Mono is the sum of all four phases; the side signal is the difference of the outer
two. Their difference at the fundamental is only f0 times about a millisecond of delay, so **low notes stay almost
mono** (side 15–25 dB below mid at C1–C2, no hollow bass in mono) while the upper harmonics spread wide (side 4–7 dB
below mid from C4 up) — the way a real ensemble's width grows with frequency. On the bass track the side is halved.

**How it fits the CPU** (the trick from `super`). All three footages jump only at the 8' phase 0 and ½, so their
mix is a straight ramp plus two kinds of steps. Per block each of the four phases writes its steps and their
two-sample PolyBLEP residuals into shared mid/side event buffers; per sample only mid and side are integrated,
filtered (two one-poles each) and added into the output with the envelope. Each block restarts from the exact
phases, so rounding never accumulates. The host envelope is advanced per block in closed form and applied as a
64-sample ramp (as in `vowel`). No division, 64-bit maths or floating point anywhere in `render`.

## Presets

Five per synth track. `slot` in `presets.json` is the track (0 bass, 1 lead, 2 arp, 3 chord); index 0 of each is the
sound a track gets when it switches to ensemble.

| Track | Preset | Octave | P1 | P2 | A | D | S | R | Level | Sound |
|---|---|---|---|---|---|---|---|---|---|---|
| bass | **string bass** | −2 | 40 | 60 | 6 | 80 | 220 | 40 | 205 | dark viola-cello bass, a touch of ensemble |
| bass | cello bass | −2 | 0 | 110 | 30 | 90 | 230 | 60 | 205 | bowed cello section, swelling |
| bass | octave bass | −2 | 255 | 40 | 0 | 60 | 180 | 30 | 200 | bright 8' + 4' saw bass, nearly dry |
| bass | ensemble bs | −2 | 128 | 153 | 16 | 90 | 220 | 50 | 200 | classic lush ensemble on a bass line |
| bass | wobble bass | −2 | 192 | 230 | 0 | 70 | 200 | 35 | 200 | violin registration, seasick |
| lead | **violin** | 0 | 192 | 120 | 30 | 90 | 230 | 60 | 195 | violin lead, gentle ensemble |
| lead | string lead | 0 | 255 | 100 | 8 | 80 | 220 | 50 | 190 | full registration, quick attack |
| lead | viola | 0 | 128 | 140 | 40 | 100 | 230 | 70 | 195 | viola, near-classic ensemble |
| lead | solo cello | −1 | 16 | 90 | 50 | 100 | 240 | 70 | 200 | hollow cello line an octave down |
| lead | seasick | 0 | 220 | 255 | 20 | 90 | 230 | 60 | 190 | warped-tape strings |
| arp | **string arp** | 0 | 200 | 140 | 0 | 50 | 0 | 40 | 195 | short violin plucks with ensemble |
| arp | pizzicato | 0 | 100 | 60 | 0 | 30 | 0 | 25 | 200 | short, darker, almost dry |
| arp | glass arp | +1 | 255 | 180 | 0 | 60 | 60 | 50 | 190 | bright octave strings, shimmering |
| arp | organ arp | 0 | 64 | 30 | 0 | 70 | 160 | 30 | 195 | hollow, nearly dry, organ-like |
| arp | drift arp | 0 | 160 | 230 | 0 | 60 | 80 | 60 | 190 | drifting, wobbly arp |
| chord | **solina** | 0 | 200 | 153 | 90 | 120 | 255 | 120 | 165 | the classic: violin + ensemble, slow swell |
| chord | string pad | 0 | 160 | 170 | 110 | 140 | 255 | 140 | 165 | viola/violin pad, slower |
| chord | cello pad | 0 | 30 | 140 | 100 | 130 | 255 | 130 | 170 | dark, hollow cello pad |
| chord | full string | 0 | 255 | 140 | 70 | 120 | 240 | 110 | 160 | bright 8' + 4' section |
| chord | seasick pad | 0 | 220 | 245 | 100 | 140 | 255 | 150 | 165 | slow, deep, warped pad |

Every preset: filter 128, resonance 0, pan 128, sends 0, no LFO (LFO depth 128 = off, speed 100, target 255,
shape 0; an LFO on P1 makes the registration breathe). Chord presets sit lower in level because several voices add
up. Names fit the OP-Z's 11 characters. `python3 tools/opz.py presets check engines/ensemble` reports 20 presets,
no warnings. Not yet tuned by ear on the device.

## Facts

| | |
|---|---|
| Plug | 61201, `trackmask` 0xF0 (bass, lead, arp, chord), version 2 (engine version 2; v2: applies the track's level and pan) |
| Memory slot | 41 (10 KB class): 5,920 of 10,240 bytes on Blackfin (5,268 B of code, tables and data, 652 B of `.bss`: the shared mid/side event buffers and the per-track LFO phases); engine file 5,144 bytes; no libgcc |
| State | 80 B of the 280 the OS leaves per voice; `memRequest` 0; no voice pool |
| CPU | Blackfin simulator ([cycle-model.md](../../research/cycle-model.md), `tools/opz_cycles.py`), 4-note chord C3 G3 C4 E4 on the chord track at velocity 127, mean / worst block per voice in cycles per sample: **dry (P2 0) 85 / 93** for every registration; **P2 76 (dry and all three ensemble voices) 114 / 124**; **lush and seasick (P2 153, 255) 104–105 / 114**; release 105 / 114; slow-attack pad 105 / 114; low notes C0–E1 99 / 108; high notes C6–E7 138 / 163; the most expensive case, top notes 120–127 at the 11 kHz pitch cap, 196 / 213. **P1 and P2 moving every block 105–107 / 118** (4 voices) and **105 / 124 with 19 voices**. **The track's level and pan (`opz_mix.h`) are ~3 of these per sample** with the hand-scheduled `opz_mix.h` loops (~13.5 with the C loops; v1: dry 82 / 89). **19 voices** (full, seasick): 105 per voice, ~126,700 cycles per block in all (worst block 134,700); 19 voices C5–F♯6 119 / 136, ~144,600 per block — against the OS's 265,000 before its own share. Note-on 123 cycles, `initModule` 18 |
| Level | at track level 255 (the level dial scales it linearly, 128 = −6 dB): per voice, velocity 127, one note held (6 s, past the LFO periods): RMS −18 … −25 dBFS per channel (registration and P2 within ~2 dB of each other from C2 to C4; the ensemble is ~1.5 dB louder on low notes, where its copies add in phase, and up to ~3 dB quieter at C4–C6 around P2 100–150, where the three-phase comb thins the fundamental; the dark cello at C5–C6 down to −28), peaks −13 … −20 dBFS, the loudest −12.8 dBFS (full, C1). **Six-note chords** at velocity 127 (C3 E3 G3 B3 D4 G4, generous chords): RMS −14 … −16 dBFS, peaks −0.9 … −3.6 dBFS in the track buffer, no clipped samples |

Simulator checks: six golden scenarios (registration sweep, ensemble sweep, six-note "solina" chords stealing the
chord track's four voices, bass lines with a bend, extremes from C−1 to G9 with bends and knob ends, P1 and P2
swept every block while chords start, steal and release), all voices freed. No steps at block edges while P1 or P2
sweep or jump (second difference by position in the block, opzsim renders). [tests/fuzz.c](tests/fuzz.c) (build
line in the file, UBSan with bounds checks) compares the event oscillator with a per-sample PolyBLEP footage mix
(3.4 × 10⁻⁶ of full scale), renders every P1 byte × every third P2 byte at notes 0–127 (tap increments within 6.5 %
of the pitch, no sample above half scale, footage sums in range at every P1 word), and fuzzes 3 × 2,000,000 blocks
of random notes (velocity 0–255, `params` NULL), knob jumps every block, extreme pitches and slopes, random
envelope states and tracks 0–15 for add-into-out, determinism (the shared LFO table included), clean event buffers,
the envelope range, wraps and writes past 280 bytes of state: clean.

## What to listen for on the device

Install it on the chord track (it starts on "solina"):

1. **Chords:** hold a 4-note chord with "solina": a slow swell into a wide, moving string section that swims
   gently (slow sweep) and shimmers (fast), not four static oscillators. It should sound like one ensemble across
   the chord, not each note wobbling on its own.
2. **P2 slowly from 0 to 255 on a held chord:** dry and static at 0; the movement and width growing up to ~150
   (lush); from ~150 a slow common wow taking over until 255 sounds seasick. No pitch jumps or clicks while turning,
   loudness about even.
3. **P1 slowly from 0 to 255 on a held chord:** hollow dark cello → viola → bright violin → the 4' octave joining
   at the top, no steps. Is 0 dark and woody enough, 255 bright enough?
4. **P1 and P2 turned fast together while chords play and retrigger** (the situation that crashed macro v3): it
   must not glitch, click or crash; a fast P2 move only changes the amount of movement.
5. **Mono:** at lush, low chords (C2–C3) should stay full when the OP-Z is heard in mono (the speaker); high notes
   may thin a little, as with a real ensemble.
6. **Bass track** ("string bass", octave −2): solid low strings, narrower stereo, no hollow notes.
7. **High notes** (C6–C8, "glass arp"): even and clean, no whistles; the 4' fades out above ~2.5 kHz.
8. **Level and CPU:** about as loud as `super`; say if it should be louder. A full chord track plus other tracks
   should run clean.

## Credits

Original code, MIT licence. After the classic string ensembles of the 1970s (three bucket-brigade delay lines on a
slow and a fast LFO 120° apart) and the string machine model of the MIT-licensed Plaits 1.2 firmware; no code from
any synthesizer or library was used, and the registrations, depths and rates are our own. The PolyBLEP polynomial
is the SDK's (`sdk/include/opz_dsp.h`); the event-buffer oscillator follows `engines/super`, the per-block envelope
`engines/vowel`.
