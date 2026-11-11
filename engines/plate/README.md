# plate

Lush stereo plate reverb for **FX1 and FX2**. It's a Dattorro figure-8 tank with modulated all-passes, so long tails
shimmer and chorus gently instead of ringing metallically. One knob takes it from a small bright room to a huge plate
that rings on forever. The other sets the tone, from dark and warm to bright and shimmering. Hold a note on its track
to **freeze** the tail.

| Control | Function |
|---|---|
| P1 **size** | one macro over **size, decay and pre-delay** (table below). 0 = a small, short room (T60 0.3 s, pre-delay 3 ms). Turning it up makes the room bigger (delays ×5 from 0 to 255) and the decay longer (×2 every ~37 steps), up to a huge plate at 240 (T60 ~20 s, pre-delay 40 ms). **240–255: the decay runs on towards endless**; at 255 the tail never dies. A limiter stops it from building up: however much goes in, the wet signal settles with its peaks around −1 to −3 dBFS. Size changes **glide** (~1.5 s from end to end, at most 1/8 sample per sample), so turning P1 bends the tail's pitch a little, like tape, instead of clicking. An LFO on P1 gives a slow, warping tail |
| P2 **tone** | **dark and warm → bright and shimmering**, in one sweep: the input's band limit opens from 2 to 18 kHz, the damping inside the tank from 1.4 to 14 kHz (how fast highs die compared to lows), and the chorus in the tail gets deeper and a little faster (all-pass excursion 4 → 32 samples, LFO 0.5 → 1 Hz). Table below. Continuous, so an LFO on P2 glides through it |
| pan | not reachable on the OP-Z: an FX track has no pan dial, app control or mixer column [proven, H-FX1], and presets can't set it, so it stays where the OS leaves it (127 on the unit checked), which this effect treats as **the reverb only** (the dry about 44 dB down). The law below the centre, for anything that does set pan: the dry blends in at equal power: 64 = dry and the reverb at −3 dB each, 0 = dry only. |
| dry level (hold the FX track's key + **red dial**) | the OS's own dry level (byte 12; TE's app shows it as "dry level" on the FX page): how much of each sending track's own sound stays in the mix. 255 = send/return (the source plays on and the effect adds to it), 0 = insert (a track sent at 100 % is heard only through the effect). This is the dry/wet control on the OP-Z [proven, H-FX1 2026-11-11] |
| level | output level (255 = unity) (nothing on the OP-Z reaches an FX track's level, neither a dial nor the mixer page [proven, H-FX1], so the preset's value holds) |
| filter, resonance | not used: they are the OS's own track filter, also on FX tracks. Presets leave it neutral (128, 0). The track filter after the reverb is handy for thinning the tail (a little high-pass) |
| **notes on its track** | **freeze**: while any note is held, the input is shut out of the tank (faded over ~12 ms) and the tank's decay and damping open up. What was ringing hangs as a pad; it fades only 0.2–0.5 dB a second, losing its top end first. New sound sent to the track doesn't get in, but the dry part (pan below 128) still passes. When the last note is released, the tail decays at P1's rate again and new sound enters. Note number and velocity don't matter. Sequence a long note at the end of a phrase to hold its tail, or tap a key live |
| **no notes** | a plain reverb |
| pitch bend | not used |

### P1 in numbers

| P1 | Size | T60 (by design) | Measured (1 kHz, P2 128) | Pre-delay |
|---:|---:|---:|---:|---:|
| 0 | 0.30× | 0.3 s | 0.35 s | 3 ms |
| 64 | 0.45× | 1.0 s | 1.0 s | 8 ms |
| 128 | 0.67× | 3.2 s | 3.2 s | 16 ms |
| 140 (preset) | 0.73× | 4.0 s | 3.9 s (P2 160) | 18 ms |
| 180 | 0.93× | 8.4 s | 7.6 s | 25 ms |
| 200 | 1.06× | 12 s | 10.7 s | 30 ms |
| 240 | 1.36× | 25 s | ~20 s | 40 ms |
| 255 | 1.50× | endless | — | 45 ms |

Size 1.0× is Dattorro's plate at 44.1 kHz (a figure-8 loop of ~0.7 s). T60 is the time to fall by 60 dB; highs
fall faster, as set by P2 (at P1 128: 2.3 s at 4 kHz with P2 128; at P1 140: 1.9 s with P2 0, 3.6 s with P2 255).

### P2 in numbers

| P2 | Band limit | Damping | Chorus |
|---:|---:|---:|---|
| 0 | 2.0 kHz | 1.4 kHz | 4 samples, 0.50 Hz |
| 64 | 3.5 kHz | 2.5 kHz | 11 samples, 0.63 Hz |
| 128 | 6.1 kHz | 4.4 kHz | 18 samples, 0.75 Hz |
| 160 (preset) | 8.0 kHz | 5.9 kHz | 22 samples, 0.81 Hz |
| 200 | 11.4 kHz | 8.4 kHz | 26 samples, 0.89 Hz |
| 255 | 18.4 kHz | 13.8 kHz | 32 samples, 1.0 Hz |

Darker settings sound quieter on bright material (the damping removes energy): on a white click the tail at P2 0 is
~8 dB below P2 255. On a saw at 110 Hz the difference is ~1 dB.

**Sends.** On the OP-Z a track's FX send moves its sound from the dry path into the FX track *by as much as the FX
track's dry level lets it*: FX parameter byte 12 (portamento on instrument tracks) is that dry level. At 0 (what every
earlier preset had) a full send leaves only the effect; the preset now ships **255**, so the sending track keeps its
dry sound at any send, the classic send/return setup [code, app; to confirm on the device,
[host-tables-flags.md](../../research/host-tables-flags.md#the-bits)]. Pan still sets the dry/wet balance inside
the effect (64 = dry and reverb at −3 dB each; 128 = all reverb).

## Preset

| Name | P1 | P2 | pan | Idea |
|---|---|---|---|---|
| plate | 140 | 160 | 128 | a classic bright plate: ~4 s, 18 ms pre-delay, soft chorus in the tail |

At level 255 (unity), filter 128 / resonance 0. Effect tracks have no preset keys, so an effect plug only ever uses
this one preset (slot 0, index 0). A draft: tune it by ear on the device.

### Settings to try

| Name | P1 | P2 | pan | Idea |
|---|---|---|---|---|
| room | 40 | 200 | 128 | small, bright room: 0.6 s, 6 ms pre-delay. Glues drums together without washing them out |
| drum plate | 90 | 140 | 128 | short plate, ~1.4 s: snare and claps get the classic bloom |
| dark hall | 190 | 40 | 128 | big and long (~10 s) but warm: highs die in a few seconds. Pads and keys |
| shimmer | 210 | 255 | 128 | huge, bright, deep chorus in a ~15 s tail. Sparse melodies, vocals through the mic input |
| endless | 255 | 120 | 128 | the tail never dies: feed it a chord, stop sending, and it drones on (the limiter holds the level) |
| freeze pad | 180 | 180 | 128 | sequence a long note on the FX track over the last bar: that bar's tail hangs, then lets go |
| half wet | 128 | 128 | 64 | dry and reverb at −3 dB each, for a full send with FX dry level 0 |

All at level 255, filter 128 / resonance 0.

## Facts

| | |
|---|---|
| Plug | 61006, version 2, `trackmask` 0x300 (FX1, FX2); preset byte 12 (the FX **dry level**) 255: the sending track keeps its dry signal, classic send/return ([host-tables-flags.md](../../research/host-tables-flags.md#the-bits)) |
| Memory slot | 47 (10 KB class), 8.0 KB of 10 KB used (Blackfin build, `-O2`) |
| Memory request | **229,592 bytes (224 KB)**: fits FX2's 256 KB pool and FX1's 2 MB. The ring of 32-bit delay lines is 219 KB (56,186 samples), the rest is work buffers. Not the master track (16 KB, and not an effect slot anyway) |
| State | 104 B of 200 |
| CPU (Blackfin simulator, `tools/opz_cycles.py`) | **v2 (2026-11-11): 578 cycles per stereo frame in its worst case, worst block 620** (fx-usability.md §4a). Earlier figures: **~420 cycles per stereo frame** (26,900 per block, worst block 438/frame) at every P1/P2 and while frozen, 4.6 % of the 580,499-cycle block. **~580 per frame (worst 620) while P1 moves** (a knob turn or the LFO on P1): the tank lines then interpolate as they glide. Note-on 41 cycles. For comparison, `swirl` is 414 |
| Start-up | the first ~14 blocks (20 ms) after selecting the plug pass the dry sound only, while the 224 KB buffer is cleared (`init` runs with interrupts masked, so it can't clear it at once) |
| Latency | none on the dry path; the reverb starts after its pre-delay (3–45 ms) |

### How it works

Input → DC blocker (35 Hz) → pre-delay → band limit (one-pole, P2) → four input diffusers per channel (all-passes:
Dattorro's 142, 107, 379 and 277 samples at 29.8 kHz, scaled to 44.1 kHz; the right channel slightly longer, so the
stereo input stays stereo) → the tank.

The tank is the figure 8 from Dattorro's paper. Each half is a modulated all-pass (−0.7), a long delay, a damping
low-pass (P2), a decay gain, an all-pass (+0.5), a second delay and a decay gain, and feeds the other half. The left
input enters the half whose taps make up most of the left output, so the stereo image holds. The two modulated
all-passes swing in quadrature, which smears the tank's modes into a slow chorus: no metallic ringing, no fixed
flutter. The decay gains are computed per block from the T60 and each segment's length, so the decay time holds at
any size. Output: Dattorro's seven taps per channel, at ×0.6.

Size changes glide: every tank delay moves towards its new length at most 1/8 sample per sample, all of them in
proportion, and reads between samples while it moves. The 14 output taps follow in steps: when the tank has moved
by 3 % (or settled somewhere new), they jump to their new positions and the output crossfades from the old set to
the new one over 8 blocks. A tap left behind a shrinking line still reads that line's own past, so nothing breaks
in between. Every line is at least a block long, so the network runs a block at a time, line after line, on
contiguous stretches of one shared ring: no index wrapping per sample.

The delay lines are 32-bit. 16-bit lines (half the memory) were tried: rounding to 16 bits in the loops left limit
cycles (a faint 3.4 kHz tone at −82 dBFS that never decayed), and truncating instead cut off the last 20 dB of every
tail.

Limiter: the wet signal at full level is measured every block. Above −6 dBFS the input to the tank ducks
(−2.5 dB a block); above −3 dBFS the loop gain trims too. Both recover in ~0.2 s. Normal settings never touch it; it
holds the endless top of P1, or a loud sustained input at a long setting, below 0 dBFS.

Q31 signals and Q15 coefficients, saturating arithmetic, no division, no `int64` and no libgcc in the audio path,
no floating point.

### Offline checks

- Seven golden scenarios (`make -C sim test`): the preset on a click train, a P1 sweep up and down with a jump (glide,
  no clicks), a P2 sweep, freeze over several hits, P1 255 with a loud saw (limiter), pan from dry to wet on FX1, and
  every knob at its limits with a freeze note on a full-scale sweep. None clips.
- Impulse responses across P1/P2: decay times as in the table. The echo density (Abel & Huang's normalised measure)
  reaches 0.75–0.9 by 100 ms after the impulse and ~1 (fully dense) by 200–400 ms, later at the largest sizes.
  Left/right correlation of the tail is between −0.08 and 0.06 (decorrelated). In the late tail's spectrum no bin
  stands more than 3–7 dB above its 1/3-octave average at sizes from 0.45× up: no narrow ringing modes. The smallest
  size (P1 0) is more coloured, as small rooms are.
- The decay is a clean exponential down past −80 dB, with no limit cycles and no DC floor.
- Steady state, wet only, level 255: a −12 dBFS saw at 110 Hz gives a tail at −19 dBFS RMS (2.5 dB under the dry
  saw) for P1 128–240. At P1 255 a full-scale saw settles at −14 dBFS RMS, peaks −2 dBFS.
- Freeze, held 7 s on noise: the level falls 0.2–0.5 dB per second (more on bright settings), then the tail decays
  normally after the release. Hits during the freeze don't get in.
- No clicks while P1 sweeps up and down and jumps on a saw: the largest second difference of the output, relative to
  its local RMS, stays below the dry saw's own, except at the reverb's onset.

## What to listen for on the device

1. **FX1** or **FX2**, preset *plate*: send a snare or clap track at about half send. You should hear a smooth,
   bright plate of about 4 s with no flutter or metallic ring, wide in stereo. Nothing should click when the plug is
   selected (the first 20 ms are dry while the buffer clears).
2. **P1** from 0 to 240: a small, tight room grows into a huge plate, and the tail gets longer and the pre-delay a
   little later. While you turn, the tail bends in pitch briefly (the glide); no clicks or zipper. Past 240 the tail
   stops dying; at 255 a chord held for a second should drone on, at a steady level, until P1 comes down.
3. **P2** from 0 to 255: dark and warm, highs gone in a second, to bright and airy with a slow chorus in the tail.
   Bright settings on a long tail should shimmer, not ring.
4. **Freeze**: play a chord into it, then hold a key on the FX track. The tail should hang, and new hits on the
   sending track shouldn't get in. Let go and it decays. Try a sequenced long note on the FX track at the end of a
   bar.
5. **Stereo**: pan a source hard left on its own track; its first reflections should come from the left before the
   tail spreads out.
6. **Pan** below 128 mixes the dry sound back in (at full send the reverb return is all you hear at 128).
7. Compare with TE's `reverb` and `fdnreverb` on the same send: plate should sound denser and smoother in the tail,
   with a chorus on long settings, and wider. TE's reverb takes its pre-delay from the keys on the FX track; plate
   uses the keys to freeze.
8. Turn P1 to 255 with a loud pad sent: the output must not clip, and the level should settle within a second.
   CPU: with P1 moving all the time (LFO on P1) plate costs ~6.5 % of a block instead of 4.6 %; check for crackles
   in a busy pattern.

## Version 2 (2026-11-11): what changed, what to listen for

Testers found our effects hard to use: the mix knob seemed not to work, some effects seemed not to pass the sound, and
loud material clipped. Measured in the simulator (fx-usability.md). plate's
centre was already the reverb alone; its lower half was a linear crossfade (64 = −6 dB each, a dip in loudness) and
dry + reverb could sum past full scale. Now the lower half is equal power (the SDK's law) and the sum passes the SDK's
output limiter. The reverb itself is unchanged. In opzsim (no control on the OP-Z reaches an FX track's pan): pan from 0 to 128 on a full send with the dry level
0: the sound moves from dry to reverb without getting quieter in the middle.

## Credits

The tank topology, delay lengths and output taps are from J. Dattorro, "Effect Design, Part 1: Reverberator and
Other Filters" (J. Audio Eng. Soc. 45(9), 1997), scaled to 44.1 kHz. The equations only, no code. In the spirit of
the OP-1 field's *Mother* and the Elements reverb; no code from either. The block-wise processing, size glide with
stepped tap crossfades, freeze, limiter and fixed-point design are our own. MIT licence.
