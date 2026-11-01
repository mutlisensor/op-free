# glue

Stereo bus compressor and soft clipper for **FX1 and FX2**: send tracks to it to glue them together. One knob sets how much
it squeezes, the other its character, from slow, clean "glue" through fast pumping to saturation. A clipper at the end
keeps the output below 0 dBFS. Notes on its track duck the sound like a sidechain.

| Control | Function |
|---|---|
| P1 **amount** | how hard it compresses. **0 = no compression** (only the safety clipper, which leaves everything below −1.4 dBFS untouched). Turning it up lowers the threshold from 0 to −30 dBFS and raises the ratio from 1:1 to 5:1 together (64 ≈ −7.5 dB at 1.5:1, 128 ≈ −15 dB at 2.5:1, 192 ≈ −23 dB at 4:1, 255 = −30 dB at 5:1). **Auto make-up** keeps a sound peaking at −12 dBFS at the same level, so the mix gets denser, not quieter: quiet parts come up, loud peaks come down |
| P2 **character** | 0–127: from **slow, clean glue** (20 ms attack, auto release 150 ms–0.7 s, wide 12 dB knee) to **fast and pumping** (1 ms attack, release 50–120 ms, 4 dB knee). 128–255: stays fast and **drives the clipper**, 0 → +12 dB of drive, with the output turned down by half the drive. Saturation then grows from none to thick. Continuous, so an LFO on P2 glides through it |
| pan | dry/wet: 0 dry, rising to fully processed at the centre (128); 128–255 stay fully processed. Below the centre glue is a **parallel ("New York") compressor**: the dry signal keeps the transients, the squashed copy adds density |
| level | output level (255 = unity) |
| filter, resonance | not used: they are the OS's own track filter, also on FX tracks. Presets leave it neutral (128, 0) |
| **notes on its track** | each note **ducks** the signal: the gain drops within ~2 ms by up to **24 dB (velocity / 127 × 24 dB)**, stays down while the note (or any note) is held, then comes back at the release speed (τ ≈ 150 ms at P2 0, 50 ms from P2 128). Sequence short notes on the beat for a sidechain pump; longer notes hold the dip longer. Chords duck as deep as their loudest note |
| **no notes** | a plain bus compressor |

**Auto release**, how P2's release times work: a fast stage follows the compressor, and a slow stage charges from it
over ~0.4 s and lets go slowly. The deeper of the two applies. A short peak recovers at the fast time; compression that
has been going on for a while recovers at the slow time, so sustained parts don't pump.

