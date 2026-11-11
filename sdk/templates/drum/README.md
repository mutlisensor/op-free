# my_engine

<!-- Made from sdk/templates/drum by `tools/opz.py new`. This page is the engine's one-page manual (the web app shows
it): replace the text as the engine becomes yours, keep the table rows. -->

A drum voice for the kick, snare, perc and sample tracks: a sine with a falling pitch, crossfaded into noise.

| | |
|---|---|
| Kind | instrument, drum tracks (`trackmask` 15 = 0x0F), flags 8 (P1 bipolar: centre-detent cue) |
| Slot | @SLOT@ (@SLOT_CLASS@) |
| Plug id | @PLUG_ID@ |
| Keys | 24 pitches, C2 (first key) to B3 (last); each octave press moves them 2 octaves |
| P1 **tune** | −12 … +12 semitones, 128 = as the key says |
| P2 **snap** | sine with a pitch drop (0) → noise (255), crossfaded |
| Envelope | the host's drum envelope over a 0.5 s sound: start, attack, hold, decay knobs, as on TE's drum tracks |
| Filter / resonance | the OP-Z's own track filter |
| Pan / level | the track's pan and level dials, applied by the engine at the end of each block (`opz_mix.h`; the OS does not apply them, and the FX sends are taken after them) |
| Level | −12 dBFS per hit at velocity 127 |
| State | 48 of 280 bytes per voice |
| CPU | fill in from `python3 tools/opz_cycles.py engines/my_engine` |

Presets ([presets.json](presets.json)): `sine drums`. Drum tracks read slot 0 only.

Tests ([tests/](tests/)): `keys` (every key once; each hit sounds and ends by itself), `knobs` (P1 and P2 swept on a
ringing hit, octave presses, every knob at 0 and 255).

## How the code works

my_engine.c, commented line by line; rules in engine-abi.md §3:

- On drum tracks `pitch` is a fixed unity rate, not a note: the pitch comes from `onNoteOn`'s note
  (the 24 keys send MIDI 53–76).
- The envelope is the host's drum envelope: `onNoteOn` turns the four envelope knobs into positions with
  `opz_ahd_layout`, `render` reports how far the sound has played to `updateParametersAHD` once per block and steps
  the level with `opz_ahd_tick`. Past the decay's end the host frees the voice. Note-off does nothing (one-shots).

Licence: MIT. No ported code.
