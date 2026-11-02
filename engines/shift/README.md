# shift

Ring modulator and frequency shifter in one knob for FX1 and FX2 (X-10); keys on the FX track tune the carrier.

| | |
|---|---|
| Kind | effect, FX1 and FX2 (`trackmask` 768 = 0x300), flags 17 (0x01 insert-type FX, 0x10 centre cue on the P2 dial: ring mode at 128) |
| Slot | 36 (10 KB class), 1.6 KB used on the device build (it would fit 5 KB; our last 5 KB slot went to `repeat`) |
| Plug id | 61103 |
| P1 **carrier** | 0.2 Hz (0) → 3.3 kHz (255), exponential over 14 octaves (128 ≈ 26 Hz). Low settings: tremolo (ring) or slow barber-pole phasing (shift); higher ones the metallic, inharmonic sounds |
| P2 **mode** | a continuous morph: 0 = **shift down** (lower sideband only), 128 = **ring modulation** (both sidebands), 255 = **shift up** (upper sideband only); in between one sideband fades against the other. The level is evened out (ring mode +3 dB), so every setting is about as loud as the input |
| pan | dry/wet, TE's balance law normalised to unity (centre = half and half). The preset is wet only, an insert |
| level | output level, times the FX track's mixer level and mute |
| Filter / resonance | the OP-Z's own track filter, also on FX tracks |
| **Notes on the FX track** | a held key sets the carrier to the note's frequency (A4 = 440 Hz): tuned ring modulation, or a shift by a musical amount; release returns to P1. The newest note wins; the OS's velocity-0 re-send of a held note sets the carrier the same way |
| Memory request | 0 (everything is in the FX state) |
| State | 180 of 200 bytes (device) |
| CPU | Blackfin simulator (`tools/opz_cycles.py engines/shift`): **372 cycles/frame** at every setting, notes and knob moves included (worst block 372), against the ≤ 600 effect target |

Not for master: the OS passes `out = NULL` there ([engine-abi.md](../../sdk/docs/engine-abi.md) §3).

## How it works

A 90° phase splitter — two chains of four second-order all-passes, the second fed one sample later — turns each
channel into an analytic pair (I, Q), 90° apart within ±1.9° from 30 Hz to 20 kHz. Then
`out = I·cos ωt − m·Q·sin ωt` with m = −1 … 0 … 1 from P2: m = 1 keeps only the upper sideband, m = −1 only the
lower, m = 0 is plain ring modulation. The carrier comes from the host's 8,192-point sine table. Nothing is
band-limited before the shift, so content shifted past 22 kHz folds back. The all-pass coefficients are our own
minimax design (Nelder–Mead on the phase error).

## Preset

FX tracks apply only slot 0 index 0, when the plug is chosen. **bell ring**: P1 170 (~130 Hz), P2 128 (ring),
pan 255 (wet only), level 255, byte 12 (FX dry level) 0 for an insert. Settings to try:

| Name | P1 | P2 | Sound |
|---|---|---|---|
| bell ring (preset) | 170 | 128 | metallic ring modulation |
| tremolo | 60 | 128 | ~2 Hz amplitude wobble |
| barber pole | 40 | 255 | an endless slow phasing (shift up by ~1 Hz) at pan 128 |
| detune up | 120 | 255 | everything ~19 Hz higher: inharmonic, chorus-like at pan 128 |
| dalek | 200 | 128 | a ~400 Hz ring on a voice or a synth |
| down shift | 190 | 0 | the whole sound shifted ~280 Hz down: hollow and dark |

## What to listen for

1. Install on FX1, send a synth chord. P2 at 128, P1 slowly up: tremolo, then the ring-mod "bell" tones; no
   clicks while turning.
2. P2 to 255, P1 low (~40) at pan 128: a smooth endless phasing that only moves one way. P2 to 0: it moves the
   other way. Check the unwanted sideband is gone (no "both directions" beating): it should be ~35 dB down.
3. P1 higher with P2 at 255: the sound shifted up by a fixed amount in Hz, inharmonic; P2 0: shifted down.
4. Hold keys on FX1 in ring mode: the carrier follows the key (A4 = 440 Hz); release returns to P1. A sequenced
   bass line on FX1 against a sustained chord gives a tuned, moving ring modulation.
5. Turn both knobs quickly while notes play: no zipper noise, no level jumps across P2.

## Tests

[tests/](tests/): `morph` (P2 swept down-shift → ring → up-shift on a saw), `carrier` (P1 swept over the whole range
in ring mode, then up-shift, at 50/50), `notes` (keys A2, E3, A3, C5 as carriers), `extremes`. Measured on sines
of 100 Hz, 1 kHz and 8 kHz with a 204 Hz carrier: the kept sideband at −6.0 dBFS, the other at −42 dBFS (36 dB
rejection) in both shift modes; ring mode −9.0/−9.1 dBFS per sideband; RMS out = RMS in (−9.0 dBFS) at P2 0, 128
and 255.

## Credits

Clean-room design: the analytic-signal frequency shifter (Bode) and the IIR all-pass Hilbert pair are textbook
structures; our own coefficients. Inspired by Mutable Instruments Warps' frequency shifter and the Bode shifter —
behaviour only, no code. MIT licence.
