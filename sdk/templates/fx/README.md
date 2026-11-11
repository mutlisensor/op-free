# my_engine

<!-- Made from sdk/templates/fx by `tools/opz.py new`. This page is the engine's one-page manual (the web app shows
it): replace the text as the engine becomes yours, keep the table rows. -->

A drive effect for FX1 and FX2: gain into a soft clipper, then a tone control.

| | |
|---|---|
| Kind | effect, FX1 and FX2 (`trackmask` 768 = 0x300), flags 1 (insert-type FX) |
| Slot | @SLOT@ (@SLOT_CLASS@) |
| Plug id | @PLUG_ID@ |
| P1 **drive** | 1× (0) → 16× (255) into a soft clipper (1.5x − 0.5x³) |
| P2 **tone** | one-pole low-pass after the clipper: dark (0) → open (255) |
| pan | the SDK's FX law (`opz_fx_wet_linear`): 0 dry only, 64 half and half, 128 … 255 the effect only. No control on the OP-Z reaches an FX track's pan (it stays at 127, the effect only), so players hear the effect alone |
| dry level (hold the FX track's key + red dial) | the OS's dry level: how much of the sending tracks' own sound stays in the mix (255 send/return, 0 insert). The player's dry/wet control on the OP-Z |
| level | output level, times the FX track's mixer level and mute |
| Filter / resonance | the OP-Z's own track filter, also on FX tracks |
| Notes on the FX track | ignored |
| Memory request | 0 (no buffer) |
| State | 8 of 200 bytes |
| CPU | fill in from `python3 tools/opz_cycles.py engines/my_engine` (cycles per sample; target ≤ 600) |

Not for master: the OS passes `out = NULL` there (engine-abi.md §3), so the
trackmask leaves master out.

Preset ([presets.json](presets.json)): `warm drive`. FX tracks apply only slot 0 index 0, when the plug is chosen;
its byte 12 (0) is the FX dry level of an insert effect.

Tests ([tests/](tests/)): `basic` (drive and tone swept on a sawtooth, wet only then 50/50 at pan 64), `extremes` (every knob
at 0 and 255 on a full-scale sweep, then dry only).

## How the code works

my_engine.c, commented line by line; rules in engine-abi.md §3 and §6:

- `renderFX` gets 64 frames of interleaved stereo in `in` and must write all 128 values of `out`.
- Coefficients come from the knobs once per block; the sample loop uses only `opz_mul_q31`, `opz_add_q31` and
  saturating shifts.
- The filter memory is the only state (`State`, at most 200 bytes per FX track: `OPZ_FX_STATE_BYTES`). An effect
  that needs a delay line asks for it in `initModule` (`*memRequest`) and clears it a piece per block; see `engines/swirl`.

Licence: MIT. No ported code.
