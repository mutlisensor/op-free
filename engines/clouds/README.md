# clouds

Granular texture for **FX1**. Whatever is sent to the track is recorded into a long ring buffer (11.9 s) and played
back as a cloud of short, windowed grains, each starting at a random point in the recent past and panned somewhere in
the stereo field. P1 goes from a few slow, long swells to a dense haze of tiny grains; P2 goes from a tight, grainy
copy of the input to a wide wash that feeds back into itself. Notes on the FX track tune the grains (a pitch shifter
or a melody played with the cloud), and a note held for 0.4 s **freezes** the buffer: the cloud keeps playing the
moment you caught, at the pitch of the keys you press, until you let go.

| Control | Function |
|---|---|
| P1 **size** | one macro from **sparse long grains** to **dense short clouds**. Grain length 600 ms → 15 ms and mean overlap 0.7 → 8 grains, both exponential: **0** = a 600 ms swell about every 0.86 s, with gaps (0.7 overlapping); **32** = 378 ms, ~3 grains a second; **64** = 238 ms, 1.3 overlapping, ~5/s; **96** = 150 ms, 1.75, ~12/s; **128** = 94 ms, 2.4, ~25/s: a smooth cloud; **160** = 59 ms, 3.2, ~54/s; **192** = 37 ms, 4.4, ~117/s; **224** = 24 ms, 6, ~250/s; **255** = 15 ms, 8 overlapping, ~530 grains a second: a buzzing haze. The level per grain follows the overlap (√2 × min(1, 1.6 / √overlap)), so from ~100 up the cloud sits within ~1 dB of the input; below that the gaps make it quieter (−3 dB at 64, −7 dB at 0). New grains take the new setting, the ones playing finish as they were: P1 is smooth to turn and an LFO on it works |
| P2 **texture** | **0** = tight: every grain starts within the last 10 ms (plus what its pitch needs), all in the centre: a grainy, slightly smeared copy of the input. **Reach** (how far back a grain may start) grows exponentially to **5.1 s at 192** and stays (28 ms at 32, 81 ms at 64, 230 ms at 96, 650 ms at 128, 1.8 s at 160): the cloud mixes more and more of the past into the present. **Stereo spread** grows from mono at 0 to full width at 128 (25 % at 32, 50 % at 64, 75 % at 96; equal-power pan per grain). From **128 to 255** the cloud **feeds back** into the buffer (0 → 0.8: 0.2 at 160, 0.4 at 192, 0.6 at 224; the feedback is low-passed at 5 kHz and high-passed at 50 Hz) and every grain is **detuned** at random (0 → ±40 cents: ±10 at 160, ±20 at 192, ±30 at 224): repeats smear into a wash, and with a note above C4 they climb in pitch (shimmer). New grains take the setting; the feedback gain ramps per sample |
| pan | dry/wet, as on `plate`, `filter` and `glue`: **0 = dry only**, rising linearly to **fully wet at 128**; 128–255 stay fully wet. 64 = half dry, half cloud |
| level | output level × the track gain (255 = unity) |
| filter, resonance | not used: they are the OS's own track filter, also on FX tracks. The preset leaves it neutral (128, 0) |
| pitch bend | transposes new grains, ±2 semitones at full scale (the simulator's scale; the device's is not confirmed) |
| **notes on the FX track** | **pitch**: a note sets the pitch of every new grain relative to **C4**: C3 = an octave down, G4 = a fifth up, C5 = an octave up; notes outside C3–C5 fold into that range by octaves (C2 = C3, C6 = C5). Tuned within 0.4 cent. Grains already playing keep their pitch, so a new note blends in over a grain's length. Releasing a key returns to the newest key still held; after the last key is up the pitch **stays** until the next note (play **C4** to go back to the input's pitch; selecting the plug also resets it). Velocity is not used. **Freeze**: when a key has been held for **0.4 s** (longer than 3 steps at 120 BPM), recording stops and the cloud keeps playing what the buffer holds (with P2's reach: at P2 0 the last ~10 ms plus the grains' length, at 192 and up the last 5 s), at the pitch of the newest key: press other keys over the held one to play the frozen cloud as a melody (each new key keeps it frozen). Releasing every key unfreezes it. Freezing and unfreezing take 12 ms and click-free: grains that could not go on in the new state fade out, the recording fades out (or back in), and new grains start only where they can live in both |
| **no notes** | the cloud at the input's pitch (or the last note's) |

