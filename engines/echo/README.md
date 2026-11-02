# echo

Tape echo for **FX1**, in the spirit of an Echoplex or Space Echo. Repeats bounce left and right and get darker,
softer and more wobbly with every pass. Turned up all the way, the tape runs away into self-oscillation, held by a
tape saturator. Moving the time changes the tape speed, so the echoes bend in pitch instead of clicking. Notes on the
FX track tune the tape loop to a pitch: the echo becomes a plucked string (Karplus-Strong) that rings at the note.

| Control | Function |
|---|---|
| P1 **time** | echo time, **19.8 ms to 5 s**, exponential at 32 steps per octave (each step 2.2 %), and exact at 120 BPM: **85 = 125 ms (1/16), 117 = 250 ms (1/8), 136 = 377 ms (dotted 1/8, 0.6 % long), 149 = 500 ms (1/4), 181 = 1 s (1/2), 213 = 2 s (one bar)**. Other values: 0 = 19.8 ms, 32 = 40 ms, 64 = 79 ms, 128 = 317 ms, 192 = 1.27 s, 255 = 4.97 s. Turning it changes the **tape speed**: within an octave the delay glides like a tape motor (critically damped, ~0.3 s for one step, ~0.8 s to double the time, ~0.4 s to halve it) and the repeats bend in pitch on the way, the right-hand repeats by at most −1.3 / +1.3 octaves, the left-hand ones by at most −0.5 / +0.8. A jump of more than an octave (a fast turn) crossfades to the new time in 6 ms instead. No click either way. Ignored while a note is held |
| P2 **feedback** | one macro for how long the echo lasts and how worn the tape sounds. **Feedback** rises 0 → 0.9 from P2 0 to 192 (64 = 0.3, 128 = 0.6, 160 = 0.75), then 0.9 → **1.4** from 192 to 255: it passes 1 at 204, and from about **210** the echo **self-oscillates** (louder and faster the higher you go, held at about −6 dBFS by the saturator). At the same time each pass through the tape gets **darker** (low-pass, 12 dB/octave, 9 kHz at 0 → 4.7 kHz at 128 → 3.4 kHz at 192 → 2.5 kHz at 255), **thinner** (high-pass 40 Hz → 66 Hz → 110 Hz) and more **wobbly** (wow and flutter at 15 % of full depth at 0, 36 % at 128, 63 % at 192, 100 % at 255). From ~115 a faint tape hiss goes onto the tape (−78 dBFS at 255). Continuous, so an LFO on P2 works |
| pan | dry/wet, TE's balance law: wet = min(2p, 1), dry = min(2(1 − p), 1). **0 = dry only, 64 = dry + echoes at −6 dB, 128 = dry and echoes both at full level, 192 = echoes + dry at −6 dB, 255 = echoes only** |
| level | output level × the track gain (255 = unity) |
| filter, resonance | not used: they are the OS's own track filter, also on FX tracks. The preset leaves it neutral (128, 0) |
| pitch bend | bends the tape speed, ±2 semitones at full scale (the simulator's scale; the device's is not confirmed). Bending up shortens the echo time (it glides, so the echoes bend in pitch), and transposes a ringing note |
| **notes on the FX track** | the tape loop becomes **one period of the note**: the echo rings at the note's pitch (Karplus-Strong), tuned within ~2 cents from C1 to C5 and within ~6 cents above (simulator). Each note with a velocity also **plucks** the loop: a burst of tape noise one period long (at least 64 samples, at most 23 ms), at a level of (velocity / 127)² × −12 dBFS, so velocity 127 plucks hard, 64 a quarter as loud, 1 not at all. **P2 sets how long a note rings**: about 0.1 s at P2 0, 0.4 s at 128, 0.7 s at 160, 1.2 s at 192 (the fundamental lasts longer, the harmonics die faster at dark settings), and from ~204 the note **drones** on (self-oscillation at the note). Whatever is sent to the track is also tuned by the loop (a resonator). The newest note plays (up to 8 held notes remembered; releasing it returns to the previous held note). A new note within an octave of the playing one **glides** to it (~12 ms); a bigger jump, and the switch from the echo to a note, crossfade in 6 ms (the pluck waits for that crossfade). Releasing every note crossfades back to the P1 echo, and the last ring comes back as echoes at P1's time. Notes fold into C1–C7 by octaves. A note re-sent with velocity 0 (the OS handing back a held note) changes the pitch without a pluck |
| **no notes** | the P1 echo |

