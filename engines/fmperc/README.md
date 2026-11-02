# fmperc

Two-operator FM percussion for the kick, snare, perc and sample tracks — no samples. One sine modulator (with
feedback) phase-modulates one sine carrier, and each of the 24 keys plays its own recipe: pitch, ratio, index,
pitch sweep and decays. Metallic hits, wood blocks, toms with pitch sweeps, zaps, FM kicks and bells in one plug;
put it on several drum tracks for separate levels and effects.

## Keys

24 recipes in six families of four. The families follow [kit](../kit/README.md)'s key map (kicks first, then
snares, hats, toms, percussion, effects), so a pattern written for kit plays the matching FM sound. A table of 24
recipes rather than "pitch steps of a few families": FM's character is in the ratio, index and decays, which a
pitch step can't change, and 24 recipes cost 384 bytes.

| Key | Sound | Carrier, Hz | Ratio at P1 0 / 128 / 255 | Index, rad | Feedback, rad | Sweep, octaves | Index T60 | Level T60 |
|---|---|---|---|---|---|---|---|---|
| 1 | deep kick | 50 | 0.5 / 1.0 / 1.41 | 2.5 | – | 2.5 | 79 ms | 2.2 s |
| 2 | punch kick | 60 | 2.0 / 1.0 / 1.73 | 3.9 | – | 3.5 | 40 ms | 1.0 s |
| 3 | boom kick | 44 | 1.0 / 0.5 / 0.71 | 1.5 | – | 1.5 | 152 ms | 3.2 s |
| 4 | hard kick | 64 | 1.0 / 3.0 / 3.53 | 5.4 | 2.0 | 4 | 15 ms | 790 ms |
| 5 | snare | 185 | 1.0 / 1.41 / 2.27 | 4.9 | 8.8 | 0.75 | 181 ms | 609 ms |
| 6 | tight snare | 235 | 1.5 / 2.13 / 3.17 | 5.9 | 11.3 | 0.6 | 128 ms | 395 ms |
| 7 | crack (clap-like noise burst) | 900 | 1.0 / 3.71 / 5.33 | 7.8 | 12.5 | 0.5 | 304 ms | 347 ms |
| 8 | rim | 500 | 1.0 / 2.66 / 3.41 | 3.4 | – | 0.25 | 8 ms | 79 ms |
| 9 | closed hat (chokes 11, 12) | 6000 | 2.0 / 3.36 / 4.73 | 9.8 | 10.8 | – | 245 ms | 117 ms |
| 10 | pedal hat (chokes 11, 12) | 5600 | 2.0 / 3.36 / 4.73 | 9.3 | 10.8 | – | 431 ms | 245 ms |
| 11 | open hat | 6000 | 2.0 / 3.36 / 4.73 | 9.8 | 10.8 | – | 1.6 s | 1.2 s |
| 12 | cymbal | 4200 | 2.0 / 2.53 / 3.73 | 11.3 | 7.8 | – | 3.0 s | 3.0 s |
| 13 | low tom | 82 | 2.0 / 1.0 / 1.41 | 1.5 | – | 1 | 59 ms | 1.6 s |
| 14 | mid tom | 120 | 2.0 / 1.0 / 1.41 | 1.5 | – | 1 | 59 ms | 1.4 s |
| 15 | high tom | 170 | 2.0 / 1.0 / 1.41 | 1.5 | – | 1 | 59 ms | 1.2 s |
| 16 | syn tom (long sweep) | 150 | 2.0 / 1.0 / 1.41 | 2.2 | 1.0 | 2 | 117 ms | 1.2 s |
| 17 | low wood block | 520 | 1.0 / 1.66 / 2.4 | 2.9 | – | 0.4 | 12 ms | 197 ms |
| 18 | high wood block | 1050 | 1.0 / 1.66 / 2.4 | 2.9 | – | 0.4 | 12 ms | 152 ms |
| 19 | clave | 2350 | 2.0 / 1.0 / 1.41 | 1.2 | – | – | 10 ms | 117 ms |
| 20 | cowbell | 560 | 1.0 / 1.48 / 2.12 | 2.9 | 0.5 | – | 117 ms | 609 ms |
| 21 | bell | 600 | 2.0 / 1.41 / 2.76 | 4.4 | – | – | 899 ms | 2.5 s |
| 22 | glock | 1320 | 4.0 / 3.5 / 5.19 | 2.0 | – | – | 304 ms | 1.4 s |
| 23 | zap | 220 | 2.0 / 1.0 / 1.41 | 2.0 | – | 5 | 99 ms | 395 ms |
| 24 | laser | 330 | 1.0 / 0.5 / 0.71 | 2.9 | 2.9 | 6 | 197 ms | 609 ms |

Values at P2 128 and full velocity; T60 is the time to −60 dB. The first key is F3 (MIDI 53): key = (note − 53)
mod 24, octave = floor((note − 53) / 24). **The octave buttons transpose** every pitch of the key (carrier, sweep
start, modulator) by whole octaves, up to ±3; the carrier is held below 11 kHz.

How a hit works: the pitch envelope starts the carrier up to the sweep depth above its pitch and falls back
linearly in frequency (the modulator follows: the ratio stays fixed); the index and the modulator feedback decay
together, so the attack is bright and the tail darker; above ~1.5 rad the feedback turns the modulator into noise,
which is where the snares' and hats' noise comes from. The level decays on its own. All three envelopes are exact
exponentials per block, ramped over its 64 samples.

## Controls

