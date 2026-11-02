# chip

An 8-bit console voice for the bass, lead, arp and chord tracks: the pulse waves with the classic duty cycles,
the 4-bit stepped triangle, a 4-bit saw, LFSR noise (long hiss and the short "metallic" loop) and a fast built-in
arpeggio at console frame rates. The sound of chiptunes, after the NES and Game Boy sound chips.

It is authentic but not harsh: the waves keep their hard edges and the triangle and saw keep their 4-bit staircase,
but every output sample is the wave's exact average over that sample (see [How it works](#how-it-works)), so the
alias products that make naive emulations sound gritty are 20–40 dB weaker, and a gentle output stage rounds off
the top octave of high notes. The OP-Z's own track filter (the filter and resonance knobs) shapes the tone after it.

| Control | Function |
|---|---|
| P1 **wave** | 8 steps of 32, stepped like the real chips (no morphing): see the table below |
| P2 **arpeggio** | 0 plain; 1–31 a delayed vibrato; 32–223 six arpeggios at four speeds; 224–255 pitch sweeps: see the table below |
| velocity | the console's 4-bit volume: 16 linear steps (127 → 15/15, 64 → 8/15, the softest notes 1/15) |
| attack … release | the OP-Z envelope |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |

### P1 wave

| P1 | Wave | Notes |
|---|---|---|
| 0–31 | pulse 12.5 % | thin and nasal; peak −12 dBFS, quieter than the others (RMS −21 dBFS), as on the chips |
| 32–63 | pulse 25 % | the classic chiptune lead; RMS −17.5 dBFS |
| 64–95 | pulse 50 % | square: hollow, odd harmonics only; peak −15 dBFS (RMS −15.7), so it is not louder than the others |
| 96–127 | duty cycling | the pulse steps 12.5 → 25 → 50 → 25 % every 2 frames (30 Hz, a 7.5 Hz cycle): the shimmering lead of many soundtracks. Restarts at 12.5 % with every note |
| 128–159 | triangle, 4-bit | 32 steps of 16 levels (15, 14 … 0, 0, 1 … 15): the NES bass, soft with a faint buzz from the staircase |
| 160–191 | saw, 4-bit | a falling ramp of 16 levels (a Game Boy wave-channel sound): brighter than the triangle |
| 192–223 | noise | a 15-bit LFSR (taps 0 and 1, 32,767 steps) clocked at 16 × the note frequency: a rumble at C1, crunchy at C3 (2 kHz clock), hiss from C6; the clock stops rising at F7 (one step per sample). Peak −15 dBFS |
| 224–255 | metallic noise | the LFSR's short mode (taps 0 and 6): a 93-step loop played at the note's pitch, a buzzy, metallic tone. Notes above A#7 (3.8 kHz) stay at A#7. Peak −15 dBFS |

### P2 arpeggio

| P2 | Mode |
|---|---|
| 0 | plain: the note as played |
| 1–31 | delayed vibrato: after 0.3 s the pitch swings ±P2/31 semitone at 5.5 Hz (triangle), up to ±1 semitone at 31 |
| 32–63 | arpeggio **octave**: 0, +12 |
| 64–95 | arpeggio **major**: 0, +4, +7 |
| 96–127 | arpeggio **minor**: 0, +3, +7 |
| 128–159 | arpeggio **fifth**: 0, +7, +12 |
| 160–191 | arpeggio **sus4**: 0, +5, +7 |
| 192–223 | arpeggio **seventh**: 0, +4, +7, +10 |
| 224–227 / 228–231 / 232–235 / 236–239 | pitch sweep **down** at 2 / 5 / 12 / 30 octaves per second from the note-on, stopping 4 octaves down |
| 240–243 / 244–247 / 248–251 / 252–255 | pitch sweep **up** at 2 / 5 / 12 / 30 octaves per second, stopping 2 octaves up (and at 11 kHz) |

Within each arpeggio zone the low 5 bits set the speed, in frames of 60 Hz: **+0–7 a new note every 4 frames
(15 Hz), +8–15 every 3 (20 Hz), +16–23 every 2 (30 Hz), +24–31 every frame (60 Hz)**. For example P2 32 is the
octave arpeggio at 15 Hz, 56 the octave at 60 Hz, 88 major at 60 Hz, 176 sus4 at 30 Hz. Every note starts on its root
and the pattern loops upwards (0 4 7 0 4 7 …). At 60 Hz the arpeggio fuses into a buzzing chord, the classic way one
chip channel plays a chord; at 15 Hz it is a quick broken chord. The arpeggio moves the noises too (noise
arpeggios are a classic chip-drum trick).

