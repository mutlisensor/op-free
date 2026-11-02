# super

JP-8000-style supersaw for the bass, lead, arp and chord tracks: seven band-limited saws detuned on the Roland
JP-8000's non-linear curve, with **real stereo width** (the side saws are panned across the field), free-running
phases that start at random on every note so chords shimmer, a centre-saw ↔ side-saws mix and a sub on the top of
P2. The OP-Z's own track filter (the filter and resonance knobs) shapes the tone after it.

| Control | Function |
|---|---|
| P1 **detune** | the spread of the seven saws, on the JP-8000's curve (slow at first, then steep at the very top). Outer pair (flattest and sharpest saw) at: 0 ±0.6 cents (near unison), 32 ±4, 64 ±9, 96 ±14, **128 ±19 (classic trance)**, 160 ±29, 192 ±46, 224 ±65, 240 ±77, 248 ±110, 255 −202/+177 cents (wide and wild). The middle pair moves about 0.57× and the inner pair 0.18× as far. The stereo width also opens over 0–32: mono at 0, full width from 32 (near unison the phases hardly move, so a spread would just lean to one side for seconds) |
| P2 **mix** | 0–192: the six side saws come in on the JP-8000's mix curves. 0: the centre saw alone (each side saw 27 dB down, a faint shimmer). 64: the six sides together 1.4 dB above the centre. 128: each side saw about level with the centre (+7 dB together). 192: sides dominant (+10 dB together). 192–255: a triangle sub one octave below the centre saw fades in, in the middle of the stereo field; at 255 it is 7 dB above the centre saw and about 7 dB under the whole voice. The level per channel stays the same over the whole range (within ~1 dB) |
| velocity | level (linear), and a little brightness: a gentle one-pole low-pass (6 dB/octave) at 3 kHz for velocity 0, ~7.4 kHz at 64 and 18 kHz at 127 (at C4), with half key tracking (½ octave per octave), capped at 18 kHz |
| attack … release | the OP-Z envelope |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |

**Stereo.** The centre saw sits in the middle. The six side saws are three pairs, one saw of each pair on each side,
with the flatter and sharper saws alternating between the sides so neither side sounds flat or sharp: outer pair
(−11 %/+10.7 % at full detune) at ±75 %, middle pair (+6.2 %/−6.3 %) at ±100 %, inner pair (−2 %/+2 %) at ±50 %. The
mono sum (L + R) / 2 is exactly the plain mono supersaw. The side signal goes through a gentle one-pole high-pass at
the note's fundamental (at least 150 Hz, at most 4 kHz), like the JP-8000's pitch-tracking high-pass, so low notes
stay mono-compatible.

**How it fits the CPU.** A weighted sum of naive saws is one straight ramp plus a step wherever one saw wraps. So
per block each saw costs only its start value, its slope and its few wraps; each wrap writes its step and its
two-sample PolyBLEP correction (the SDK's `opz_saw_blep` polynomial) into an event buffer, for the mid and the side
signal. Per sample the engine just integrates mid and side, filters them and writes L = M + S, R = M − S. The cost
hardly depends on the number of saws, so **all seven saws play at full polyphony**; no saws are dropped at high
voice counts. Every block restarts from the exact phases, so the integration's rounding never builds up (checked
against a per-sample sum of seven `opz_saw_blep` saws over 80,000 random blocks by [tests/events_check.c](tests/events_check.c): equal to −118 dBFS). All saws run
at the block's middle frequency, so a glide moves in 1.5 ms steps (phase-continuous, inaudible).

## Presets

Five per synth track. `slot` in `presets.json` is the track (0 bass, 1 lead, 2 arp, 3 chord); index 0 of each is the
sound a track gets when it switches to super.

| Track | Preset | Octave | P1 | P2 | A | D | S | R | Level | Sound |
|---|---|---|---|---|---|---|---|---|---|---|
| bass | **wide bass** | −2 | 100 | 220 | 0 | 80 | 160 | 30 | 200 | wide saw stack over a mono triangle sub; the side high-pass keeps the low end centred |
| bass | saw bass | −2 | 70 | 60 | 0 | 70 | 120 | 25 | 205 | mostly the centre saw, a little shimmer |
| bass | sub stack | −2 | 120 | 255 | 0 | 100 | 200 | 30 | 200 | full sub under the supersaw |
| bass | pluck bass | −2 | 110 | 200 | 0 | 45 | 0 | 25 | 210 | short plucked supersaw bass |
| bass | reese | −2 | 50 | 140 | 0 | 120 | 220 | 30 | 200 | slow beating, gently detuned bass |
| lead | **trance lead** | 0 | 128 | 170 | 0 | 90 | 200 | 50 | 190 | the classic trance supersaw lead |
| lead | anthem | 0 | 160 | 190 | 4 | 100 | 220 | 70 | 185 | wider, brighter festival lead |
| lead | soft lead | 0 | 90 | 110 | 20 | 100 | 230 | 60 | 190 | softer attack, less detune |
| lead | wild lead | 0 | 245 | 180 | 0 | 90 | 210 | 50 | 185 | wide and wild, almost chorus-like detune |
| lead | thin lead | 0 | 40 | 40 | 0 | 80 | 220 | 40 | 195 | near-single saw with a slight shimmer |
| arp | **shimmer arp** | 0 | 140 | 170 | 0 | 40 | 0 | 40 | 195 | short bright supersaw pluck |
| arp | pluck arp | 0 | 110 | 140 | 0 | 30 | 0 | 30 | 200 | tighter, shorter pluck |
| arp | gate arp | 0 | 128 | 180 | 0 | 60 | 180 | 15 | 190 | sustained, gated supersaw for trance gates |
| arp | bell arp | +1 | 60 | 100 | 0 | 50 | 0 | 70 | 195 | softer pluck with a longer tail |
| arp | rave arp | 0 | 200 | 190 | 0 | 36 | 40 | 30 | 190 | wide, very detuned rave stab arp |
| chord | **super pad** | 0 | 150 | 180 | 110 | 150 | 220 | 120 | 160 | slow-swelling wide supersaw pad |
| chord | trance stab | 0 | 135 | 170 | 0 | 50 | 0 | 40 | 165 | classic trance chord stab |
| chord | string pad | 0 | 100 | 130 | 90 | 140 | 230 | 110 | 160 | gentler, string-machine-like pad |
| chord | rave chord | 0 | 210 | 200 | 0 | 70 | 60 | 40 | 160 | very detuned rave chord |
| chord | wide keys | 0 | 80 | 120 | 0 | 100 | 100 | 70 | 165 | keys with a wide, moderate shimmer |

