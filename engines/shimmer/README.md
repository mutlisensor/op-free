# shimmer

A lush stereo reverb with a **pitch shifter inside its loop**, the Eventide/Strymon "shimmer" sound, for **FX1 and
FX2**. Every time the sound goes round the reverb, part of it comes back an octave higher (or a fifth, a fourth, two
octaves, an octave down: the keys on the FX track choose). The tail grows a halo above itself, and with P2 up it climbs
in a rising cascade that thins out into the highs. P1 takes it from a small room to an endless wash.

| Control | Function |
|---|---|
| P1 **size** | one macro over **size, decay and pre-delay**, as on `plate` (table below). 0 = a small, short room (T60 ~0.5 s, pre-delay 3 ms). Turning it up makes the room bigger (the long delays ×5 from 0 to 255) and the decay longer (×2 every ~37 steps), up to a huge hall at 240 (T60 ~20 s without shimmer, pre-delay 40 ms). **240–255: the decay runs on towards endless**. Without shimmer (P2 0) the tail at 255 never dies. With shimmer the cascade drains it into the highs, so even 255 fades (P2 64: −50 dB over 40 s; P2 255: gone in ~8 s). A limiter stops any build-up from clipping. Size changes **glide** (at most 1/8 sample per sample, ~1.5 s from end to end), so turning P1 bends the tail like tape instead of clicking |
| P2 **shimmer** | **how much of the loop passes through the pitch shifter each lap** (table below). **0 = a plain reverb**, no shifting. Rising: the interval fades in above the tail as a faint halo (64), a clear octave (128), as loud as the tail itself and climbing on (192), and **at the top (224–255) nearly all of it moves up every lap: a rising cascade** of octaves that gets brighter, then thins out and is gone in a few seconds. The shifted path also darkens a little as P2 rises (low-pass 8 → 5.7 kHz) so the top of the cascade stays smooth, not fizzy. Continuous at every value, so an LFO on P2 works |
| pan | dry/wet, as on `plate`, `filter` and `glue`: 0 dry, rising linearly to fully wet at the centre (128); 128–255 stay fully wet |
| level | output level (255 = unity) |
| filter, resonance | not used: they are the OS's own track filter, also on FX tracks. Presets leave it neutral (128, 0). A little high-pass on the track filter keeps a long shimmer from muddying the low end |
| **notes on its track** | **the shift interval**, as the key's distance from **C3** (table below): **C4 = an octave up (the default)**, G3 = a fifth, F3 = a fourth, C5 = two octaves, C3 = unison (shimmer off), keys below C3 shift down (C2 = an octave down). The newest note wins and **the interval stays after you let go** (it's a setting, not a gate), so sequence one note per bar to change the interval with the song. Velocity doesn't matter. Selecting the plug starts at an octave |
| pitch bend | not used |

### P1 in numbers

| P1 | Size | T60, P2 0 (measured, click) | T60, P2 128 | T60, P2 255 | Pre-delay |
|---:|---:|---:|---:|---:|---:|
| 0 | 0.30× | 0.54 s | 0.55 s | 0.58 s | 3 ms |
| 64 | 0.45× | 1.0 s | 1.0 s | 1.1 s | 8 ms |
| 128 | 0.67× | 3.0 s | 2.9 s | 2.7 s | 16 ms |
| 180 (preset) | 0.93× | 7.5 s | 5.6 s | 5.0 s | 25 ms |
| 200 | 1.06× | 10.8 s | 7.2 s | 5.9 s | 30 ms |
| 240 | 1.36× | 23 s | 11 s | 8.0 s | 40 ms |
| 255 | 1.50× | endless | ~20 s | ~8 s | 45 ms |

