# opzsysex (vendored)

Unmodified copy of the `opzsysex` Python package from
[ioma8/opz-firmware-notes](https://github.com/ioma8/opz-firmware-notes), directory `refs/op-z-sysex/opzsysex/`,
commit `79ce9bd1ee89e3e8092d1b9eb8cb3591cb667bdf` (2026-10-16). MIT licence, copyright (c) 2026 Kevin Morrill —
see [LICENSE](LICENSE).

Used by `tools/opz.py` for SysEx framing, 7-bit packing, zlib parts and the file-transfer upload/download state
machines. Live MIDI I/O (`device.py`) needs `mido` and `python-rtmidi`. Don't edit these files; wrap or extend
them in `tools/` instead, so the copy can be refreshed from upstream.