Every preset: filter 128, resonance 0, pan 128, sends 0, no LFO (the arp track keeps its own LFO values when a
preset applies). Chord presets sit lower in level because four voices add up. Names fit the OP-Z's 11 characters.
`python3 tools/opz.py presets check engines/super` reports 20 presets, no warnings. Not yet tuned by ear on the
device.

## Facts

| | |
|---|---|
| Plug | 61015, `trackmask` 0xF0 (bass, lead, arp, chord), version 1 |
| Memory slot | 49 (10 KB class), 4,976 of 10,240 bytes used on Blackfin (incl. 524 bytes of `.bss`: the shared event buffers) |
| State | 52 B of 280 per voice; `memRequest` 0 |
| Saws | 7 at every polyphony (centre + 3 stereo pairs), plus the triangle sub on P2 192–255 |
| CPU | Blackfin simulator ([cycle-model.md](../../research/cycle-model.md), `tools/opz_cycles.py`), 4-note chord C3 G3 C4 E4: **7,415 cycles/block/voice (116 per sample) with P2 ≤ 192, 8,580 (134) with the sub**; worst block 9,684 (151). P1 doesn't change the cost. High notes cost more (more wraps per block): C6 G6 C7 E7 with the sub, 10,990 mean (172), worst 13,065 (204). **19 voices** (notes 36–72, P2 255): 7,724 per voice (121), ~146,800 cycles per block in all (worst block per voice 9,831), against the OS's 265,000 before its own share, so 19 voices of super fit with ~45 % to spare. Note-on 422 cycles |
| Level | per channel ~−20.5 dBFS RMS at velocity 127 (a single saw at the ABI's −12 dBFS peak is −16.8), peaks around −8 dBFS once the side saws are in (the supersaw's crest factor), −14 dBFS at P2 0. A 4-note chord at full velocity peaks at −1 dBFS in the track buffer without clipping |

Simulator checks (opzsim, one voice, A3 unless noted): the level per channel stays within −20.2…−21.1 dBFS RMS for P2
0…255 at P1 128; L/R correlation 0.99 at P2 0 (mono), 0.5–0.6 with the sides in; at P1 0 the output is mono; on an
A1 "wide bass" the side signal is 13 dB under the mid below 200 Hz and 2–4 dB under it above 200 Hz; voices are
freed after release; steals fade through the host's stage 5.

## What to listen for on the device

Install it on the lead or chord track (it starts on that track's index-0 preset), or start from P1 128, P2 128 with
sustain up:

- **Stereo width:** on headphones the sound should be wide, the outer saws clearly to the left and right, without
  leaning to one side. Summed to mono (one earbud, or the speaker) it should still sound full, not hollow.
- **P1:** a slow sweep from 0 to ~200 goes from one fat saw to the classic trance shimmer to a wide chorus; the last
  part (240–255) jumps to a very wide, wild detune, on purpose. At 0 the image is mono, and it widens up to ~32.
- **P2:** 0 is one plain saw; turning it up brings the six side saws in until they dominate at ~192; above that a
  sub octave fades in under the stack. The loudness should stay about the same throughout.
- **Chords:** each chord should shimmer slightly differently (random start phases); no clicks on repeated notes.
- **Bass:** "wide bass" on headphones should be wide in the mids and centred in the low end.
- **Velocity:** soft steps are a little darker as well as quieter.
- **Level:** super is about 5 dB quieter (RMS) than macro's saw at the same level knob, to leave room for chords;
  say if it should be louder.
- **CPU:** with the step 11 stress layout super should cost about the same as macro at P2 0, well under macro at
  P2 ≥ 128.

## Credits

Original code, MIT licence. The detune curve, the per-saw offsets and the centre/side mix curves are Adam Szabo's
published measurements and polynomial fits of the Roland JP-8000's supersaw ("How to Emulate the Super Saw", 2010);
`gen_tables.py` tabulates them. No code from any synthesizer or library was used; the BLEP polynomial is the SDK's
(`sdk/include/opz_dsp.h`).
