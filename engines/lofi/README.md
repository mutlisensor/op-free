# lofi

Worn tape and cheap converters for FX1 and FX2 (X-12): wow and flutter, a narrowing band, saturation, hiss and
crackle, then bit and sample-rate reduction — and a tape stop on the FX track's keys.

| | |
|---|---|
| Kind | effect, FX1 and FX2 (`trackmask` 768 = 0x300), flags 1 (insert-type FX) |
| Slot | 37 (10 KB class), 3.6 KB used on the device build |
| Plug id | 61102 |
| P1 **age** | one macro, 0 = clean (transparent apart from a 12 ms delay): wow (0.6 Hz with a drifting rate, up to ±3 ms ≈ ±20 cents, grows as age²) and flutter (6.5 Hz, up to ±0.2 ms); the band narrows (low-pass 16 → 2.8 kHz, 12 dB/oct; high-pass 20 → 250 Hz); a soft saturator bends in (x − k·x³/4, k 0 → 1, drive 1× → 2×) |
| P2 **noise** | 0–127: tape hiss (−66 → −30 dBFS) and vinyl crackle (0 → 40 pops/s) fade in. 128–255: they stay, and the signal drops to fewer bits (16 → 4) at a lower sample rate (44.1 → 3 kHz, no anti-alias filter: the grit is the point) |
| pan | dry/wet, TE's balance law normalised to unity (centre = half and half). The preset is wet only, an insert |
| level | output level, times the FX track's mixer level and mute |
| Filter / resonance | the OP-Z's own track filter, also on FX tracks |
| **Notes on the FX track** | **tape stop**: a held key slows the tape to a halt, pitch and level falling together; the key sets how fast, F3 (left) 1.2 s … E5 (right) 0.16 s (other octaves repeat the pattern). Releasing the key returns to the live sound with a 12 ms crossfade. A new note restarts the stop from the current speed; the OS's velocity-0 re-send of a held note keeps it going |
| Memory request | 128 KB (a 32,768-frame stereo 16-bit tape ring), cleared over the first 8 blocks (dry meanwhile) |
| State | 116 of 200 bytes (device) |
| CPU | Blackfin simulator (`tools/opz_cycles.py engines/lofi`): 233 cycles/frame with no noise, 329–361 with noise, bits and rate, **worst block 477** (a tape stop with knobs moving every block), against the ≤ 600 effect target. Note-on ~500 |

Not for master: the OS passes `out = NULL` there ([engine-abi.md](../../sdk/docs/engine-abi.md) §3).

## How it works

Input → the tape: a ring read at a moving delay (12 ms + wow + flutter, linear interpolation; wow and flutter are
computed once per block and ramped across it; while the tape stops, the delay also grows by 1 − speed per sample, so
the pitch falls with the speed) → drive and saturation, scaled back by 1 / drive → high-pass, two-pole low-pass →
bits and rate → + hiss (low-passed noise per channel) and crackle (random pops, 0.4 ms decay).

## Preset

FX tracks apply only slot 0 index 0, when the plug is chosen. **cassette**: P1 110, P2 70, pan 255 (wet only),
level 255, byte 12 (FX dry level) 0 for an insert. Settings to try:

| Name | P1 | P2 | Sound |
|---|---|---|---|
| cassette (preset) | 110 | 70 | a played-out tape: gentle wobble, hiss, a little crackle |
| warped | 230 | 30 | seasick wow, dark band |
| phone | 255 | 150 | narrow band, 13 bits at ~20 kHz |
| sampler | 40 | 210 | clean band, 8 bits at ~6 kHz: crunchy aliasing |
| broken | 255 | 255 | 4 bits at 3 kHz under heavy noise |

## What to listen for

1. Install on FX1; send a chord pad to it at full send. P1 0, P2 0: the sound is unchanged (only 12 ms late).
2. Turn P1 up: the pitch starts to wobble (slowly, unevenly), the top end closes, the sound thickens. No zipper noise
   while turning.
3. P2 0 → 127: hiss and crackle fade in; past the middle the sound gets grainy (fewer bits) and then aliased and
   metallic (lower rate). At 255 it is still recognisable, not a full-scale noise.
4. Hold keys on FX1: the tape slows to a stop (left keys slowly, right keys fast) and stays silent while held;
   release: the live sound comes back cleanly. Sequence a stop at the end of a bar.
5. Turn both knobs quickly while keys play: no crackle beyond the intended one, no stuck stop.

## Tests

[tests/](tests/): `age` (P1 swept both ways on a saw, no noise), `noise` (P2 swept through hiss, crackle, bits and
rate on a sine), `stop` (stops on F3 held to a halt, A#4 released half-way, E5), `extremes`. Measured: at P1 0, P2 0
a sine comes out at exactly its level (residual −39 dB against the ideal delayed sine: the 16 kHz low-pass's phase);
hiss and crackle alone at P2 127 −30 dBFS RMS; F3's stop is −54 dB down 1.2 s after the key.

## Credits

Clean-room design (OP-1's *Phone* and worn cassettes as the inspiration; no code from any). MIT licence.