Size 1.0× is Dattorro's plate at 44.1 kHz. The second all-pass of each half stays at 0.75× (see *How it works*),
so the smallest room is a little longer than `plate`'s (0.54 s instead of 0.35 s). T60 is the time to fall by 60 dB,
fitted from −5 to −35 dB of the energy decay of a click. **With shimmer the tail is shorter at long sizes**: each lap
moves energy up an octave, and the damping in the loop eats it once it is high enough. That is the cascade thinning
out; the shimmer itself keeps its level lap after lap. At P1 255 the wet level of a noise burst after 2 / 5 / 10 /
20 / 40 s: P2 0 −32 / −35 / −36 / −38 / −40 dBFS (endless), P2 64 −34 / −40 / −48 / −61 / −84, P2 128 −37 / −47 /
−63 / −92 / silent.

### P2 in numbers

| P2 | Mix through the shifter (a) | Power shifted per lap | Shifted path low-pass | What you hear (440 Hz tone, P1 180) |
|---:|---:|---:|---:|---|
| 0 | 0 | 0 | — | a plain reverb |
| 32 | 0.06 | 0.4 % | 7.7 kHz | a hint of air |
| 64 | 0.13 | 2 % | 7.3 kHz | a faint octave halo, 12–20 dB under the tail |
| 96 | 0.21 | 6 % | 7.0 kHz | |
| 128 | 0.30 | 16 % | 6.7 kHz | a clear octave, 12 dB under the tail at first, 4–7 dB under after a second; two octaves up joins after ~1.5 s |
| 140 (preset) | 0.34 | 21 % | 6.6 kHz | |
| 160 | 0.41 | 32 % | 6.4 kHz | |
| 192 | 0.53 | 56 % | 6.2 kHz | the octave as loud as the tail after ~1.5 s, two and three octaves up following |
| 224 | 0.66 | 79 % | 5.9 kHz | |
| 255 | 0.80 | 94 % | 5.7 kHz | **the cascade**: the tone itself fades within ~1.5 s, the octave peaks at 0.75 s, two octaves at 1.75 s, three at 2.75 s, then it thins out |

a = 0.8 · (u + u²) / 2 with u = P2/255, so the low half of the knob is finer. The direct path gets 1 − a, the shifted
path a × √(3/2) (the triangle crossfade loses a third of the power of what it shifts, and this makes it up). The
decay gain after the mix makes up the power the mix itself loses, so the total T60 holds when nothing climbs out
of the top.

### Notes: the interval

| Key | Interval | | Key | Interval |
|---|---:|---|---|---:|
| C2 and below | −12 (an octave down) | | C4 | **+12 (an octave: the default)** |
| F2 | −7 | | D4 | +14 |
| G2 | −5 | | E4 | +16 |
| C3 | **0: shimmer off** (a plain reverb, whatever P2) | | F4 | +17 |
| D3 | +2 | | G4 | +19 (octave + fifth) |
| E3 | +4 | | A4 | +21 |
| F3 | +5 (a fourth) | | B4 | +23 |
| G3 | +7 (a fifth) | | C5 and above | +24 (two octaves) |
| A3, B3 | +9, +11 | | | |

Every key is its distance from C3 in semitones, clamped to −12 … +24. A fifth or a fourth stacks up in the cascade
(G3: a fifth, then a ninth, …); the octave keeps it consonant. Downward intervals cascade into the bass, where a
90 Hz high-pass in the shifter path stops them. Which keys the FX track's 24 keys send at its default octave is a
hardware check (the drum tracks send F3–E5); use the octave buttons to reach C3 and below.

The shifter is a two-tap delay-line shifter: its two read points sweep across a 139 ms window, half a window apart,
and crossfade. On a pure tone that pulls the shifted pitch by up to ±7 Hz (±fs / window), an expected and slightly
chorused detune; on a reverb tail it just smears.

