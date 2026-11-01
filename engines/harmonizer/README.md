# harmonizer

Two pitch-shifted voices over a short, moving delay network for FX1 and FX2 (X-11): a slow detune shimmer at the
centre of P1, harmonised intervals towards its ends, and keys on the FX track that play intervals while held.

| | |
|---|---|
| Kind | effect, FX1 and FX2 (`trackmask` 768 = 0x300), flags 8 (send-type FX; 0x08 centre cue on the P1 dial) |
| Slot | 35 (10 KB class), 2.6 KB used on the device build |
| Plug id | 61104 |
| P1 **shift** | bipolar around 128. **96–159: detune**, the two voices ± up to 25 cents (a wide doubler towards the zone's edges, nothing at 128). Outside it, **intervals** in steps of 16: up 160 +3 (minor third), 176 +4, 192 +5, 208 +7, 224 +9, 240–255 +12; down the same mirrored (80–95 −3 … 0–15 −12). Symmetric; both voices on the interval, 6 cents apart for thickness |
| P2 **space** | 0 = the voices alone; up: a stereo delay network (20 → 110 ms left, 1.37× right, slowly moving) mixes in and feeds back through the shifter (up to 0.85), so the harmony repeats and climbs (or falls) by the interval and smears into a swirl |
| pan | dry/wet, TE's balance law normalised to unity (centre = half and half). The preset is wet only, as a send |
| level | output level, times the FX track's mixer level and mute |
| Filter / resonance | the OP-Z's own track filter, also on FX tracks |
| **Notes on the FX track** | a held key sets the interval for as long as it is held: its distance from **C4 = unison** (G4 +7, C5 +12, F3 −7; clamped to ±24). Voice 1 takes the newest key, voice 2 the key held before it — **two keys = two-voice harmony** — or 6 cents below voice 1. Release returns to P1 (momentary; no latch). The OS's velocity-0 re-send of a held note keeps it held |
| Memory request | 64 KB (an 8,192-sample shifter ring and an 8,192-frame 16-bit stereo delay pair), cleared over the first 4 blocks (dry meanwhile) |
| State | 52 of 200 bytes (device) |
| CPU | Blackfin simulator (`tools/opz_cycles.py engines/harmonizer`): **396 cycles/frame** at every setting, notes and knobs moving included; worst block 461 (a clearing block), against the ≤ 600 effect target |

Not for master: the OS passes `out = NULL` there ([engine-abi.md](../../sdk/docs/engine-abi.md) §3).

## How it works

The input (mono sum, plus the feedback) goes into a ring; each voice reads it with two taps whose delays sweep
across a 4,096-sample window (93 ms) half a window apart, at a rate set by the ratio, crossfaded by triangles — the
classic delay-line pitch shifter, as `shimmer` uses — with +3.5 dB to make up what the crossfade loses. Voice 1
leans left, voice 2 right. The voices feed a stereo pair of delay lines whose lengths glide as P2 moves; their mean,
band-limited (150 Hz–5 kHz) and softly saturated, goes back into the shifter.

Like every shifter of this kind it lands a **pure tone** up to half the window rate off the interval (the taps'
phase steps: up to ±5 Hz at +12, ±2.6 Hz at +7); sounds with many partials average out on it. Measured on a 150 Hz
saw: +12 within 4 cents, +7 within 2, −12 within 12; on a 400 Hz sine +12 is 19 cents sharp.

## Preset

FX tracks apply only slot 0 index 0, when the plug is chosen. **halo**: P1 150 (detune, ±17 cents), P2 110,
pan 255 (wet only), level 255, byte 12 (FX dry level) 255 so the sending track keeps its dry sound. Settings to try:

| Name | P1 | P2 | Sound |
|---|---|---|---|
| halo (preset) | 150 | 110 | wide detuned doubling with a short moving space |
| fifths | 216 | 60 | a fifth above, tight |
| octave swirl | 250 | 230 | an octave up, cascading upwards |
| sub | 5 | 40 | an octave down |
| thirds | 176 | 140 | a major third above, roomy |

## What to listen for

1. Install on FX2 (or FX1); send a lead or a pad. P1 at 128, P2 0: very little (the voices sit on the input). Move
   P1 to ~150: a wide, slowly beating doubling; back to 128: it closes again.
2. P1 through the upper zones: minor third, major third, fourth, fifth, sixth, octave above, each a clean step; the
   lower half the same intervals below. A slight warble is normal; a steady out-of-tune interval is not.
3. P2 up on a fifth: the harmony repeats, climbs by fifths and fades into a swirl; it never builds up or distorts.
4. Hold keys on the FX track with P1 at 128: G4 a fifth up, C5 an octave up, F3 a fifth down; hold G4 and E4 for a
   two-voice harmony; release returns to P1.
5. Turn both knobs quickly while keys play: no clicks (the delays glide), no level jumps.

## Tests

[tests/](tests/): `intervals` (P1 swept −12 → detune → +12 on a saw, voices alone), `space` (a fifth up with P2
swept to full feedback, clicks), `notes` (G4, then G4 + E4 for two voices, E4 alone, C3, back to P1), `extremes`.
A full-scale saw at P2 255 stays bounded (peak 0 dBFS through the saturating adds, nothing wraps); an impulse at
P2 255 dies away within ~1.5 s.

## Credits

The two-tap crossfaded delay shifter is the classic one (as in Mutable Instruments Clouds' pitch shifter, MIT; no
code from it — ours is `shimmer`'s idea, written again). The behaviour is inspired by TE's unreleased `Swirl`
(a pitch-shift ambience that follows held keys; docs/ideas/effects.md X-11): our own
delay lengths, modulation, symmetric intervals and code, none of TE's. MIT licence.
