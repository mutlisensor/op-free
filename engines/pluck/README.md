# pluck

Plucked and struck strings for the bass, lead, arp and chord tracks: one Karplus-Strong waveguide per voice, from
nylon and steel guitar strings through a stiff, piano-like string to metallic, bell-like tines, with a bowed sustain
at the top of the decay knob. Velocity shapes the pick or hammer (soft: a dark thump, hard: a bright, thin attack),
note-off damps the string like a hand on it, and every note is in tune at any setting. The OP-Z's own track filter
(the filter and resonance knobs) shapes the tone after it.

| Control | Function |
|---|---|
| P1 **material** | three zones. **0–84 nylon → steel:** plucked by a noise burst one period long; the string's high-frequency damping falls from nylon to steel (the 4 kHz region dies with a T60 of 0.14 s at 0 and 2.1 s at 84, the same on every note), so the tone goes from warm and soft to bright and ringing. **85–169 stiff string:** four all-pass filters in the loop take 0 → 30 % of the period (quadratic over the zone, so the start is fine-grained) and stretch the upper partials sharp like a piano string (the 10th partial is 0.8 % sharp at 128, 6 % at 169); over 85–127 the pick turns into a hammer (a smooth pulse), and the damping rises a little (4 kHz T60 1 s at 169). **170–255 metal:** the stretch grows to 70 % of the period and the partials stop lining up: at 255 they sit at 1, 2.34, 4.22, 6.51, 9.03 and 11.65 × the fundamental (bell, kalimba, gamelan); the damping falls to a 4 kHz T60 of 8.5 s, so the metal rings bright. The fundamental never moves with P1 |
| P2 **decay** | **0–191:** the fundamental's T60, 0.1 s at 0 → 25.6 s at 191, exponential (1.6 s at 96, 7.6 s at 150). Below 112 the string also gets darker, its high-frequency damping rising up to 8 × at 0: P2 0–40 is a palm-muted thunk. **192–255:** a bow-like noise feed sustains the string while the note is held, from nothing at 192 to full at 255, at the same sustained level on every note; the T60 shortens from 25.6 s to 4 s on the way, so the bow takes hold faster (the pluck still starts each note) |
| velocity | level (linear: 64 is −6 dB); the pick's or hammer's brightness (two one-pole low-passes from the 2nd harmonic at velocity 0 to the 72nd at 127); the pluck position, from a quarter of the string at velocity 0 to an eighth at 127 (hard notes sound thinner and brighter; the hammer always strikes at an eighth); the hammer's width, from a quarter of the period to 1/64. Soft notes are darker and quieter than the level alone would make them |
| note-off | the OP-Z envelope releases the level as on every engine, **and a damper touches the string**: from the first release block the string's own decay is at most twice the release time (per period it keeps at most c^(P/2), c = the envelope's per-sample release multiplier, P = the period in samples), its high-frequency damping rises 4 × (it goes darker), and the bow lifts. A string that already decays faster is left as it is. Together a ringing note reaches −60 dB in ~0.6 × the time a sustained engine needs with the same release: release 20 → 0.25 s, 40 → 0.5 s, 90 → 1.1 s, 128 → 1.6 s (simulator, A3 at P2 191). Short steps therefore sound muted; let-ring presets use a long release |
| attack, decay, sustain | the OP-Z envelope, applied to the level. The string makes its own decay, so the presets use attack 0, sustain 255; a slow attack fades a plucked note in (pads), a lower sustain shortens the ring further |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |

How it works: the string is a 1,024-sample delay line read at a fractional delay (4-point Hermite), a one-pole loop
low-pass whose coefficient follows the pitch (`(1 − k)/k² = B / f0`, so the high-frequency damping B is per second,
the same on every note), the loop gain set from P2's T60, the four all-passes, and the exciter added before the
write. The delay is corrected for the phase delay of the low-pass and the all-passes at the fundamental, and the
gain for the loss of the low-pass and the interpolator there, so the pitch is exact and the fundamental decays as P2
says. The exciter is the noise burst (pick) and the smooth pulse (hammer), each minus itself a pluck position later
(no DC, the classic pluck-position comb), through two one-pole low-passes, limited at 0.41 of full scale with the
excess carried forward (the noise's rare peaks no longer make some notes 6 dB louder, and the DC stays zero). The
bow is noise at a level normalised by √(1 − g) through the same low-passes, minus its slow mean. The line is written
at 16 bits with error feedback: the part below the LSB is carried to the next sample, so the string decays to
silence exactly (plain rounding freezes it in a limit cycle, truncation over-damps the tail).

The coefficients are recomputed only when something changes: when the knobs move (at most every other block, so an
LFO on P1/P2 costs half), at note-off, or when the pitch has moved more than 3 %; smaller pitch moves (vibrato,
bends, glides) keep the filters and recompute only the two phase delays, so the pitch stays exact (within 0.04 cent
after a glide in the simulator). The delay ramps across the block, so bends and glides are smooth.

## Presets

Five per synth track. `slot` in `presets.json` is the track (0 bass, 1 lead, 2 arp, 3 chord); index 0 of each (in
**bold**) is the sound a track gets when it switches to pluck, the others are on the preset keys.

| Track | Preset | Octave | P1 | P2 | A | D | S | R | Level | Sound |
|---|---|---|---|---|---|---|---|---|---|---|
| bass | **pluck bass** | −2 | 50 | 120 | 0 | 0 | 255 | 30 | 210 | round picked bass guitar, muted by short steps |
| bass | palm bass | −2 | 70 | 24 | 0 | 0 | 255 | 20 | 215 | palm-muted thunk: short, dark, punchy |
| bass | upright | −2 | 8 | 96 | 0 | 0 | 255 | 40 | 215 | warm, woody double-bass pluck |
| bass | piano bass | −2 | 120 | 140 | 0 | 0 | 255 | 40 | 205 | hammered, slightly stretched piano-bass tone |
| bass | metal bass | −2 | 186 | 110 | 0 | 0 | 255 | 35 | 205 | clangy, metallic bass |
| lead | **steel gtr** | 0 | 75 | 150 | 0 | 0 | 255 | 60 | 190 | bright steel-string guitar |
| lead | nylon gtr | 0 | 20 | 130 | 0 | 0 | 255 | 60 | 195 | soft, warm classical guitar |
| lead | koto | 0 | 94 | 120 | 0 | 0 | 255 | 50 | 190 | twangy, slightly stiff plucked string |
| lead | e-bow | 0 | 70 | 240 | 40 | 0 | 255 | 70 | 185 | bowed, endlessly sustaining steel string with a soft attack |
| lead | banjo | 0 | 82 | 70 | 0 | 0 | 255 | 30 | 190 | bright, short and snappy |
| arp | **harp** | 0 | 30 | 150 | 0 | 0 | 255 | 100 | 195 | soft harp: rings on after short steps |
| arp | kalimba | +1 | 176 | 90 | 0 | 0 | 255 | 60 | 195 | metal tines with slightly inharmonic overtones |
| arp | dulcimer | 0 | 110 | 160 | 0 | 0 | 255 | 110 | 190 | hammered, shimmering dulcimer |
| arp | music box | +1 | 232 | 140 | 0 | 0 | 255 | 90 | 190 | small, bell-like metal comb |
| arp | pizzicato | 0 | 14 | 50 | 0 | 0 | 255 | 20 | 200 | short, dark plucked strings |
| chord | **steel pad** | 0 | 80 | 245 | 70 | 0 | 255 | 120 | 150 | bowed steel strings: slow swell, sustains while held |
| chord | piano | 0 | 128 | 165 | 0 | 0 | 255 | 70 | 160 | stiff, hammered string: a simple piano |
| chord | guitar | 0 | 60 | 150 | 0 | 0 | 255 | 70 | 160 | strummed acoustic guitar chords |
| chord | harpsichord | 0 | 84 | 120 | 0 | 0 | 255 | 30 | 160 | bright, plucked, quickly damped |
| chord | bell pad | 0 | 230 | 232 | 60 | 0 | 255 | 130 | 150 | bowed bells: an inharmonic, shimmering pad |

Every preset: filter 128, resonance 0, pan 128, sends 0, no LFO (the arp track keeps its own LFO values when a preset
applies). Chord presets sit lower in level because several voices add up. Names fit the OP-Z's 11 characters.
`python3 tools/opz.py presets check engines/pluck` reports 20 presets, 0 errors, 0 warnings. Not yet tuned by ear on
the device.

## Facts

| | |
|---|---|
| Plug | 61014, `trackmask` 0xF0 (bass, lead, arp, chord), version 1 |
| Memory slot | 6 (50 KB class), 42,516 of 51,200 bytes on Blackfin: 14,908 of code and data, 27,608 of `.bss`, of which 26,728 are the string pool |
| String pool | **13 lines of 1,024 + 4 samples (16 bits), 2,056 bytes each.** Sized for the worst case: every synth track on pluck at once, bass 2 + lead 3 + arp 2 + chord 6 voices (`generous_chords`; 4 without it) = 13, the OS's voice counts including a note still releasing. A voice claims a line at note-on and frees it when the host frees the voice; the lines belong to voices, not tracks. If a 14th voice ever asks (it can't on the OP-Z), it takes the line idle longest and the voice that loses it ends at once |
| Low notes | The keys run F3–E5 at octave 0 and the octave goes −4…+4, so the lowest note is F−1 (10.9 Hz, a 4,046-sample period): lines that long for 13 voices would need 105 KB. Instead a line holds periods up to 960 samples (≈46 Hz, with a semitone of bend-down headroom below 1,020); longer periods run the string at 1/2 (down to 23 Hz), 1/4 (11.5 Hz) or 1/8 of the sample rate, linearly interpolated back to 44.1 kHz (11 kHz bandwidth at 1/2). On the bass track's default octave (−2) only the lowest key, F1, runs at half rate; octave −3 (F0–E1) at half and quarter rate. A 10 KB slot would leave ~6 KB for lines (256 samples: native only above 172 Hz, the bass at 1/4–1/16 rate and a few kHz of bandwidth), so the engine stays in the 50 KB slot 6 |
| State | 156 B of 280 per voice |
| Tuning | the fundamental within 0.6 cent of the note from C0 (16 Hz) to C8 at every P1 (simulator, measured by DFT), except C8 on nylon (3 cents as measured: it dies within ~0.1 s there). Glides and bends end within 0.04 cent |
| Decay | the fundamental's T60 as P2 sets it: 1.62 s at P2 96 (1.6 set), 7.74 s at 150 (7.6), 25.4 s at 191 (25.6), A1 to A3 and up to A5 for steel and stiff strings (10 % shorter in the metal zone). Very high notes ring shorter, because the loop's own losses there can't be made up with a gain below 1: at P2 191, E6 / C7 / E7 ring 17 / 7.4 / 3.1 s on steel (P1 84), 12 / 5.2 / 2.5 s at P1 128, 23 / 10 / 12 s on metal (P1 255); nylon (P1 0) damps its high notes hard (A5 2.8 s, E6 ~0.5 s). Decays reach silence: the residual floor is ~−107 dB |
| Level | −12.1 dBFS median per voice at velocity 127 in every zone (OPZ_VOICE_PEAK), −10.6 dBFS at most, C1–G♯7; the top notes (A7 up) −13 to −16 dBFS. Velocity 60: −18.6 dBFS median. Bowed sustain (P2 255) holds 4–6 dB under the pluck's attack on every note |
| CPU | Blackfin simulator ([cycle-model.md](../../research/cycle-model.md), `python3 tools/opz_cycles.py engines/pluck`), cycles per sample per voice, mean of 100 blocks: **plucked 116, stiff and metal 124–125, bowed 145–154, sub-rate low notes 92, high notes (C6–C8) 170**; 13 voices, bowed metal: 165 (10,551 cycles/block/voice, ~137,000 per block for all 13). With the pitch moving every block (vibrato, bends): 122–171 (13 bowed metal voices 186); with P1 or P2 moving every block (an LFO): 138–194 (13 voices 195). The most a voice costs is its note's first block, ~16,600–22,900 cycles (260–360 per sample: the coefficients, the exciter's set-up, the attack and the burst); 13 notes starting in the same block take ~262,000. Note-on ~700 cycles (it clears the line) |