**Sends.** On the OP-Z a track's FX send moves its sound from the dry path into the FX track *by as much as the FX
track's dry level lets it*: FX parameter byte 12 (portamento on instrument tracks) is that dry level. At 0 (what every
earlier preset had) a full send leaves only the effect; the preset now ships **255**, so the sending track keeps its
dry sound at any send, the classic send/return setup [code, app; to confirm on the device,
[host-tables-flags.md](../../research/host-tables-flags.md#the-bits)]. Pan still sets the dry/wet balance inside
the effect (64 = half dry, half wet; 128 = all reverb).

## Preset

| Name | P1 | P2 | pan | Idea |
|---|---|---|---|---|
| shimmer | 180 | 140 | 128 | the classic: a ~6 s hall with a clear octave halo that climbs a little, for pads, keys and sparse melodies |

At level 255 (unity), filter 128 / resonance 0. Effect tracks have no preset keys, so an effect plug only ever uses
this one preset (slot 0, index 0). A draft: tune it by ear on the device.

### Settings to try

| Name | P1 | P2 | Note | Idea |
|---|---|---|---|---|
| plain hall | 180 | 0 | — | the reverb on its own, no shifting: a smooth ~7.5 s hall to compare against |
| halo | 200 | 70 | C4 | a long hall with a faint octave above it: the shimmer you notice only when it's gone |
| cathedral | 230 | 150 | C4 | huge and long, a clear octave climbing to two: big pads, choirs, the mic input |
| fifths | 190 | 130 | G3 | the halo a fifth up, stacking into ninths: open, modal |
| ascend | 210 | 255 | C4 | the rising cascade: every chord flies up in octaves and dissolves in ~5 s |
| sub | 170 | 110 | C2 | an octave down: a dark, growing undertow under the tail |
| room sparkle | 60 | 160 | C5 | a short room whose reflections come back two octaves up: bells and plucks |
| endless | 255 | 40 | C4 | a nearly endless wash with a slowly fading octave on top: stop sending and it hangs |

All at pan 128, level 255, filter 128 / resonance 0. *Note* is a key to play once on the FX track (C4 is the default).

## Facts

| | |
|---|---|
| Plug | 61008, version 1, `trackmask` 0x300 (FX1, FX2); preset byte 12 (the FX **dry level**) 255: the sending track keeps its dry signal, classic send/return ([host-tables-flags.md](../../research/host-tables-flags.md#the-bits)) |
| Memory slot | 44 (10 KB class), **9,168 of 10,240 bytes** used (Blackfin build, `-O2`) |
| Memory request | **249,052 bytes (243 KB)** on every track: fits FX2's 256 KB pool as well as FX1's 2 MB. The 32-bit ring of tank lines is 206 KB (52,795 samples), the shifter ring 32 KB (8,192 samples), the rest work buffers. One size for both tracks: the OS doesn't enforce the trackmask and the FX pools sit back to back, so a buffer that fits FX2 is safe wherever the plug lands. On the master track (16 KB pool, and not a user slot) `init` refuses the buffer and the plug passes the dry signal only |
| State | 108 B of 200 |
| CPU (Blackfin simulator, `tools/opz_cycles.py`) | **~473 cycles per stereo frame** (30,300 per block, worst block 490/frame) at every P1/P2 and interval, 5.2 % of the 580,499-cycle block. **~535 (worst block 552) with P1 and P2 both moving every block** (`knob_cases`). **~567 per frame (worst block 642) while P1 glides end to end**: the tank's delays then read between samples, and in the blocks where the output taps also crossfade to a new size it peaks. Note-on 43–53 cycles (also the velocity-0 re-send). The shifter is ~110 of the 473; `plate` is 420 |
| Start-up | the first 15 blocks (22 ms) after selecting the plug pass the dry sound only, while the 243 KB buffer is cleared (`init` runs with interrupts masked, so it can't clear it at once) |
| Latency | none on the dry path; the reverb starts after its pre-delay (3–45 ms), the shifted part ~70 ms later (half the window) |

### CPU cases

| Case | cycles/frame | worst block |
|---|---:|---:|
| P1 0 / 180 / 255 × P2 0 / 140 / 255 (9 cases) | 472.7–473.5 | 489.7–490.5 |
| max size + max shimmer, wet only, level 255 | 472.8 | 490.0 |
| intervals G3, C5, C2, C6, C3 arriving every 8 blocks, re-sent with velocity 0 and NULL params in between | 473.2 | 490.5 |
| size glide from P1 0 to 255, P2 200 | 567.3 | 641.5 |
| knobs moving every block, period 8 | 534.2 | 550.7 |
| knobs moving every block, period 40 | 535.2 | 551.5 |
| clearing warm-up (first 16 blocks, dry only while clearing) | 107.0 | 476.9 |

### How it works

Input → DC blocker (35 Hz) → pre-delay → band limit (one-pole, 10 kHz) → three input diffusers per channel
(all-passes: Dattorro's 142, 379 and 277 samples at 29.8 kHz scaled to 44.1 kHz, his 107 left out to save CPU;
the right channel slightly longer, so the stereo input stays stereo) → the tank.

The tank is the figure 8 from Dattorro's paper, as in `plate`: each half is a modulated all-pass (−0.7, 12 samples
of excursion at 0.7 Hz, the two halves in quadrature), a long delay, a damping low-pass (one-pole, 6 kHz), a decay
gain, an all-pass (+0.5), a second delay and a decay gain, and feeds the other half. The second all-pass keeps its
length (Dattorro's at 0.75×) while the size moves: the long delays carry the size, and it saves two interpolated
lines while P1 glides. Output: Dattorro's seven taps per channel, at ×0.6.

**The shifter** sits in half 0, after its first long delay, so everything going round the loop passes it once a
lap (~0.3 s per lap at size 0.3, ~0.65 s at 0.93, ~1 s at 1.5). Its branch is high-passed (90 Hz) and low-passed
(two poles, 8 → 5.7 kHz with P2, lower for intervals above an octave, whose reads go faster and would alias) into
an 8,192-sample ring. Two read taps sweep their delays across a 6,144-sample (139 ms) window at a rate set by the
interval (an octave up: one sample per sample), half a window apart, each faded in and out by a triangle that is
zero where its delay jumps. Their crossfade, made up by √(3/2), is mixed with the unshifted signal as
(1 − a) direct + a shifted and goes on into the damping and the rest of the loop. The decay gain after it makes up
the power that mixing two unrelated signals loses (1/√((1 − a)² + a²), up to +3 dB at a = 0.5), capped below 1.

**Bounded at every setting.** The shifted part is at a new pitch every lap, so it can't pile up on one frequency;
what climbs ends in the damping (or, going down, in the 90 Hz high-pass). At unison (C3) the shifter is muted, since
there it would only add a comb. Every lap's gain for what isn't shifted stays below 1, and the arithmetic
saturates. Over that, `plate`'s limiter watches the wet signal at full level every block: above −6 dBFS the input
to the tank ducks (−2.5 dB a block), above −3 dBFS the loop gain trims too, both recovering in ~0.2 s. Normal
settings never touch it.

The shifter runs at every P2, including 0, so its ring is never stale and turning P2 up never starts with a jump.
Size changes glide as in `plate`: the tank delays move at most 1/8 sample per sample and read between samples while
they move; the 14 output taps follow in steps of 6 % of the size, crossfading over 8 blocks.

The delay lines and the shifter ring are 32-bit (16-bit lines leave limit cycles in a long loop, as `plate` found).
Q31 signals and Q15 coefficients, saturating arithmetic, no division, no `int64` and no libgcc in the audio path, no
floating point. The input diffusers multiply with a single 16 × 16 instruction (an error below 2⁻¹⁵, outside the
loop, so it can't build up); everything in the loop uses the full 32 × 16 multiply.

### Offline checks

- Eight golden scenarios (`make -C sim test`): the preset on a click train, a P2 sweep from plain reverb to cascade,
  intervals changing under a held sine (C4, G3, C5, C2, F3, C3, C4) on FX2, a P1 sweep up and down with a jump, the
  worst case (P1 255, P2 255, a loud saw, two octaves), both knobs swept fast while notes change the interval (the
  macro v3 crash situation), pan from dry to wet on FX2, and every knob and interval at its limits on a full-scale
  sweep. None clips (peaks −2.2 dBFS and below).
- [`tests/check.c`](tests/check.c) (host build of the engine, header has the commands):
  - **fuzz**: 20 one-minute runs on FX1 and FX2 with random knob jumps, sweeps and extremes, notes of every kind
    (0–127, 255, out of range, the velocity-0 re-send with params NULL, note-offs), random pan, level and gain, and
    input switching between silence, full-scale noise, saws, sines and clicks. Built with UBSan and run under macOS
    guard malloc (each allocation against an unmapped page, after and before), with a guard after the 200-byte state
    and the buffer filled with 0x5a as the OS's pool would be. No out-of-bounds access, no undefined behaviour, the
    state stays in range, the output never sits at full scale, and every run decays to silence (below −127 dBFS)
    once its input stops. Then the worst case for 100 s: P1 255, P2 255, a full-scale saw (and noise) through all ten
    interval keys: peaks −0.9 to −3.6 dBFS, RMS −13 to −17 dBFS.
  - **measure**: the tables above; a 7 kHz tone's aliases (16.1 and 11.9 kHz, from the second and third laps)
    58–64 dB under it; a 110 Hz saw at −12 dBFS, wet at unity, settles at −21 to −26 dBFS RMS (P1 128–240) and
    −18 to −22 dBFS at P1 255; P1 and P2 sweeping 0–255–0 every 0.4 / 0.6 s on a saw: the largest second
    difference relative to the local RMS is 1.9, about half the dry saw's own (3.5): no clicks.
- Cycle cases in `tools/opz_cycles.py` (table above), with the device build.

## What to listen for on the device

1. **FX1** or **FX2**, preset *shimmer*: send a pad or keys track at about half send. A smooth ~6 s hall with an
   octave halo above it that climbs a little as it fades. No flutter, no metallic ring, wide in stereo. Selecting
   the plug should not click (the first 22 ms are dry while the buffer clears).
2. **P2** from 0 to 255 while a chord rings: at 0 a plain hall; the octave fades in; past ~192 it climbs; at 255 the
   chord flies up in octaves and dissolves within a few seconds. Turning it should never click or zipper, and the
   top of the cascade should stay smooth, not fizzy or whistling.
3. **Keys on the FX track**: press G3 (a fifth), F3 (a fourth), C5 (two octaves), C2 (octave down), C3 (shimmer
   off), then C4 (back to the octave). Each takes over within a lap; the interval stays after you let go. Check
   which keys the FX track sends at its default octave.
4. **P1** from 0 to 240: a small room with sparkle grows into a huge hall. While you turn, the tail bends in pitch a
   little (the glide); no clicks. At 255 with P2 0 the tail hangs forever; with P2 up it still fades as it climbs.
5. **Both knobs at once, fast**, while notes play and the interval changes (and an LFO on P1 or P2): the device must
   keep running (macro v3 crashed in exactly this situation) and nothing should crackle. CPU: ~5.2 % of a block,
   ~6–7 % while P1 moves; check a busy pattern for crackles.
6. **P1 255, P2 255, a loud pad**: the output must not clip and should settle within a second.
7. **Pan** below 128 mixes the dry sound back in.
8. Compare with `plate` on the other FX track: at P2 0 shimmer should sound like a close cousin (a little longer
   and darker, fewer diffusers); with P2 up it's the shimmer pedal sound.

## Credits

The tank topology, delay lengths and output taps are from J. Dattorro, "Effect Design, Part 1: Reverberator and
Other Filters" (J. Audio Eng. Soc. 45(9), 1997), scaled to 44.1 kHz; the equations only, no code. The reverb engine
is `engines/plate`, copied into this engine and changed (fixed tone, three diffusers per channel, fixed second
all-passes, the shifter). The two-tap triangle-crossfaded delay-line shifter is the classic one, as in Mutable
Instruments Clouds' pitch shifter (MIT licence); no code from it. In the spirit of the Eventide H3000 / Strymon
BigSky shimmer patches; no code from either. The shifter-in-the-loop design, power make-up, fixed-point
implementation and limiter use are our own. MIT licence.