**Stereo.** The tape is mono, as on the real machines: it records the sum of the input's left and right channels.
Two playback heads read it. In echo mode the **left** head sits at the echo time and the **right** head, which also
feeds the record head, at twice it. So the repeats **ping-pong**: left at 1×, right at 2×, left at 3×, right at
4× the time, and each left/right pair is one feedback step quieter than the pair before (at P2 128, −4.4 dB per pair
plus the tape's darkening). In note mode the right head is one period behind the record head (the loop) and the left
head two periods: the same tone in both ears, with a little width from the wow. The dry signal stays stereo.

**The tape.** The record path is input + feedback → high-pass → soft saturator → two one-pole low-passes → 16-bit
tape. The saturator is y = z − 16/27 z³: unity for quiet signals, −0.3 dB at −12 dBFS, −1.4 dB at −6 dBFS, and flat
(a ceiling of −6 dBFS) from −2.5 dBFS of record level up. Repeats and loud inputs are compressed and rounded, and the
echo can never get louder than −6 dBFS. Wow (0.5 Hz and 0.83 Hz) and flutter (6.7 Hz) vary the tape speed, by up to
0.35 %, 0.2 % and 0.15 % at P2 255. A head D samples behind the record head moves by the speed variation integrated
over those D samples. A long echo therefore bends in pitch by up to twice the speed variation (up to about ±24 cents
at P2 255 and ±9 at 128 when the three line up), and a short loop (a note) is detuned by the speed variation itself.

**Tempo.** echo does not follow the sequencer's tempo: the OS offers a value that is very likely the tempo
(`sequencerGetValue` key 4, Q-18), but its units are not confirmed. P1's steps are therefore exact at 120 BPM (the
OP-Z's default). For another tempo, set P1 = 85 + 32 × log2(wanted time / 125 ms), rounded. Examples for a dotted
1/8: 100 BPM → 450 ms → P1 144; 90 BPM → 500 ms → P1 149.

## Preset

| Name | P1 | P2 | pan | Idea |
|---|---|---|---|---|
| **tape** | 136 | 110 | 128 | dotted 1/8 at 120 BPM (377 ms), feedback 0.52: about five audible pairs of ping-pong repeats, each darker |

At level 255 (unity), filter 128 / resonance 0. Effect tracks have no preset keys: the OP-Z applies this one preset
(slot 0, index 0) when echo is chosen ([presets.md](../../research/presets.md)). A draft: tune it by ear on the
device.

### Settings to try

| Name | P1 | P2 | pan | Idea |
|---|---|---|---|---|
| slapback | 40 | 20 | 128 | one short 47 ms slap (feedback 0.09): rockabilly vocals, snappy drums |
| eighths | 117 | 140 | 128 | 1/8 at 120 BPM, feedback 0.66: a rhythmic bed of repeats |
| dub | 149 | 195 | 150 | 1/4, feedback 0.93, dark and wobbly: long tails; nudge P2 past 205 for a runaway and back |
| siren | 100 | 235 | 200 | self-oscillating loop (feedback 1.24); sweep P1 slowly for pitch dives. **Loud: start with the level down** |
| space | 181 | 160 | 100 | 1 s, feedback 0.75: long ambient repeats tucked under the dry signal |
| pluck | 128 | 170 | 160 | sequence notes on FX1 (velocity 100–127): plucked, tape-dark strings ringing ~0.8 s; the last one echoes at 317 ms after the note ends |
| drone | 128 | 215 | 180 | hold notes on FX1 and send a pad or noise: the loop rings on at the note's pitch, a tape-saturated drone |

## Facts

| | |
|---|---|
| Plug | 61005, version 1, `trackmask` 0x100 (**FX1 only**); preset byte 12 (the FX **dry level**) 255: the sending track keeps its dry signal, classic send/return ([host-tables-flags.md](../../research/host-tables-flags.md#the-bits)) |
| Memory slot | 48 (10 KB class), 8,220 of 10,240 bytes used (Blackfin build, `-O2`) |
| Memory request | **1,048,576 B (1 MB)** of FX1's 2 MB pool: 2^19 16-bit samples, 11.9 s of mono tape. The longest loop is 10 s (the right head at twice the 5 s echo time), plus 1,024 samples of margin for the wow and the read ramps. Not cleared: `init` writes 128 bytes of silence, and a head that would read tape not yet written since `init` reads that silence instead. The record level fades in over the first 23 ms after the plug is selected |
| FX2 | the OS does not enforce `trackmask` (`opz.py` warns, it doesn't refuse), and the FX pools sit back to back (FX2's 256 KB right before tape's pool). So on any track but FX1 (`init`'s `track` ≠ 8) echo uses only **256 KB** (2.97 s of tape): times up to **1.47 s**, P1 above ~199 stays at 1.47 s. Making FX2 official is a `trackmask` change to 0x300 after a hardware check: does `init` get track 9 on FX2, and does the OS accept a 1 MB request there |
| State | 188 B of 200 (Blackfin, 32-bit pointers) |
| CPU (Blackfin simulator, `tools/opz_cycles.py`) | **~186 cycles per stereo sample** (11,900 per block) at every P1/P2, 196 in the worst block; ~219 in a block that crossfades (four heads). Note-on 108 cycles; the note's tuning and gain (two `atan`, a few 32-bit divisions) are worked out once per note, bend or P2 change. 2.1 % of the 580,499-cycle block |
| Latency | none for the dry signal; the first echo arrives at P1's time |
| Library code | libgcc's 32-bit `__udivsi3` (note setup only); no 64-bit maths, no floating point |

How it works per block: P1 (and the bend) give the target loop length; notes give one period of the note minus the
phase delay of the loop's filters at that pitch (exact `atan` of the one-pole responses, so the loop is in tune
whatever P2 does). The head setting glides to the target like a tape motor: the speed it wants is a one-pole of the
distance (93 ms for the echo, 12 ms for notes) within the speed limits, and the speed itself follows over 23 ms
(echo) or 3 ms (notes), so the pitch bends smoothly. A change of mode or a jump of more than an octave starts a 6 ms
smoothstep crossfade to a second head setting instead. The wow offset of each head is worked out at the block end and
every head's read position ramps linearly across the block. Per sample: two (four while crossfading) linearly
interpolated tape reads, the record path, and the dry/wet mix. Feedback, output gains and the record level ramp per
sample, so knob moves don't step.

Offline checks (a harness linking `echo.c` directly, plus the six golden scenarios):

- Echo times from an impulse, P2 0: left 19.82 / 124.97 / 249.91 / 377.14 / 499.82 / 999.66 / 1999.91 / 4966.58 ms
  for P1 0 / 85 / 117 / 136 / 149 / 181 / 213 / 255 (each within a sample or two of the formula, the rest is the
  tape filters' delay), right at twice that. On track 9 (FX2) P1 255 gives 1474 ms.
- Note tuning, plucked, no input: −2 to +2 cents from C1 to C5 at P2 160–255 (autocorrelation); noise-fed resonance
  peaks within ±6 cents from C5 to C7 at P2 200. The loop's own model (filters plus delay) is within 1 cent
  everywhere.
- Self-oscillation, P2 255: from a 50 ms noise burst the loop reaches its −6 dBFS ceiling in ~8 s at P1 128 and stays
  there (30 s); with no input at all the hiss starts it within ~20 s. The threshold is P2 ~210 (P2 205: decays;
  215: grows).
- Clicks (second-difference spikes against the local level, a click scores above ~10): P1 slammed 0 ↔ 255 with
  host smoothing 5.0; note on / off / legato / octave jumps with a sine input 3.6; pan 3.4; level 2.1; bend ±2
  semitones 5.3.
- Pan 0 passes the input within −96 dBFS (the 16-bit gain).
- The device build's output equals the host build's bit for bit on all 11 cycle cases (60 blocks each).

## What to listen for on the device

1. Install on **FX1** and choose it (preset *tape*). Raise a drum or synth track's FX1 send: dotted-eighth repeats
   bounce left, right, left, each a little darker. Pan the FX1 track to 0: only the dry sound; to 255: only the echoes.
2. Turn P1 slowly: the repeats bend down in pitch while the time grows and up while it shrinks, like a tape machine
   changing speed, with no clicks. A fast, wide turn jumps to the new time instead of a long swoop.
3. Turn P2 up: more repeats, darker, more wobble. Past ~210 it runs away into a howl that never clips. Turn P2 back
   down and it dies away. With P2 at 255 and nothing playing it will start howling by itself within half a minute
   (the tape hiss), as a real machine does.
4. Check the P1 values against the tempo: at 120 BPM, P1 117 should sit exactly on the eighths and 149 on the
   quarters.
5. **Notes**: sequence notes on FX1 with nothing sent to it (setting *pluck*): plucked tape-strings at the notes'
   pitches, louder with higher velocity, longer with higher P2, a drone from ~204. Then send a pad to FX1 while the
   notes play: the pad resonates at the notes. After the last note the echo comes back at P1's time.
6. Check that the pitch of a sequenced note matches the same note on a synth track (tuning), and listen for clicks on
   note changes.
7. If FX2 is to be tried (it is outside the `trackmask`, see Facts): put echo there and check the longest time is
   ~1.5 s and that the tape track's audio is untouched.

## Credits

Plucked-string loop after K. Karplus and A. Strong, "Digital Synthesis of Plucked-String and Drum Timbres" (Computer
Music Journal, 1983), with the usual tuning correction for the loop filters' phase delay (D. Jaffe and J. O. Smith,
"Extensions of the Karplus-Strong Plucked-String Algorithm", CMJ 1983). The tape model (one mono tape with two heads,
wow and flutter as integrated speed variation, saturation and losses on every pass) is our own clean-room design from
how tape echoes work; no code or measurements from any tape echo product or emulation. MIT licence.
