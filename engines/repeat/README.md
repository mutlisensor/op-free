# repeat

Beat repeat / stutter for FX1 and FX2 (X-04): captures what is sent to it and loops a slice of it in time with the
sequencer — from a knob or from keys held on its own track.

| | |
|---|---|
| Kind | effect, FX1 and FX2 (`trackmask` 768 = 0x300), flags 1 (insert-type FX) |
| Slot | 71 (5 KB class), 1.8 KB used on the device build |
| Plug id | 61100 |
| P1 **division** | 0–31 **off** (the input passes through); then 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/64 (32 per zone). Leaving "off" starts a repeat; moving P1 while repeating changes the slice length at the next repeat over the same capture, so a sweep is a roll |
| P2 **decay** | each repeat against the one before: 0 = all at full level, 128 = −3 dB, 255 = −12 dB (−12·u² dB) |
| pan | dry/wet, TE's balance law normalised to unity (centre = half and half). The preset is wet only: the stutter replaces the sound |
| level | output level, times the FX track's mixer level and mute |
| Filter / resonance | the OP-Z's own track filter, also on FX tracks |
| **Notes on the FX track** | a held key repeats while held, whatever P1 says; every new note starts a new capture. The 24 keys from F3 pick the division, long (left) to short (right): 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T, 1/64, 1/64T, 1/128, 1/128T. **The upper row (F4–E5) plays the same divisions reversed.** Other octaves repeat the pattern. The OS's velocity-0 re-send of a held note keeps the capture |
| Memory request | 240 KB (61,440 stereo 16-bit frames, 1.39 s): fits FX2's 256 KB pool. Never cleared: nothing reads a frame the current capture has not written |
| State | 64 of 200 bytes (device) |
| CPU | Blackfin simulator (`tools/opz_cycles.py engines/repeat`): 59 cycles/frame off, 138 at 1/4, 176 at 1/64, **worst block 209** (1/128T reversed with knobs moving every block), against the ≤ 600 effect target. Note-on 286 |

Not for master: the OS passes `out = NULL` there ([engine-abi.md](../../sdk/docs/engine-abi.md) §3).

## How it works

**Forward capture.** A repeat records the input from the moment it starts and plays it as it goes, so the first
pass *is* the live sound: no wait, no click. At the end of the slice it loops what it recorded (reversed on the
upper keys: the first pass is still forwards, the reversal starts with the first repeat). Each slice is gated with a
short linear fade at both ends (128 frames, 64 or 32 for short slices), the classic stutter; the first pass has no
fade-in. Stopping, or a new note while repeating, crossfades to the live input over 64 frames (1.5 ms); a new
capture starts after that crossfade.

**Tempo.** The slice is `10,584,000 / (den × BPM)` frames, with the tempo read every block from sequencer key 4
(whole BPM, 40–200), as TE's delay does ([engine-abi.md](../../sdk/docs/engine-abi.md#sequencer-keys), Q-18 [code];
hardware check H-7 with `engines/opz_test_tempo` has not run yet). If the key gives nothing usable repeat runs at
120 BPM. A quarter note below 43 BPM is capped at the buffer (1.39 s). Repeats start when the note or the knob
says, not on the next grid line: sequenced notes land on steps anyway, a hand-turned knob starts at once. The
length is latched per pass, so a tempo change applies at the next repeat.

## Preset

FX tracks apply only slot 0 index 0, when the plug is chosen. **roll**: P1 0 (off), P2 64 (−0.75 dB per repeat),
pan 255 (wet only), level 255, byte 12 (FX dry level) 0 for an insert. Settings to try:

| Name | P1 | P2 | pan | Sound |
|---|---|---|---|---|
| roll (preset) | 0 | 64 | 255 | nothing until a key or P1; then near-constant repeats |
| quarter loop | 40 | 0 | 255 | a beat-long loop that holds |
| sixteenth roll | 140 | 30 | 255 | drum-roll stutter |
| buzz | 230 | 0 | 255 | 1/64: a pitched machine-gun buzz |
| echo out | 70 | 200 | 128 | eighth repeats that fade like a dub echo, under the dry sound |

## What to listen for

1. Install on FX1, send a drum track to FX1 at full send. P1 at 0: the drums pass unchanged (no level jump, no
   click when the plug is chosen).
2. Turn P1 to ~40 (1/4) while playing: the beat that follows is played, then repeats exactly, in time with the
   metronome. Back to 0: straight back to the live drums without a click.
3. Sweep P1 slowly up to 255 while repeating: the repeats get shorter (roll) without restarting; at the top a
   pitched buzz. A step-locked P1 on the FX track gives a sequenced roll.
4. P2: at 0 the repeats hold their level; at 255 each is 12 dB quieter (gone after 4–5).
5. Hold keys on FX1: left keys long, right keys short; the upper row the same, reversed after the first pass.
   Sequence a few notes (e.g. F3 on step 13, C#4 on 15, F4 on 16): fills that stay in time.
6. Change the tempo while a key is held: the next repeat takes the new length.
7. Fast: turn both knobs quickly while keys play: no crackle, no stuck repeat, no level jump. `opz log` shows no
   `cpu overload`.

## Tests

[tests/](tests/): `notes` (held keys F3, A3, C#4, F4, C#5 on a rising sweep: forward repeats match to −95 dB and
the reversed one is the exact reverse), `knob` (P1 swept through every zone while repeating, on clicks), `decay`
(1/8 repeats at P2 255: −12.0 dB per pass, measured; then P2 0, 50/50, a key takes over), `extremes`.

## Credits

Clean-room design (Ableton's Beat Repeat and the PO punch-in effects as the inspiration; no code from any).
MIT licence.
