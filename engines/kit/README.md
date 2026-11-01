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

Knobs are read when a hit starts. Each hit ends itself at −60 dB and frees its voice; the OP-Z envelope is not used.

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
| Plug | 61020, version 5, `trackmask` 0x0F (kick, snare, perc, sample). The device does not enforce the mask. flags 24: P1 and P2 bipolar, a centre-detent cue at 128 ([host-tables-flags.md](../../research/host-tables-flags.md#recommended-flags-for-our-engines)) |
| Memory slot | 54 (10 KB class), 9.7 KB used with 1.2 KB of `.bss` (`onNoteOn` at `-Os`) |
| State | 164 B of 280 per voice |
| CPU | Blackfin (simulator, [cycle-model.md](../../research/cycle-model.md)), v4: **5,600–17,000 cycles/block/voice** by key (88–265 per sample; v3: 18,000–87,000). Hats/cymbal 10,300 (161), toms/conga/hi-q 12,400 (194), kicks 9,000–13,200, snares 16,300–17,000 (254–265, the only models over the 218/sample 19-voice budget); a kick's, tom's or snare's first block is up to 19,200 (the click). P2 changes the length, not the cost. Note-on ~1,800 (~4,200 for the first hit of a filtered key at a new tune). 25 hat voices fit in 265,000. Image 9.7 of 10 KB (1.2 KB of it `.bss`: block buffers and the filter cache) |

Simulator checks: all 24 keys sound and end; RMS over each key's first quarter second −38…−17 dBFS; the choke
drops an open hat by ~16 dB; a three-track groove peaks at −6.7 dBFS. With the tone filter and drive removed, the
renders at the old neutral setting (filter open, no drive) are bit-identical to kit v2.

## Credits

Original code following the classic analog drum circuits (TR-808/909), as also explored by Mutable Instruments
Plaits' and Peaks' drum models. MIT licence.
