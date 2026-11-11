# Engine templates

Starting points for a new engine. Don't copy them by hand: `python3 tools/opz.py new NAME --kind synth|drum|fx`
copies one to `engines/NAME/`, fills in the name, a free test plug id and a free memory slot, and records the slot in
[memory-slots.md](../docs/memory-slots.md). The whole path from there to an installed engine is in
[QUICKSTART.md](../docs/QUICKSTART.md).

| Template | Tracks | Shows |
|---|---|---|
| [synth](synth/) | bass, lead, arp, chord | `render`, the host ADSR, velocity, pitch glide, band-limited saw |
| [drum](drum/) | kick, snare, perc, sample | pitch from the note, the host drum envelope (`updateParametersAHD`) |
| [fx](fx/) | FX1, FX2 | `renderFX`, per-block coefficients, the SDK's dry/wet law, level and output limiter |

In the templates `my_engine` is the placeholder name, `"memoryslot": 0` and `"id": 0` are filled in by the
scaffold (both are invalid on purpose), and `@SLOT@`, `@SLOT_CLASS@`, `@PLUG_ID@` in the README are replaced.
`tools/tests/test_opz_new.py` scaffolds each template and, when `sim/build/opzsim` exists, runs its scenarios.
