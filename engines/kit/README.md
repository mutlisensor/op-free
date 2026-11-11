# kit

Synthesized drum kit for the kick, snare, perc and sample tracks — no samples. Each of the 24 keys plays its own
model, so one plug covers a whole kit; put it on several drum tracks for separate levels and effects.

| Keys | Models |
|---|---|
| 1–4 | kick: deep, punchy, boom, hard |
| 5–8 | snare 808, tight snare, clap (three bursts and a tail), rim |
| 9–12 | closed hat, pedal hat, open hat, cymbal — the closed and pedal hats choke the open hat and cymbal on the same track |
| 13–16 | low, mid and high tom, conga |
| 17–20 | cowbell, clave, shaker, zap |
| 21–24 | blip, noise burst, hi-q, metal hit |

The first key is F3 (MIDI 53): key index = (note − 53) mod 24. **To confirm on hardware** which note numbers the
OP-Z sends for its drum keys.

| Control | Function |
|---|---|
| P1 **tune** | ±1 octave on every pitch and filter of the model (128 = nominal) |
| P2 **decay** | ×¼ … ×2 on every decay (128 = nominal) |
| velocity | level |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |
| pan, level | the track's pan and level (yellow and red dial, orange page), applied by the engine as TE's engines do (`opz_mix.h`): level is linear (128 = −6 dB, 0 = silent), pan a balance (centre: both sides full; hard left: right silent). The FX1/FX2 sends are taken after them. They follow the LFO (targets pan and volume) without zipping |
| attack, decay, sustain, release (the green page) | the OP-Z's drum envelope over each hit, as on TE's sample kits ([engine-abi.md](../../sdk/docs/engine-abi.md#the-drum-envelope-updateparametersahd), `opz_drumenv` in `opz_adsr.h`), read at the hit. **attack** moves the envelope's start into the hit: the hit gets shorter in proportion (on a sample it skips the start; a synthesized strike stays). **decay** fades the hit in, up to half its length (a swell: the peak drops as the natural decay runs underneath). **sustain** is how long it holds at full level: 255 the whole hit, 128 about half, 32 an eighth, 0 a click (12 ms). **release** fades it after the hold: below 128 the fade's length (0 a cut), from 128 the full length with the curve bending from a straight line to "hold, then drop". The presets' 0 / 0 / 255 / 0 leave every hit exactly as designed |
| note style | **gate**: releasing the key fades the hit out in ~20 ms (the step length on a sequenced note); retrig, mono and loop play one-shots |

Knobs are read when a hit starts. Each hit ends itself at −60 dB and frees its voice, or when the envelope page ends it first.

## Building blocks

Pitch-swept sine/triangle/square body, a second tone, six TR-808 hi-hat square oscillators (205–800 Hz), filtered
noise (low/band/high-pass), a click, one-pole decays, and a soft clipper for the loudest hits. All filter and decay
coefficients are computed once per hit.

## Presets

analog · tight · big room · snappy · low · deep · pitched up · blips — all in slot 0 (indexes 0–7), the only slot
drum tracks read; **analog** (index 0, both knobs at 128) is the sound a drum track gets when it switches to kit
([presets.md](../../research/presets.md)).

## Facts

| | |
|---|---|
| Plug | 61020, version 6 (v6: applies the track's level and pan, `opz_mix.h`, and gains the envelope page and gate style, `opz_drumenv`), `trackmask` 0x0F (kick, snare, perc, sample). The device does not enforce the mask. flags 24: P1 and P2 bipolar, a centre-detent cue at 128 ([host-tables-flags.md](../../research/host-tables-flags.md#recommended-flags-for-our-engines)) |
| Memory slot | **3 (50 KB class)** since v6: 10,712 of 51,200 bytes with 1.2 KB of `.bss`. Level and pan (`opz_mix.h`) and the envelope page (`opz_drumenv`) together took kit 472 B past its old 10 KB slot 54; trimming the specialised noise loops instead would cost hats and cymbals 48 cycles/sample, so it moved. A size trim could bring it back to a 10 KB slot later |
| State | 208 B of 280 per voice (40 of them the envelope page, 4 the level and pan ramp) |
| CPU | Blackfin (simulator, [cycle-model.md](../../research/cycle-model.md), `opz_cycles.py engines/kit`), v6: **means 96–272 cycles/sample** by key, worst block 311 (the first block of a snare, with the click); the host's `updateParametersAHD` is counted inside render as on the device. Snares are over the 218/sample 19-voice budget, as they were in v4; hats, toms, kicks within it. Note-on up to ~4,700. P2 and the envelope page change the length, not the cost. Before v6 (v4): 88–265 per sample; v3: 18,000–87,000 per block |

Simulator checks: all 24 keys sound and end; RMS over each key's first quarter second −38…−17 dBFS; the choke
drops an open hat by ~16 dB; a three-track groove peaks at −6.7 dBFS. With the tone filter and drive removed, the
renders at the old neutral setting (filter open, no drive) are bit-identical to kit v2.

## Credits

Original code following the classic analog drum circuits (TR-808/909), as also explored by Mutable Instruments
Plaits' and Peaks' drum models. MIT licence.