| Control | 0 | 128 | 255 |
|---|---|---|---|
| P1 **ratio** (read every block: it moves under a ringing hit) | the key's harmonic ratio (whole or half numbers: tonal, woody, hollow) | the key's designed ratio (the table) | the key's inharmonic ratio: metallic, clangorous |
| P2 **sweep** (read at the hit) | no pitch sweep; index and level decays ×¼ (short and dry) | the table | sweep ×2 (e.g. the zap starts 10 octaves up, held at 11 kHz); decays ×2.8 (long, ringing) |

Between the marks P1 moves the ratio linearly (0 → 128, then 128 → 255), passing through the ratios in between;
P2 scales the sweep depth linearly (×P2/128) and the decay times exponentially (×2^(−2 … 0) below 128, ×2^(0 …
1.5) above). The pitch-sweep time does not change with P2. filter, resonance: not used: they are the OS's own track
filter, which runs after the engine. Presets leave it neutral (128, 0).

**Velocity**: level (linear, like kit) and the index and feedback ×0.3 (softest) … ×1 (127): harder hits are
brighter, soft hats and snares are tonal rather than noisy.

Each hit ends itself at −72 dBFS and frees its voice; the OP-Z envelope is not used. A stolen voice (the host's
stage 5/6) fades out within one block (1.45 ms); a closed or pedal hat chokes an open hat or cymbal on the same
track in ~30 ms.

## Presets

All in slot 0, indexes 0–4 (the only slot drum tracks read), LFO off (depth 128, speed 100, target 255, shape 0):

| # | Name | P1 | P2 | Character |
|---|---|---|---|---|
| 0 | fm drums | 128 | 128 | every key as designed |
| 1 | wood | 30 | 90 | near-harmonic ratios, shorter: woody, tonal |
| 2 | metal | 230 | 150 | inharmonic ratios, a little longer: clangy |
| 3 | zappy | 150 | 230 | deep pitch sweeps, long decays |
| 4 | tight | 110 | 40 | almost no sweep, short |

## What to listen for on the device

1. Put fmperc on the **perc** track (`opz.py install engines/fmperc --track perc`), preset 0. Play keys 1–24 one by
   one: each sounds and dies away by itself; 1–4 thump, 9–12 hiss, 21–22 ring.
2. Hats: hold the open hat (key 11), then hit the closed hat (key 9): the open hat stops at once.
3. Velocity: play the snare (key 5) softly and hard: soft is quieter and more tonal, hard louder and noisier.
4. P1: let the bell (key 21) ring at P2 255 and turn P1 slowly from 0 to 255: harmonic → clangorous, without clicks
   or zipper steps.
5. P2: on the kicks (keys 1–4), P2 0 is a dry blip with no sweep, P2 255 a long, deep boom.
6. Octaves: play key 1 and key 21 at every octave: they transpose, the top octave stays clean (no stuck tone).
7. **Fast pattern + knobs:** program a 16-step pattern with a hit on every step (kicks, hats, snare, bell) at a
   fast tempo, then turn P1 and P2 quickly back and forth across their whole range for half a minute while it
   plays: no crash, no stuck voices, no clicks beyond the sounds themselves (the macro v3 crash situation).
8. Retrigger: hammer one long key (boom kick, P2 255) on the kick track: each new hit starts at once.
9. Levels: put kit on the kick track and fmperc on the perc track: kicks and snares peak at about the same level
   (both −12 dBFS per hit at full velocity).

## CPU

Blackfin cycles in the GNU simulator (`tools/opz_cycles.py engines/fmperc`, [cycle-model.md](../../research/cycle-model.md)):

| Case | cycles/sample/voice | worst block |
|---|---:|---:|
| keys without feedback (kicks 1–3, rim, toms, wood, clave, bell, glock, zap) | 58.7 | 58.7 |
| keys with feedback (hard kick, snares, crack, hats, cymbal, syn tom, cowbell, laser) | 62.4–86.5 | 86.9 |
| 8 hits in one block (8 keys / 8 open hats / octaves −2 … +2) | 72.6–86.9 | 86.9 |
| knobs moving every block, period 8 / 40, 4 voices | 74.3 / 74.8 | 86.9 |

Every block of a hit costs the same: render does no per-hit setup, so the first block (and a retrigger's) is no
dearer than the rest. Far under the 218/sample 19-voice budget (and under the ABI's 150 target). Note-on 600–820
cycles. initModule 21.

## Size

3,186 of 5,120 bytes in memory slot 80 (5 KB class): code, the 24 recipes (384 B), a 257-entry Q15 sine table
(514 B, linearly interpolated: within −82 dB of a sine) and 144 B of `.bss` (the modulator block buffer, the choke
counters). Voice state 60 B of 280. No 64-bit maths, division or float anywhere; `onNoteOn` at `-Os`.

## Checks

`make -C sim test`: 7 golden scenarios (all24, groove, knobs, choke, retrig, octaves, velocity). Simulator levels
at velocity 120: peaks −12.6 … −13.5 dBFS on the kicks, snares, crack, rim, toms, woods, clave, cowbell, zap and
laser (kit's keys: −12.0 … −12.4), −14 … −23 dBFS on the
hats, cymbal and bells (kit's hats −12 … −18). `tests/fuzz.c` (UBSan + bounds; build line in the file): 10⁶ blocks
× 8 voices of random hits (notes −300 … 400, velocity 0 … 255, NULL params), retriggers, steals, re-inits, knob
words over the full 32 bits jumping every block, plus a grid of every key × octave × P1 × P2: 0 failures.

## Credits

Original code. Ideas from the classic FM drum voices: Peaks' FM drum (Mutable Instruments, MIT licence) — a
pitch-swept carrier with a decaying modulation index — and the DX7's percussion patches (ratios such as 1.41 and
3.5, operator feedback for noise). No code ported. MIT licence.