**Why not the master track?** On the OP-Z the master track is a fixed chain, not an effect slot: you can't choose
an effect there, and its notes transpose the song (TE's manual, p.33, 40, 44–45; [Q-19](../../research/OPEN_QUESTIONS.md)).
So glue is an FX1/FX2 effect. Notes sequenced on FX1/FX2 reach it, which is what the ducking uses.

## Preset

| Name | P1 | P2 | pan | Idea |
|---|---|---|---|---|
| glue | 72 | 16 | 128 | a few dB of slow, clean bus compression on the whole mix (~16 ms attack, auto release) |

At level 255 (unity), filter 128 / resonance 0. Effect tracks have no preset keys, so an effect plug only ever uses
this one preset (slot 0, index 0). A draft: tune it by ear on the device.

### Settings to try

| Name | P1 | P2 | pan | Idea |
|---|---|---|---|---|
| punch | 140 | 40 | 128 | firmer, ~8 ms attack lets the drum transients through while the body is held together |
| pump | 200 | 127 | 128 | heavy and fast (1 ms attack, ~50 ms release): the mix breathes with the kick |
| warm | 96 | 180 | 128 | moderate compression plus ~+5 dB of drive into the clipper: rounded, saturated peaks |
| duck | 48 | 64 | 128 | light compression; sequence notes on the track (FX1/FX2) for sidechain-style ducking |
| parallel | 200 | 100 | 64 | heavy compression mixed under the dry signal: dense but keeps every transient |

## Facts

| | |
|---|---|
| Plug | 61004, version 2, `trackmask` 0x300 (FX1, FX2), flags 1 (insert-type FX, [host-tables-flags.md](../../research/host-tables-flags.md#recommended-flags-for-our-engines)) |
| Memory slot | 84 (5 KB class), 3,144 of 5,120 bytes used (Blackfin build, `-O2`) |
| Memory request | **0** (no buffer, no look-ahead delay) |
| State | 112 B of 200 |
| CPU (Blackfin simulator, `tools/opz_cycles.py`) | **~109 cycles per stereo sample** (6,960–7,015 per block) on every P1/P2 combination, ~123 in a block where P1 or P2 moves; ~126 with pan below the centre (dry path on). Note-on 64 cycles. 1.2 % of the 580,499-cycle block |
| Latency | none |

How it works: stereo-linked peak detector → gain computer in the log domain (threshold, ratio and a quadratic soft
knee) → attack and two-stage release smoothing → make-up, drive and duck → soft clipper → dry/wet and level. The
detector and gain computer run every 16 samples on the peak of the *next* 16 input frames. The whole block is already
in the input buffer, so the gain sees the peak it will be applied to. The gain then ramps sample by sample, so nothing
steps. The clipper passes everything below its knee unchanged, then bends with a parabola into a ceiling of
−0.18 dBFS with a continuous slope. The knee is −1.4 dBFS at P2 0, −3.1 dBFS at P2 128 and −10.5 dBFS at P2 255. It is
branchless, so CPU use doesn't depend on the signal. Coefficients are recomputed only when P1 or P2 moves. Q31/Q26
integer maths, saturating, no division, no `int64` in per-sample code, no libgcc.

Offline checks (a harness linking `glue.c` directly, plus the six golden scenarios; `fx2_pump` replaced the earlier
master-track scenario):

- Static curve (100 Hz sine, output peak for input peak, P2 0): P1 128 maps −36/−24/−12/−6/0 dBFS to
  −33.9/−21.9/−11.6/−8.7/−6.2; P1 255 to −21.6/−13.7/−10.9/−9.7/−8.5. P1 0 is unity up to the clipper (0 dBFS
  → −0.4).
- Neutral (P1 0, P2 0) on a −6 dBFS signal: output = input within −96 dBFS (the 16-bit output gain: −0.0003 dB).
- Full-scale square waves over a P1/P2 grid: output peak −0.175 dBFS, never above the ceiling.
- Recovery at P1 200 from −6 to −30 dBFS input: P2 0 recovers to within 1 dB in 1.9 s after 2 s of compression, but in
  1.1 s after a 50 ms burst (the auto release). P2 127/255: 0.38 s and 0.30 s.
- Duck at velocity 127: −24.1 dB within one 50 ms window, held while the note is held, then −13.6/−7.5/−4.2/−2.3 dB
  in the following 50 ms windows. Velocity 64: −12.1 dB. `onNoteOn` with velocity 0 and no params is handled (the
  ABI's re-sent held note).
- P1 and P2 jumped between 0 and 255 every 100 ms (host-smoothed) on a 50 Hz sine: the largest sample step is twice
  the input's own, with no clicks.

## What to listen for on the device

1. **FX2** (or FX1), preset *glue*: send a busy drum track and a bass to it at full send. With P1 at 0 it should
   sound exactly like no effect. Turn P1 up slowly: the sound gets denser and more even, but not louder. Peaks no
   longer poke out, quiet tails come up. Nothing should click when the knobs move.
2. Turn P2 right through: at 0 the drums keep their snap and the compression is smooth. Towards 127 it gets fast and
   breathy, and with P1 high it should **pump** with the kick (setting *pump*). Past 128 it starts to saturate:
   louder, rounder, crunchier at 255.
3. Play loud: whatever you do, the output should never clip hard (the clipper's ceiling is −0.18 dBFS).
4. **Notes**: with the *duck* setting, sequence notes on the FX track on every beat while a pad or bass is sent to
   it: a clean sidechain pump, deeper with higher velocity, held for long notes.
5. Pan below the centre mixes the dry signal back in (parallel compression, setting *parallel*).

## Credits

Compressor structure after the textbook feed-forward design in D. Giannoulis, M. Massberg and J. D. Reiss, "Digital
Dynamic Range Compressor Design — A Tutorial and Analysis" (JAES, 2012): log-domain gain computer with a quadratic
knee, smoothing in the log domain. The equations only, no code. The two-stage auto release, the note ducking, the
clipper and the fixed-point design are our own. MIT licence.