Simulator checks (opzsim, `tests/`): eight golden scenarios (bass line with the sub-rate notes, guitar velocities,
harp and kalimba arpeggios, piano chords with voice steals, bowed pad and e-bow, a P1 sweep through the three zones
with bends, every synth track on pluck at once, the knobs at both ends); all pass the host's state guard and "adds
into out" probe, and every voice is freed after its release. The P1 sweep shows no click at the zone boundaries:
the only blocks whose second difference stands out from their neighbours are note-ons.

## What to listen for on the device

- **Velocity:** play the steel gtr or nylon gtr presets with soft and hard notes (or step velocities): soft should be
  a dark, round thump, hard a bright, thinner pick. The piano preset should bark more when struck hard.
- **Tuning:** compare a few notes with another engine or a tuner, then sweep P1 slowly under a held note: the timbre
  goes nylon → steel → stiff → metal without the pitch moving, the hammer coming in around 85–127, and no clicks at
  85 and 170. High notes on metal should ring as bells, out of tune with their own overtones but not with the key.
- **Decay and mute:** P2 from 0 (palm-muted thunk) to 191 (rings for many seconds); above 192 the note should sustain
  while held (bowed) and stop when released. Sequence one-step notes: they should sound damped, not cut. Raise the
  release: they ring on, darker.
- **Low notes:** on the bass track at octave −2 and −3: every note in tune, the lowest (F1 and below, half rate)
  only a little softer at the top, no grit.
- **Voices:** put pluck on all four synth tracks (with `generous_chords` on, too), play full chords everywhere: no
  note should cut out or lose its string. Hold a chord while new notes steal voices: no clicks.
- **CPU:** 13 bowed voices at once (steel pad / bell pad chords plus the other tracks), then with an LFO on P1:
  listen for crackles, and whether the OS kills releasing notes (see the runbook's step 11).

## Credits

Original code. The design follows the Karplus-Strong plucked string (Karplus and Strong, 1983) and its digital
waveguide extensions (Jaffe and Smith's pitch and decay corrections; Van Duyne and Smith's all-pass dispersion for
stiff strings), in the spirit of the inharmonic string model of Mutable Instruments' Plaits and the OP-1's string
engine. No code from either (no TE code at all). MIT licence.
