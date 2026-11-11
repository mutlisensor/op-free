# vowel

A formant voice for the bass, lead, arp and chord tracks: sung vowels, "aah" choir pads, talking leads and vocal
basses. A band-limited glottal pulse train feeds four resonant formants that morph smoothly through eight vowels
(P1), scaled for the body size of the singer from a giant bass to a child, with a breath zone that ends in a whisper
(P2). Every voice is its own singer (pitch drift, a natural vibrato, its own formants and place in the stereo field);
on the chord track every voice is **two** singers, so chords sound like a small choir. The OP-Z's own track filter
(the filter and resonance knobs) shapes the tone after it.

| Control | Function |
|---|---|
| P1 **vowel** | a smooth morph along **hum [m] – oo [u] – oh [o] – ah [a] – eh [e] – ee [i] – ue [y] – er [ɝ]**, each vowel at one point: **0 hum, 36 oo, 73 oh, 109 ah, 146 eh, 182 ee, 219 ue, 255 er**. In between, the four formants' frequencies and bandwidths glide on a log scale and their levels in dB, so the path passes through the vowels you would expect (oh → ah passes "aw", ah → eh passes "a" as in *cat*, ee → er passes the French *u*). The coefficients are recomputed every block from the host's smoothed knob: no clicks or zipper while turning, and the LFO can drive it |
| P2 **voice** | **0–191: body size.** All formants scale together: **0 ×0.72 "giant"** (a deep, dark monk/basso voice), **64 ×1.0 male** (tenor/baritone, the table's reference), **128 ×1.18 female**, **191 ×1.36 child**. The pitch is not changed, only the size of the "mouth". **192–255: breath zone** (size stays at child): breath noise through the same formants replaces the voice, about equal-power, voice and breath equal at ~224, **a whisper at 255** (the vowels stay intelligible, no pitch). The level stays within ~2 dB over the whole range |
| velocity | level (linear); brightness: a gentle 6 dB/octave tilt with its corner at 1.2 kHz (velocity 0) up to 9 kHz (127), so soft notes are rounder; and a short breathy **"h" onset** (~8 ms decay, ~30 ms long) whose level goes with velocity² (half the voice's level at 127, inaudible below ~40) |
| notes | each note is one singer. The first glottal pulse comes within 2 ms of the note-on. Pitch bend and portamento move the pitch; glides retune the formant limits (below) block by block |
| attack … release | the OP-Z envelope |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |
| pan, level | the track's pan and level (yellow and red dial, orange page), applied by the engine as TE's engines do (`opz_mix.h`): level is linear (128 = −6 dB, 0 = silent), pan a balance on the stereo voice (centre: both channels full, the whole width; hard left: only the left channel, the right silent). The FX1/FX2 sends are taken after them. They follow the LFO (targets pan and volume) without zipping |

**The voice.** A pulse train (the derivative of a PolyBLEP sawtooth: three samples per period, a flat line spectrum
up to the top, no aliasing to speak of) excites four parallel band-pass formants (Chamberlin state-variable
filters, peak gain 1 × the formant's level). The formants alternate in sign, so neighbouring formants add between
their peaks instead of notching. Formant table ([gen_tables.py](gen_tables.py)): our own rounding of published
phonetic averages (Peterson & Barney for F1–F3 of spoken vowels, Fant and Sundberg for bandwidths and the levels of
sung vowels and the singer's F3/F4); checked on renders, the spectral peaks land on the table (ah at A2: 770, 1100,
2530, 3300 Hz).

**Low and high notes.** On high notes the first formant never drops below 1.11 × f0 (sopranos tune F1 to the
fundamental the same way), each formant stays above the one below it, and bandwidths widen to at least 0.7 × f0, so
a note's sparse harmonics can't fall into a gap between formants (no notes that suddenly go quiet). Formants are
capped at 7 kHz. The output gain follows the RMS of a line spectrum through the formants,
1 / sqrt(f0 · max(f0, π B1 / 2)), so the level stays even above C4. Below C4 the pulses ring apart (vocal fry) and
the crest factor grows 3 dB per octave, so the gain meets the peaks half-way: the RMS falls ~2.5 dB per octave
while peaks stay level. On a glide downwards the gain rises at most 1/16 octave per block (never amplifies what's
still ringing from the higher note); upwards it follows at once.

**The choir.** Seeded per note: a slow pitch drift (a triangle at 0.17–0.47 Hz, ±4 cents), a natural vibrato that
fades in after a quarter second (4.7–5.7 Hz, ±5 cents), formant sizes ±2.4 % (chord) / ±1 % (others) and a stereo
position. **Chord track:** every voice is two singers 7–13 cents apart, each drifting on its own, through the same
formants, panned up to ±45 % (each voice is mono, the chord is wide). **Lead and arp:** one singer, ±15 %. **Bass:**
one singer, centred.

**How it fits the CPU.** Between glottal pulses the source is zero, so each formant only rings: three multiplies per
sample instead of four, and the input is added only around the pulses (or everywhere while breath noise plays). The
pulses are found per block from the phase (as `super` finds its saw wraps), so a second singer costs almost nothing.
Formant coefficients are recomputed only when P1, P2 or the pitch (by more than 1.6 %) changed. The host envelope
is advanced per block in closed form (decay/release: level × c^64; attack/sustain: (1 − r³)·s held for the block;
the rare fade with a step: the exact 64 steps) and applied as a 64-sample ramp, so the stages the host sees are the
per-sample ones to within rounding.

## Presets

Five per synth track. `slot` in `presets.json` is the track (0 bass, 1 lead, 2 arp, 3 chord); index 0 of each is the
sound a track gets when it switches to vowel.

| Track | Preset | Octave | P1 | P2 | A | D | S | R | Level | Sound |
|---|---|---|---|---|---|---|---|---|---|---|
| bass | **vocal bass** | −2 | 80 | 24 | 0 | 80 | 200 | 30 | 205 | dark "aw" from a big bass voice |
| bass | monk | −2 | 36 | 0 | 20 | 100 | 230 | 50 | 205 | giant "oo", slow onset, chant |
| bass | doo bass | −2 | 50 | 40 | 0 | 45 | 0 | 25 | 210 | plucked "doo" bass |
| bass | hum bass | −2 | 0 | 30 | 0 | 90 | 220 | 30 | 210 | closed-mouth hum |
| bass | yah bass | −2 | 230 | 50 | 0 | 70 | 150 | 25 | 205 | nasal "ue/er" bass, cuts through |
| lead | **aah lead** | 0 | 109 | 128 | 5 | 90 | 220 | 50 | 195 | female "aah" |
| lead | ooh lead | 0 | 45 | 100 | 8 | 90 | 220 | 55 | 195 | soft "ooh" |
| lead | ee lead | 0 | 182 | 70 | 0 | 80 | 210 | 45 | 195 | bright male "ee" |
| lead | whisper | 0 | 120 | 240 | 10 | 90 | 220 | 60 | 195 | breathy, mostly whispered "ah" |
| lead | er lead | 0 | 255 | 150 | 0 | 85 | 210 | 45 | 195 | talky "er"; sweep P1 down for "er-ah" |
| arp | **doo arp** | 0 | 40 | 140 | 0 | 35 | 0 | 25 | 200 | short "doo" |
| arp | dah arp | 0 | 109 | 160 | 0 | 40 | 0 | 30 | 200 | short "dah" |
| arp | bee arp | 0 | 182 | 180 | 0 | 38 | 0 | 28 | 200 | short, bright "bee" |
| arp | choir arp | 0 | 120 | 128 | 0 | 60 | 120 | 40 | 195 | sung arp with a sustain |
| arp | breath arp | 0 | 100 | 215 | 0 | 50 | 60 | 40 | 200 | half voice, half breath |
| chord | **aah choir** | 0 | 109 | 140 | 110 | 150 | 230 | 120 | 165 | slow-swelling "aah" choir |
| chord | ooh choir | 0 | 40 | 128 | 120 | 150 | 230 | 120 | 165 | "ooh" choir |
| chord | mmm pad | 0 | 0 | 110 | 100 | 140 | 230 | 110 | 170 | hummed pad |
| chord | boys choir | 0 | 140 | 185 | 100 | 150 | 230 | 120 | 165 | child-sized "eh/ah" |
| chord | whisper pad | 0 | 90 | 235 | 110 | 150 | 230 | 130 | 170 | whispered pad |

Every preset: filter 128, resonance 0, pan 128, sends 0, no LFO (an LFO on P1 makes any of them talk). Chord
presets sit lower in level because four voices add up. Names fit the OP-Z's 11 characters.
`python3 tools/opz.py presets check engines/vowel` reports 20 presets, no warnings. Not yet tuned by ear on the
device.

## Facts

| | |
|---|---|
| Plug | 61018, `trackmask` 0xF0 (bass, lead, arp, chord), version 2 (v2: applies the track's level and pan) |
| Memory slot | 46 (10 KB class): 6,700 of 10,240 bytes on Blackfin (6,016 B of code and tables, 684 B of `.bss`: the shared block buffers) |
| State | 208 B of 280 per voice; `memRequest` 0 |
| CPU | Blackfin simulator ([cycle-model.md](../../research/cycle-model.md), `tools/opz_cycles.py`), 4-note chord C3 G3 C4 E4 at velocity 127, mean / worst block per voice in cycles per sample: **lead (one singer) 108 / 153** for every vowel and size; **chord (two singers) 115 / 160**; breath zone and whisper 131 / 154 (135 / 160 on chord); **P1 and P2 moving every block 131–132 / 153 (138 / 159 on chord)**; release 136 / 158; low notes C0–E1 110 / 157; the most expensive case, high notes C7–C8 on chord, 161 / 201. **The track's level and pan (`opz_mix.h`) are ~5 of these per sample** with the hand-scheduled `opz_mix.h` loops (~14 with the C loops; v1: lead 103 / 148). **19 voices** of chord (two singers, breath zone): 136 per voice, ~165,300 cycles per block in all (worst block 193,100; per voice 10,265), against the OS's 265,000 before its own share. A note's first block (coefficients, the breathy onset) is the worst block; note-on 236–259 cycles |
| Level | at track level 255 (the level dial scales it linearly, 128 = −6 dB): per voice, velocity 127, one note held: above C4 −18.6 … −24.5 dBFS RMS (most vowels −19 … −22), peaks −8 … −15 dBFS; C3 ~−21.5, C0 ~−29 RMS with peaks −9 … −14 (see above). Over P2 0 … 255 at A3: −18.7 … −20.9 RMS. Chord track (two singers): about 1 dB lower RMS, peaks up to ~−6 dBFS when the two singers' pulses line up. Full scenarios: a 5-note "aah choir" peaks −4.2 dBFS in the track buffer, no clipping |

Simulator checks: six golden scenarios (vowel sweep, voice sweep, choir with steals, vocal bass, extremes, knobs
moving every block while chords start, steal and release), all voices freed. [tests/fuzz.c](tests/fuzz.c) (build
line in the file, UBSan with bounds checks) compares the pulse source with a per-sample PolyBLEP reference (3.8e-6 of
full scale), checks every P1 × P2 byte at notes 0–127 with bends for coefficient ranges and filter stability
(1.86 million settings), and fuzzes 200,000 blocks of random notes, knob jumps, extreme pitches and envelope states
for add-into-out, a clean shared buffer, the envelope range and wraps.

## What to listen for on the device

Install it on the chord track (it starts on "aah choir") or on the lead track ("aah lead"):

- **P1:** a slow sweep should sound like someone singing "mmm-ooo-oh-aah-eh-ee-ue-er" on one breath, with no
  steps, clicks or zipper; an LFO on P1 should make it talk ("wow-wow"). Does each vowel sound like its name?
- **P2:** 0 should sound like a huge, dark voice, 64 a man, 128 a woman, 191 a child, at the same pitch; above ~200
  breath creeps in until 255 is a whisper that still says the vowel. Loudness about the same throughout.
- **Chords on the chord track:** four voices should sound like a small choir, wide in stereo, gently moving (each
  singer drifts and starts its vibrato after a moment), not like four identical oscillators. Compare with the lead
  track (one singer per voice, narrower).
- **Low notes:** "vocal bass" around C1–C2 should be a deep, slightly buzzy voice (pulses ringing apart, like vocal
  fry), not clicks; down at C0 it becomes a creak.
- **High notes:** C6–C8 should stay sung and even in level (vowels fade into "oo"-ish, as with a real soprano), no
  notes that drop out or whistle.
- **Velocity:** soft notes rounder and quieter; hard notes brighter with a short breathy "h" at the start.
- **Level:** about as loud as `super`, a little quieter than macro's saw; say if it should be louder.
- **CPU:** a full chord track of "aah choir" plus other tracks should run clean; turning P1 and P2 fast while chords
  play (the situation that crashed macro v3) must not glitch.

## Credits

Original code, MIT licence. The formant table is our own, rounded from published phonetic measurements (Peterson &
Barney 1952; Fant, *Acoustic Theory of Speech Production* 1960; Sundberg, *The Science of the Singing Voice* 1987).
The source-filter idea is the classic one (parallel formant synthesis, Holmes' alternating formant signs); Plaits'
vowel/formant models (MIT), VOSIM and FOF were read as references, no code from them or any other synthesizer or
library was used. The PolyBLEP polynomial is the SDK's (`sdk/include/opz_dsp.h`); finding the wraps per block
follows `engines/super`.
