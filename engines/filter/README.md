# filter

Resonant stereo filter for the FX tracks, with an envelope follower (auto-wah), played by notes sequenced on its own
track.

| Control | Function |
|---|---|
| P1 **cutoff** | 30 Hz–18 kHz, exponential (0.43 semitone per step; 128 ≈ 745 Hz). Steps 242–254 fade the filter out towards the dry sound; **255 is fully open** (the input passes unchanged) |
| P2 **character** | one macro over resonance, shape and movement (table below) |
| pan | dry/wet: 0 dry, rising to fully filtered at the centre (128); 128–255 stay fully filtered. Unlike TE's effects the centre is all wet, because a half-dry filter barely filters |
| level | output level |
| filter, resonance | not used: they are the OS's own track filter, also on FX tracks. Presets leave it neutral (128, 0) |
| **notes on the FX track** | the cutoff moves to the **note's pitch** and P1 shifts it from there: P1 128 puts it exactly on the note, each step away moves it 0.43 semitone. Each note also **sweeps the cutoff up** 2–4 octaves (more with higher velocity) and lets it fall back: ~250 ms while the note is held, ~60 ms after note off, so short steps pluck and long notes sweep. With resonance up, sequencing notes plays the filter's ringing in tune |
| **no note held** | the filter sits at P1 (the note offset glides back in ~40 ms) |

| P2 | Shape | Resonance | Movement |
|---|---|---|---|
| 0–95 | low-pass, 12 dB/octave | none (Q 0.7) → strong (Q 10); the pass band drops up to 6 dB as it rises | — |
| 96–127 | low-pass morphs into band-pass | Q 10 | — |
| 128–255 | band-pass eases back to a resonant low-pass (30 % band-pass at 255) | Q 10 → Q 4 | **auto-wah**: the envelope follower opens the filter with the input's loudness, 0 → +5 octaves above P1 |

The curve is continuous, so an LFO on P2 glides through it without steps. The follower is logarithmic: input peaks
from −48 dBFS to −6 dBFS open the filter evenly, with a 2 ms attack and 150 ms release; P1 sets where the wah rests
when the input is quiet. Both channels share one cutoff, so the filter stays mono-compatible.

There is no internal LFO: the OS's own LFO can target P1 (cutoff) with its speed, shape and sync, and notes on the FX
track give tempo-locked sweeps.

## Preset and settings to try

FX tracks have no preset keys: the OP-Z applies one preset, slot 0 index 0, when filter is chosen
([presets.md](../../research/presets.md)). That preset is **warm**. The other sounds are settings to dial in:

| Name | P1 | P2 | Idea |
|---|---|---|---|
| **warm** (preset) | 150 | 24 | gentle low-pass at ~1.3 kHz, a touch of resonance |
| acid | 80 | 88 | dark, strongly resonant low-pass; sequence notes on the FX track for squelchy sweeps |
| vowel | 140 | 118 | resonant low-pass/band-pass blend, talky |
| auto wah | 70 | 200 | envelope follower on drums or bass |
| note ping | 128 | 95 | Q 10 low-pass that sits exactly on each sequenced note and pings down onto it |

All at pan 128 (fully filtered), level 200, filter 128 / resonance 0.

## Facts

| | |
|---|---|
| Plug | 61002, version 2, `trackmask` 0x300 (FX1, FX2), flags 1 (insert-type FX, [host-tables-flags.md](../../research/host-tables-flags.md#recommended-flags-for-our-engines)) |
| Memory slot | 86 (5 KB class), 3.2 KB used at `-O2` |
| Memory request | 0 (no buffers) |
| State | 60 B of 200 |
| CPU | ~14 ns/sample on an M-series Mac (opzsim). Blackfin not yet measured; the inner loop is ~125 instructions per channel plus the control code every 16 samples, so an estimated 250–350 cycles per stereo sample, under the 600 target |

Core: the trapezoidal ("zero-delay feedback") state-variable filter from Andrew Simper's published derivation —
stable at any cutoff and resonance, tuned by `g = tan(π·f/fs)` from a 77-entry table (1/8 octave). Q31 integer
maths with 24 dB of internal headroom and saturation everywhere (no denormals, no wrap-around), a soft clip above
−6 dBFS on the filtered path, no division (the coefficient `1/(1 + g(g + k))` uses Newton-Raphson). Coefficients,
envelopes and the dry/wet mix update every 16 samples; output gains and the mix ramp per sample, so knob moves don't
click.

Offline checks (a test harness linking `filter.c` directly, plus the six golden scenarios):

- At P1 255 / P2 0 the output equals the input times level, bit for bit.
- Notes put the resonance on the note: A1–A7 measured within −6…0 cents at P2 95 (the low-pass peak sits a few cents
  below the cutoff at Q 10, as it should), 0 cents in band-pass.
- Full-scale square waves at P2 0, 95, 127, 160 and 255 (with random P1 jumps and notes every 70 ms at 95 and 160):
  nothing blows up, and 3 s after the input stops the output is at or below −100 dBFS.
- Host-smoothed sweeps of P1 and P2 across their whole range on a sine: no sample step beyond what the resonance
  peak itself produces (no block-edge clicks).
- Loudness moves smoothly over the whole P2 range, without dips: −0.5 to −7 dB on a 110 Hz saw at P1 128.

## What to listen for on the device

Put it on FX1 or FX2 with a synth or drums sent to it. P1 sweeps the cutoff and fully clockwise passes the sound
untouched; P2 adds resonance (left half), then turns into an auto-wah that opens with every hit (right half).
Sequence notes on the FX track with the "note ping" setting (P1 128, P2 95): each note should ping the filter in
tune with the note.

## Credits

Filter structure after Andrew Simper (Cytomic), "Solving the continuous SVF equations using trapezoidal integration
and equivalent currents" (2013); the equations only, no code. Envelope follower, note behaviour and fixed-point design
are our own. MIT licence.
