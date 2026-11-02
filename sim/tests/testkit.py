#!/usr/bin/env python3
"""Sample packs for opzsim's tests, synthesised here (no recordings) with integer maths only, so the files and the
renders made from them are identical on every Mac.

    python3 sim/tests/testkit.py OUT_DIR

writes, through tools/opz_samples.py (the same writer `opz.py samples build` uses):

  opzk_testkit.aif   drum pack, 24 keys: key k (1..24) is a triangle tone k-1 semitones above C4 (a rising ladder,
                     so the key mapping is audible), 50 + 5k ms long; knob values differ per key
  opzk_testkit5.aif  drum pack with only 5 keys, drum_version 3, editable: keys 6-24 must play key 5
  opzk_plain.aif     plain big-endian AIFF without op-1 metadata: one slice for every key
  opzk_synth.aif     synth sample ("sampler"): A4 triangle, base_freq 440, start/end/loop knobs inside the file,
                     editable, play mode past 16384 (reverse)

These are not the hardware test kit: H-S1 uses `opz.py samples build --test-kit tiny` (research/samples.md).
"""

import array
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "tools"))
import opz_samples as smp  # noqa: E402

SR = smp.SAMPLE_RATE
# Phase increments of C4..B5 (Hz * 2^32 / 44100), precomputed so no float maths reaches the samples.
INC_C4 = [25480119, 26995246, 28600467, 30301139, 32102938, 34011878, 36034330, 38177043, 40447168, 42852281,
          45400411, 48100060, 50960238, 53990491, 57200933, 60602278, 64205876, 68023757, 72068660, 76354085,
          80894335, 85704563, 90800821, 96200119]


def tone(k, frames, level=16000):
    """A triangle with a linear decay: integer only."""
    out, ph, inc = array.array("h"), 0, INC_C4[k]
    for i in range(frames):
        t = ph >> 16                                          # 0..65535
        tri = (t * 4 if t < 32768 else (65535 - t) * 4) - 65535   # -65535..65533
        out.append(tri * level // 65535 * (frames - i) // frames)
        ph = (ph + inc) & 0xFFFFFFFF
    return out


def plain_aiff(samples):
    """FORM AIFF, big-endian 16-bit mono 44.1 kHz, no APPL chunk."""
    pcm = array.array("h", samples)
    if sys.byteorder == "little":
        pcm.byteswap()
    comm = struct.pack(">hIh", 1, len(samples), 16) + bytes.fromhex("400EAC44000000000000")
    body = b"AIFF" + b"COMM" + struct.pack(">I", len(comm)) + comm
    ssnd = struct.pack(">II", 0, 0) + pcm.tobytes()
    body += b"SSND" + struct.pack(">I", len(ssnd)) + ssnd
    return b"FORM" + struct.pack(">I", len(body)) + body


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    sounds = [tone(k, SR * (50 + 5 * (k + 1)) // 1000) for k in range(24)]
    kit = smp.build_drum_pack(sounds, "testkit", volume=[1000 * k + 7 for k in range(24)],
                              pitch=[1111 * k - 13001 for k in range(24)], playmode=[300 * k for k in range(24)],
                              reverse=[20000 if k % 2 else 8192 for k in range(24)])
    pcm = array.array("h")
    starts, ends = [], []
    for k in range(5):
        s = tone(k * 5, SR // 20)
        starts.append(smp.pos_from_frame(len(pcm)))
        pcm.extend(s)
        ends.append(smp.pos_from_frame(len(pcm)))
    meta = {"drum_version": 3, "type": "drum", "name": "testkit5", "editable": True, "start": starts, "end": ends,
            "volume": [8192] * 3, "pitch": [513, -1023, 7]}             # short lists: the OS pads them
    synth = {"type": "sampler", "synth_version": 2, "name": "synth", "base_freq": 440.0, "editable": True,
             "knobs": [300, 2000, 3000, 6000, 20000, 1234, 4321, 9000], "adsr": [64, 10746, 32767, 14096, 4000,
                                                                               4000, 4000, 4000]}
    files = {"opzk_testkit.aif": kit, "opzk_testkit5.aif": smp.write_aiff(pcm, meta),
             "opzk_plain.aif": plain_aiff(tone(9, SR // 10)), "opzk_synth.aif": smp.write_aiff(tone(9, SR), synth)}
    for name, data in files.items():
        info = smp.parse(data)
        if info["errors"]:
            sys.exit("%s: %s" % (name, "; ".join(info["errors"])))
        path = os.path.join(out, name)
        old = open(path, "rb").read() if os.path.exists(path) else None
        if old != data:
            with open(path, "wb") as f:
                f.write(data)


if __name__ == "__main__":
    main()