The frame clock runs per voice from the note-on and lands on the OP-Z's 1.45 ms blocks (frames of 11 or 12 blocks).
Pitch changes from the arpeggio, vibrato and sweep keep the wave's phase, as on the real chips. The host's pitch
(portamento, bend, the LFO's pitch target) moves underneath. With an LFO on P1 or P2 the engine steps through the
waves or modes.

## How it works

Every wave but the long noise is a table of N steps (8 for the pulses, 32 triangle, 16 saw, 93 metallic), stored as
its running integral: a piecewise-linear curve. Each output sample is the difference of that integral across the
sample's phase step, divided by the step: the exact average of the wave over the sample (first-order
antialiasing, all integer, no tables of band-limited waves). That is what a chip running much faster than 44.1 kHz
into a one-sample box filter would give: the edges stay sharp but most of their aliasing is gone. The long noise
steps its LFSR at most once per sample and averages across the step the same way. The tables are zero-sum (no DC):
pulses +14/−2, +12/−4, +8/−8 units of 1/16, the metallic loop +15/−16 on a 93-step cycle with 48 ones.

Then an output stage: from C5 up, a 2-tap average fades in, linear in frequency, to a full 2-sample box at C7. It
rounds off the top octave where high notes would otherwise fizz (−3.2 dB at 10 kHz on a C7) and leaves lower notes
bright.

Measured in opzsim (one voice, velocity 127; energy that is not on a harmonic of the note, relative to the
harmonics; a naive table emulation for comparison):

| Wave, note | below 2.5 kHz | 2.5–5 kHz | 5–10 kHz | 10–22 kHz | naive, below 5 kHz |
|---|---:|---:|---:|---:|---:|
| pulse 12.5 %, A4 | −63 dB | −50 | −42 | −27 | −25 |
| pulse 12.5 %, C7 | −46 | −41 | −38 | −29 | −16 |
| pulse 50 %, C7 | −62 | −44 | −39 | −34 | −21 |
| triangle, C7 | −81 | −65 | −63 | −53 | −32 |
| saw, C7 | −51 | −45 | −37 | −32 | −19 |

