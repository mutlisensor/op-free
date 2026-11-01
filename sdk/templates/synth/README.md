# my_engine

<!-- Made from sdk/templates/synth by `tools/opz.py new`. This page is the engine's one-page manual (the web app shows
it): replace the text as the engine becomes yours, keep the table rows. -->

A synth for the bass, lead, arp and chord tracks: one oscillator from sine to saw, with a sub-octave sine.

| | |
|---|---|
| Kind | instrument, synth tracks (`trackmask` 240 = 0xF0) |
| Slot | @SLOT@ (@SLOT_CLASS@) |
| Plug id | @PLUG_ID@ |
| P1 **shape** | sine (0) → band-limited saw (255), crossfaded |
| P2 **sub** | sine one octave below: off (0) → as loud as the main oscillator (255) |
| Envelope | the host's ADSR (attack, decay, sustain, release knobs) |
| Filter / resonance | the OP-Z's own track filter (the engine doesn't read them) |
| Level | −12 dBFS per voice at velocity 127 with the sub off, up to −6 dBFS with it full |
| State | 12 of 280 bytes per voice |
| CPU | fill in from `python3 tools/opz_cycles.py engines/my_engine` (cycles per sample per voice; budget ~167) |

Presets ([presets.json](presets.json)), one per synth track: `sub bass`, `saw lead`, `pluck` (arp), `soft pad` (chord).

Tests ([tests/](tests/)): `basic` (notes across the range, gate, release, voices freed, level), `knobs` (P1 and P2
swept under a chord with pitch bends, then every knob at 0 and 255).

## How the code works

my_engine.c is the whole engine, commented line by line. The entry points and the rules they follow
are in engine-abi.md §3 and §6:

- `initModule` keeps the host's callback table; `init` clears a voice's 280-byte state.
- `onNoteOn` stores the velocity and resets the phases. It must not latch a frequency: with the legato note style
  new notes only move `pitch`.
- `render` runs once per voice per 64-frame block: it lets the host move the envelope
  (`updateParametersADSR`), computes per-block values, then adds 64 stereo frames into the track buffer with
  `opz_mul_q31`/`opz_add_q31`, stepping the envelope (`opz_env_tick`) and gliding the pitch (`pitchSlope`).
  The host frees the voice when the envelope reaches stage 0.

Licence: MIT. No ported code.