**The buffer.** Mono (the input's L + R sum), 16-bit at half scale, 2^19 samples = **11.9 s** on FX1. The stereo
image comes from the grains' pans, not from the input's. Grains read it with linear interpolation at any speed from
0.25× to 2.3×. A grain pitched up starts far enough back that it never catches up with the record head before it ends;
one pitched down never lets the record head overtake it; while frozen, no grain reads across the seam where the
newest and the oldest audio meet. The record path is input + feedback → soft saturator (unity when quiet, −1.4 dB at
full scale, a ceiling at 0 dBFS) → buffer.

**Window and grains.** Hann window (a 129-point table), evaluated at block boundaries and ramped linearly in between
together with the grain's pan. 16 grains at most (the densest setting averages 8, peaks at 12), sample-accurate
onsets with random intervals of 0.5–1.5× the mean.

**Limiter.** The cloud is played one block (1.45 ms) late so that a limiter can look one block ahead: gain = knee /
peak (knee −1 dBFS), ramped linearly between block ends and released over ~90 ms. Dense coherent clouds and runaway
feedback stay at or below −1 dBFS without a gain step or a clipped sample.

## Preset

| Name | P1 | P2 | pan | Idea |
|---|---|---|---|---|
| **cloud** | 140 | 110 | 128 | 79 ms grains, 2.7 overlapping, from the last 0.36 s, wide stereo, no feedback: a soft, wide cloud just behind the sound |

At level 255 (unity), filter 128 / resonance 0. Effect tracks have no preset keys: the OP-Z applies this one preset
(slot 0, index 0) when clouds is chosen ([presets.md](../../research/presets.md)). A draft: tune it by ear on the
device.

### Settings to try

| Name | P1 | P2 | pan | Idea |
|---|---|---|---|---|
| haze | 255 | 30 | 128 | 15 ms grains, 8 deep, within 25 ms of the input, nearly mono: a buzzing, blurred double of the sound |
| swells | 0 | 150 | 128 | 600 ms grains from the last 1.3 s, one at a time, anywhere in the stereo field: slow swells with gaps (−7 dB: raise the level) |
| harmonizer | 200 | 0 | 64 | tap a note on FX1 (G4 = a fifth, E4 = a third, C5 = an octave; under 3 steps so it doesn't freeze): a grainy pitch shifter under the dry sound |
| scatter | 190 | 192 | 128 | 38 ms grains picked from the last 5 s, feedback 0.4, ±20 cents: a smeared, shifting texture of everything that played |
| shimmer | 160 | 235 | 110 | tap C5 on FX1: every pass through the feedback climbs an octave, a rising, glassy wash held at −1 dBFS by the limiter |
| freeze pad | 120 | 128 | 128 | hold a key on FX1 over a chord: after 0.4 s the cloud freezes into a pad; press other keys over it to play it as a melody, release all to move on |
| stutter | 40 | 10 | 128 | 340 ms grains within 14 ms of the input, ~3 a second: a pulsing, wobbly double of the sound |

## Facts

| | |
|---|---|
| Plug | 61007, version 1, `trackmask` 0x100 (**FX1 only**); preset byte 12 (the FX **dry level**) 255: the sending track keeps its dry signal, classic send/return ([host-tables-flags.md](../../research/host-tables-flags.md#the-bits)) |
| Memory slot | 45 (10 KB class), 5,842 of 10,240 bytes used (Blackfin build, `-O2`); no `.bss` |
| Memory request | **1,050,752 B** (1.0 MB) of FX1's 2 MB pool: the 2^19-sample ring (+192 mirrored samples after its end so a block's reads never wrap), 16 grains, two grain-sum blocks and the feedback block (1,792 B). Like `echo`, half of FX1's pool: long enough for a 5 s reach plus 600 ms grains at 2.3× and a freeze that holds ~12 s of material |
| FX2 / other tracks | the OS does not enforce `trackmask` (`opz.py` warns, it doesn't refuse), and the FX pools sit back to back (FX2's 256 KB right before tape's pool). On any track but FX1 (`init`'s `track` ≠ 8) clouds uses only **133,248 B**: a 2^16 ring (1.49 s), so P2's reach stops at ~1.4 s. On master (16 KB) it passes the dry signal only. Making FX2 official is a `trackmask` change to 0x300 after a hardware check |
| Clearing | `init` runs with interrupts masked, so it only sets up the state; `renderFX` zeroes the grain table in its first block and the ring 32 KB a block after that (34 blocks, **49 ms** on FX1, 9 ms elsewhere), passing the dry signal meanwhile; then the recording fades in over 12 ms |
| State | 80 B of 200 (Blackfin) |
| CPU (Blackfin simulator, `tools/opz_cycles.py`) | below. **~100 cycles per stereo sample at the preset**, 337 at the densest setting with detune (P1 255, P2 255), 393 in its worst block, 405 in a freeze hand-over: 1.7–7 % of the 580,499-cycle block. Note-on 138 cycles |
| Latency | none for the dry signal; the cloud starts at least 4.4 ms (192 samples) plus one block behind the input |
| Library code | none: no libgcc, no 64-bit maths, no division (reciprocals by table and Newton steps), no floating point |

CPU per stereo sample (mean / worst block), one FX track, the cycle driver's 345 Hz saw input:

| Case | Mean | Worst |
|---|---:|---:|
| P1 0 / 128 / 200 / 255, P2 0 or 128 (whole-sample reads) | 87 / 98 / 137 / 202 | 149 / 149 / 166 / 238 |
| P1 0 / 128 / 200 / 255, P2 255 (interpolated reads, feedback) | 144 / 162 / 229 / 337 | 161 / 172 / 275 / 393 |
| densest + note C5 or C3 | 337 | 393 |
| frozen (C5 held) / thaw | 277 / 313 | 405 / 405 |
| held note re-sent (velocity 0, params NULL) | 337 | 392 |
| clearing warm-up (first 40 blocks) | 151 | 240 |
| knobs moving every block, period 8 / 40 (C5 held) | 227 / 168 | 297 / 221 |
| knobs moving every block, period 8, frozen | 214 | 307 |

How it works per block: the notes decide whether the buffer should be frozen (a hand-over of 8 blocks when that
changes); P1 and P2 give the grain length, window step, level per grain, mean onset interval, reach, spread, feedback
and detune. The block's input is recorded (unless frozen); grains due in this block start (a slot, a random delay
within the reach and the pitch's limits, a random pan and detune); every active grain adds its block into the grain
sum, the window and pan ramped across the block (unity-rate grains on a whole sample walk the buffer, the others
interpolate: 11 and 22 instructions per grain-sample); the limiter looks at this block's peak, and the last block's
sum goes out with the dry signal.

Offline checks:

- `engines/clouds/tests/fuzz.c` (build line in the file) includes `clouds.c` with test hooks and, run with 200000, plays 4 × 200,000
  blocks on FX1 and 4 × 100,000 on FX2 (~720 million grain reads): knobs jumping every block, extremes and sweeps,
  notes and chords, held notes re-sent with velocity 0 and params NULL, note 255, out-of-range notes, bends, gain 0
  and 1, full-scale, silent and square inputs. The buffer is mapped between inaccessible pages at exactly the size the
  engine says it uses (FX2: 130 KB), the state has a guard, UBSan is on, and every audible grain read is checked
  against a shadow of when each sample was recorded: no seam, no overwritten audio, no jump. 0 errors.
- Clicks, 330 Hz sine through the engine (largest second difference over the local peak; steady state 0.003): P1
  slammed 0 ↔ 255 every 0.29 s 0.006, P2 slammed 0.006, P1 and P2 a new value every block 0.005, notes, freeze,
  re-send and thaw 0.007 (pitched grains raise the baseline), pan sweeps 0.003.
- Freeze, a sine sweeping 200 Hz → 1.6 kHz: the cloud holds ~430 Hz from 0.4 s after the key while the input sweeps on,
  plays a fifth up under a second key, and follows the sweep again after the release (golden `freeze`).
- Level, a mono 220 Hz sine at −12 dBFS, fully wet: −0.0 to −0.8 dB from P1 128 up at every P2; −3 dB at P1 64,
  −7 dB at P1 0. Peaks at P2 255 (feedback 0.8): −1.0 dBFS.
- The device build's output equals the host build's bit for bit in all 21 cycle cases (`opz_cycles.py --dump`
  against the same cases run on the host).

## What to listen for on the device

1. Install on **FX1** and choose it (preset *cloud*). Raise a synth or drum track's FX1 send: a soft, wide cloud just
   behind the sound. The first ~50 ms after choosing the plug are dry only (the buffer is being cleared).
2. Turn **P1** slowly from 0 to 255: single slow swells with gaps, then a smooth cloud, then a dense buzzing haze. No
   clicks anywhere, also when turned fast.
3. Turn **P2**: from a tight, centred double, through a wide cloud of the last second, to a smeared wash of the last
   few seconds; past ~160 the repeats feed back. Check that P2 255 with loud input never clips (the limiter holds it
   at −1 dBFS).
4. **Notes**: tap short notes on FX1 (setting *harmonizer*): the cloud moves to each note's interval relative to C4 and
   stays there after the note; tap C4 to return. Check the tuning against the same note on a synth track.
5. **Freeze**: hold a key on FX1 for half a second: the cloud freezes; change the sound on the other tracks, the cloud
   doesn't follow. Press other keys over the held one: the frozen cloud plays at their pitch. Release everything: it
   moves on with the music. Listen for clicks at the freeze and the release (there should be none).
6. Sequence the FX1 track with short notes and one long note per bar: a melody in the cloud with a freeze at the end
   of each bar. Check that a note re-sent by the OS (release one of two held keys) changes the pitch back to the
   other key without a click.
7. Pan the FX1 track to 0: only the dry sound; 64: half and half; 128 and up: the cloud only.
8. If FX2 is to be tried (it is outside the `trackmask`, see Facts): put clouds there and check that the longest reach
   is ~1.4 s and that the tape track's audio is untouched.

## Credits

Granular processing after the ideas of Mutable Instruments Clouds and Beads (MIT licence: the design of a freezable
record buffer read by windowed, panned, pitch-shifted grains with random positions and a density control). No code
from either: the buffer handling, grain scheduling and safety rules, the window and limiter are our own. Hann window
after J. von Hann; overlap-normalised grain level after the usual granular-synthesis practice (C. Roads,
*Microsound*, 2001). MIT licence.