Levels (peak, any note): pulse 12.5 % and 25 %, duty cycling, triangle and saw −12.0 dBFS (the ABI's voice peak);
pulse 50 % and both noises −15.0 dBFS, since their RMS equals their peak. DC within 2·10⁻⁴ of full scale on the
tonal waves. Arpeggio, vibrato, sweep, the frame clock and the envelope run once per block; the envelope is
advanced in closed form (as in `vowel`) and applied as a ramp.

## Presets

Five per synth track. `slot` in `presets.json` is the track (0 bass, 1 lead, 2 arp, 3 chord); index 0 of each is the
sound a track gets when it switches to chip.

| Track | Preset | Octave | P1 | P2 | A | D | S | R | Level | Sound |
|---|---|---|---|---|---|---|---|---|---|---|
| bass | **tri bass** | −2 | 140 | 0 | 0 | 60 | 180 | 20 | 215 | the NES triangle bass |
| bass | pulse bass | −2 | 72 | 0 | 0 | 80 | 120 | 20 | 205 | square bass with a little decay |
| bass | oct bass | −2 | 140 | 60 | 0 | 70 | 150 | 20 | 210 | triangle with a 60 Hz octave arpeggio: a buzzing, fat bass |
| bass | thin bass | −2 | 40 | 0 | 0 | 50 | 80 | 16 | 205 | 25 % pulse, short and punchy |
| bass | drop bass | −2 | 140 | 228 | 0 | 50 | 0 | 20 | 215 | triangle hit that drops 5 octaves per second: a chip kick-bass |
| lead | **chip lead** | 0 | 40 | 20 | 0 | 90 | 200 | 40 | 195 | 25 % pulse with a delayed vibrato |
| lead | square | 0 | 72 | 12 | 0 | 90 | 220 | 40 | 190 | 50 % square, light vibrato |
| lead | duty lead | 0 | 110 | 24 | 0 | 90 | 210 | 40 | 195 | duty cycling with vibrato |
| lead | thin lead | 0 | 8 | 16 | 0 | 80 | 220 | 40 | 200 | 12.5 % pulse, nasal |
| lead | jump | 0 | 40 | 244 | 0 | 40 | 0 | 24 | 195 | the platformer jump: a short blip sweeping up |
| arp | **major arp** | 0 | 40 | 88 | 0 | 70 | 150 | 40 | 195 | every note a 60 Hz major chord |
| arp | minor arp | 0 | 72 | 120 | 0 | 70 | 150 | 40 | 190 | square, 60 Hz minor chord |
| arp | noise hat | 0 | 200 | 0 | 0 | 20 | 0 | 20 | 200 | short noise tick; higher notes are brighter |
| arp | metal hit | 0 | 232 | 0 | 0 | 30 | 0 | 30 | 200 | short metallic noise, pitched |
| arp | zap | 0 | 8 | 236 | 0 | 40 | 0 | 30 | 195 | laser zap: 12.5 % pulse falling 30 octaves per second |
| chord | **chip chord** | 0 | 40 | 0 | 0 | 100 | 180 | 60 | 160 | 25 % pulse chords |
| chord | tri organ | 0 | 140 | 0 | 10 | 100 | 230 | 60 | 165 | soft triangle chords |
| chord | saw pad | 0 | 170 | 10 | 60 | 120 | 200 | 90 | 155 | slow 4-bit saw pad with a slight vibrato |
| chord | sus arp | 0 | 72 | 176 | 0 | 90 | 170 | 60 | 155 | square, each note a 30 Hz sus4 arpeggio |
| chord | 7th arp | 0 | 40 | 216 | 0 | 90 | 170 | 60 | 155 | 25 % pulse, each note a 60 Hz seventh arpeggio |

Every preset: filter 128, resonance 0, pan 128, sends 0, portamento 0, LFO off (depth 128, speed 100, target 255,
shape 0). Chord presets sit lower in level because four voices add up. Names fit the OP-Z's 11 characters.
`python3 tools/opz.py presets check engines/chip` reports 0 warnings. Not yet tuned by ear on the device.

## What to listen for on the device

1. Install: `python3 tools/opz_build.py engines/chip`, then `.venv/bin/python tools/opz.py install engines/chip
   --track lead` (lead and arp may be full: use `--slot N --replace`). Restart the OP-Z if the plug does not show.
2. On the lead track, hold a note around C4 and turn P1 slowly from 0 to 255: eight distinct waves, each change a
   clean step (12.5 %, 25 %, 50 %, the shimmering duty cycle, triangle, saw, hiss, metallic tone). No crackle
   within a wave.
3. Play high notes (C6–C8) on the pulses and the saw: bright but not gritty, no whistling tones that move against
   the note when you bend.
4. Triangle at C1–C3 on the bass track: a soft bass with a faint buzz from the 4-bit steps (that is wanted).
5. P2: 0 plain; 1–31 vibrato that starts after ~0.3 s; then turn up through the arpeggio zones: octave, major,
   minor, fifth, sus4, seventh, each from 15 Hz (broken chord) to 60 Hz (a buzzing chord); 224–255 sweeps down and
   up from every note-on.
6. Velocity: 16 volume steps, soft notes stay audible.
7. Noise drums on the arp track: preset `noise hat` at C3, C5, C7 (darker to brighter); `metal hit` follows the
   pitch; `drop bass` and `zap` are clean falling sweeps.
8. **Turn P1 and P2 fast, back and forth, while chords play and retrigger on the chord track** (the situation that
   crashed macro v3): the sound jumps between waves and modes, but there must be no crash, hang or stuck note.
   Also play fast notes so voices are stolen, and lean on pitch bend.
9. Chord track, `chip chord` with four-note chords: the level stays under clipping; release tails end cleanly.

## Facts

| | |
|---|---|
| Plug | 61203, `trackmask` 0xF0 (bass, lead, arp, chord), version 1, engine version 1 |
| Memory slot | 81 (5 KB class): **3,392 of 5,120 bytes** on Blackfin (3,040 B code, tables and data, 352 B `.bss`: the 171 running-integral entries, built in `initModule`) |
| State | 32 B of the 280 B the OS leaves per voice; `memRequest` 0, no libgcc, no 64-bit maths, division or float |
| CPU | Blackfin simulator ([cycle-model.md](../../research/cycle-model.md), `tools/opz_cycles.py engines/chip`): **61–62 cycles/sample/voice** (3,900–4,000 per block) for every table wave, arpeggio, vibrato and sweep, with 4 or 19 voices, with P1/P2 moving every block, in release; noise 37–45. Worst: noise on C7–C8 with an octave arpeggio (an LFSR step every sample), 81 mean, 83 worst block. 19 voices ≈ 76,000 cycles per block against the OS's 265,000. Note-on 68 cycles. A stolen voice's fade (a few blocks) steps the envelope exactly per sample, as in `vowel` (not in the cases) |

Checks: `tests/fuzz.c` (UBSan with bounds: tables zero-sum and in range, the averaging exact against a double
reference over 200,000 random intervals, 300,000 fuzz blocks with random notes, velocity 0–255, `params` NULL in
`onNoteOn`, knob jumps every block, pitches 0 / 0xffffffff / random with random slopes, random envelope states and
re-use without init: render adds into out, L = R, never above the voice peak, deterministic, state in range). Six
golden scenarios in `tests/` (waves, arpeggios and sweeps, chip drums, bass, knobs moving while chords steal and
release, extremes).

## Credits

Original code (MIT licence, see [CONTRIBUTING.md](../../CONTRIBUTING.md)). The waves, duty cycles, LFSR taps and
the 93-step short mode follow the published hardware documentation of the NES 2A03 and Game Boy APUs; the idea of a
chiptune voice with a built-in arpeggio comes from Plaits 1.2's chiptune model and the OP-1's pulse synth. No code
from either was used.
