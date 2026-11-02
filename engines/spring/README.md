# spring

Dispersive spring reverb for FX1 and FX2 (X-08): two springs whose echoes chirp — the descending "drip" of a spring
tank — and a tank you can kick from the FX track's keys.

| | |
|---|---|
| Kind | effect, FX1 and FX2 (`trackmask` 768 = 0x300), flags 0 (send-type FX) |
| Slot | 38 (10 KB class), 2.5 KB used on the device build |
| Plug id | 61101 |
| P1 **tension** | loose (0) → tight (255): spring A's round trip 75 → 25 ms (spring B 12.7 % longer); late decay (T60, measured) ~3.1 s → ~2.5 s. Turning P1 glides the length (1/8 sample per sample), so the tail bends like a stretched spring instead of clicking |
| P2 **drip** | how much each pass disperses and how bright it stays: chirp coefficient 0.35 → 0.82 (the low end of an echo trails the top by 1.7 → 11 ms per pass, more with every pass) and the loop damping 2.5 → 6.5 kHz. 0 = smooth and dark; 255 = the surf-guitar "pew" on every hit |
| pan | dry/wet, TE's balance law normalised to unity (centre = half and half). The preset is wet only, as a send |
| level | output level, times the FX track's mixer level and mute |
| Filter / resonance | the OP-Z's own track filter, also on FX tracks |
| **Notes on the FX track** | **kick the tank**: a noise burst (20 ms decay) into both springs, low-passed at 8× the note's frequency (C2 a thud, C6 a bright crash), as loud as the velocity. Note off does nothing; the OS's velocity-0 re-send of a held note is ignored |
| Memory request | 41 KB (two 4,096-sample delay lines, the all-pass memories, four diffusers), cleared over the first 6 blocks (dry meanwhile) |
| State | 84 of 200 bytes (device) |
| CPU | Blackfin simulator (`tools/opz_cycles.py engines/spring`): **550 cycles/frame** steady at any knob setting, **589 worst block** (kicks arriving with knobs moving), against the ≤ 600 effect target. The simulator does not model DDR wait states, and the delay lines live in the FX pool in DDR: this is the engine of the batch to watch for `cpu overload` on the device |

Not for master: the OS passes `out = NULL` there ([engine-abi.md](../../sdk/docs/engine-abi.md) §3).

## How it works

Each spring is a feedback loop: input + feedback → **8 stretched first-order all-passes**
H(z) = (−a + z⁻⁶) / (1 − a z⁻⁶) — lossless, with a group delay of 6(1 + a)/(1 − a) samples at DC falling to
6(1 − a)/(1 + a) at 3.7 kHz, so the low end of each echo trails the top: the chirp → two short Schroeder all-passes
(5–9 ms) that smear the passes into a dense tail instead of a flutter echo → the spring's delay line (linear
interpolation) → a one-pole damping low-pass → the decay gain → back in. Spring A is the left output, spring B the
right, both fed from the mono sum band-limited to 100 Hz–5 kHz like a tank's transducers. The loops run at −24 dB
for headroom inside the all-passes; every add saturates.

## Preset

FX tracks apply only slot 0 index 0, when the plug is chosen. **tank**: P1 90, P2 170, pan 255 (wet only), level 255,
byte 12 (FX dry level) 255 so the sending track keeps its dry sound. Settings to try:

| Name | P1 | P2 | Sound |
|---|---|---|---|
| tank (preset) | 90 | 170 | a classic amp spring |
| surf | 40 | 255 | loose and drippy: the "pew" on every snare |
| dark room | 150 | 20 | short, smooth, no chirp |
| boing | 0 | 230 | longest spring: kick it with low keys |

## What to listen for

1. Install on FX2 (or FX1). Send a snare or a clean guitar-like synth to it: a reverb whose echoes chirp downwards
   ("pew") as P2 rises, smooth and dark at P2 0. No flutter (a hard repeating echo) and no metallic ring at one pitch.
2. Turn P1 slowly while the tail rings: the tail bends in pitch and gets shorter and tighter towards 255, no clicks.
3. Hold or sequence keys on the FX track with nothing sent: low keys thud, high keys crash, the tail rings out.
4. Snare hits at full send, P2 255: the tail must not build up or distort; `opz log` shows no `cpu overload`
   (spring is the heaviest of the batch: 589 cycles/frame in the simulator, see CPU above). If it overloads, note it
   and try one instance only.
5. Turn both knobs quickly while notes and audio play: no crackle, no stuck sound.

## Tests

[tests/](tests/): `hits` (clicks through every P2 and a P1 jump), `kick` (keys C2–C6 at several velocities, no input),
`tension` (P1 swept both ways on a saw at 50/50), `extremes`. Measured from the renders (noise at −20 dBFS RMS, wet
only): wet −17.8 … −15.4 dBFS RMS, peak below −2.5 dBFS; late T60 3.0–3.2 s at P1 0, 2.5 s at P1 255.

## Credits

The stretched all-pass chain is the standard dispersion model of spring reverbs: J. Parker, "Efficient dispersion
generation structures for spring reverb emulation", EURASIP JASP 2011; V. Välimäki, J. Parker, J. S. Abel,
"Parametric spring reverberation effect", JAES 58(7/8), 2010. Our structure, lengths and coefficients; no code
from either. MIT licence.
