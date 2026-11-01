# swarm

Tonal noise and oscillator swarms for the bass, lead, arp and chord tracks. One knob (P1, **density**) goes from a
few sparse particles — pings and crackle tuned to the note — through a sparkling cloud and fizzing tonal noise to a
dense cloud of twelve detuned saws, a big unison drone. The other (P2, **spread**) opens the swarm from unison
through a chorus and a wide tone cluster to inharmonic, bell-like ratios. Whatever the knobs do, the result stays
tuned to the played note, and the swarm is spread across the stereo field. The OP-Z's own track filter (the filter
and resonance knobs) shapes the tone after it.

| Control | Function |
|---|---|
| P1 **density** | **0–112: particles only.** Random particles ring short sine pings ("resonators" of quality Q) on and around the note: **0** ~1.5 a second (each note starts with a ping on the note itself, then now and then another), **64** ~30 a second (a sparkling cloud), **112** ~290 a second (a dense fizz). The pings ring shorter as they get denser (Q 250 at 0 → 25 at 192; between 40 ms and 1.5 s). **112–224: the crossfade.** The saws come in one after the other (the centre saw first) while the particles thicken into tonal noise (up to ~5,500 a second) and fade out (equal power); from **160** the particle cloud thins to one partial a side. **224–255: the saw swarm alone** (all twelve by 255); each saw swells slowly on its own (0.13–0.63 Hz), deeply at first (0.85) and steadier towards 255 (0.35) |
| P2 **spread** | **0–128: unison → chorus.** The swarm's half-width grows from 0 to ±50 cents (quadratic, fine near 0; the saws drift ±1.5 cents even at 0, so unison still moves). **128–192: wide cluster**, ±50 → ±200 cents: a tone cluster around the note. **192–255: inharmonic.** Every saw and particle glides (smoothstep) from its place in the cluster to one of a circular membrane's mode ratios (1, 1.59, 2.14, 2.30 … 4.15) while the cluster closes to ±30 cents; the centre saw and three in eight particles stay on the note, and the other saws get quieter towards 1/√ratio. Stereo width opens over 0–32 (60 % at 0) |
| velocity | level (linear), and the **crackle**: every particle adds a one-sample click at its own sample, none below velocity 24, then growing with velocity² (0.3 of the particle's level at 127; half that for a note's first ping) |
| notes | each note is one swarm: twelve free-running saws with random phases (each note's unison sounds a little different), four particle partials, its own random numbers. Pitch bend and portamento move everything; the particles' rings follow |
| attack … release | the OP-Z envelope |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |

**The saw swarm.** Twelve saws, each with its own place in the cluster, its own stereo position (from hard left to
hard right, alternating flat and sharp; narrower below 150 Hz, mono two octaves below, so bass stays
mono-compatible), its own slow swell and drift. As in [`super`](../super/README.md), a weighted sum of naive saws is
one straight ramp plus a step wherever one of them wraps, so each saw costs only its starting value, its slope and
its few wraps per block (each wrap writes its step and two-sample PolyBLEP residual into an event buffer); per
sample the left and right signals are only integrated. The weights are normalised by 1/√(sum of squares), so the
level stays even while saws come in, swell and spread. Saws fade out between 4 and 6 kHz, and a voice spends at most
~16 saw wraps per block: on very high notes and wide inharmonic ratios the last saws fade out (smoothly, in order),
which bounds the cost.

**The particles.** A Poisson-like count of random impulses per block excites four sine partials, each decaying like
a resonator of quality Q: an exact exponential per block, ramped per sample, so nothing can blow up. A particle
that is louder than what still rings on its partial re-pitches it to a new random place in the cluster (or to an
inharmonic ratio): sparse particles scatter in pitch, dense ones settle into tonal noise around four pitches.
The particle level follows the overlap (RMS-normalised for dense clouds, capped for single pings), and where the
cloud is dense a note starts with its partials already ringing. Partials 0 and 2 sit left, 1 and 3 right, with a
quarter crossing over.

## Presets

Five per synth track. `slot` in `presets.json` is the track (0 bass, 1 lead, 2 arp, 3 chord); index 0 of each is the
sound a track gets when it switches to swarm.

| Track | Preset | Octave | P1 | P2 | A | D | S | R | Level | Sound |
|---|---|---|---|---|---|---|---|---|---|---|
| bass | **swarm bass** | −2 | 240 | 40 | 0 | 80 | 200 | 30 | 205 | dense saw swarm, slight chorus |
| bass | drop bass | −2 | 45 | 30 | 0 | 100 | 170 | 60 | 215 | a ping on each note with a few stray drops |
| bass | fizz bass | −2 | 150 | 60 | 0 | 90 | 200 | 35 | 210 | tonal noise with the first saws |
| bass | hive bass | −2 | 205 | 150 | 0 | 85 | 190 | 30 | 205 | buzzing wide-cluster bass |
| bass | metal bass | −2 | 255 | 230 | 0 | 70 | 120 | 30 | 205 | inharmonic swarm, gong-like |
| lead | **swarm lead** | 0 | 255 | 90 | 5 | 90 | 220 | 50 | 195 | twelve-saw lead, chorus |
| lead | fizz lead | 0 | 160 | 40 | 8 | 90 | 220 | 55 | 200 | breathy tonal noise and saws |
| lead | bees | 0 | 220 | 170 | 10 | 90 | 220 | 50 | 195 | wide tone cluster |
| lead | glass rain | 0 | 40 | 210 | 0 | 120 | 230 | 90 | 205 | sparse bell-like particles |
| lead | dust lead | 0 | 110 | 128 | 0 | 90 | 220 | 60 | 200 | a cloud of pings around the note |
| arp | **spark arp** | 0 | 70 | 90 | 0 | 40 | 0 | 40 | 200 | short sparkles |
| arp | drop arp | 0 | 20 | 20 | 0 | 50 | 0 | 60 | 205 | water-drop pings |
| arp | bell arp | 0 | 30 | 240 | 0 | 45 | 0 | 50 | 205 | inharmonic pings |
| arp | saw arp | 0 | 255 | 70 | 0 | 35 | 0 | 25 | 195 | short supersaw-like stabs |
| arp | fizz arp | 0 | 150 | 110 | 0 | 40 | 60 | 35 | 200 | noisy plucks |
| chord | **swarm pad** | 0 | 230 | 110 | 100 | 140 | 230 | 120 | 165 | slow-swelling saw swarm |
| chord | drone | 0 | 255 | 20 | 120 | 140 | 255 | 140 | 165 | near-unison drone |
| chord | cloud pad | 0 | 120 | 150 | 80 | 140 | 230 | 130 | 170 | particle cloud, wide cluster |
| chord | rain | 0 | 50 | 128 | 0 | 150 | 220 | 150 | 175 | particle rain on every chord note |
| chord | metal pad | 0 | 245 | 220 | 90 | 140 | 220 | 120 | 165 | inharmonic pad |

Every preset: filter 128, resonance 0, pan 128, sends 0, **no LFO** (depth 128, speed 100, target 255, shape 0; an
LFO on P1 makes the swarm breathe between particles and saws, on P2 it wanders between unison and cluster). Chord
presets sit lower in level because four voices add up. Names fit the OP-Z's 11 characters.
`python3 tools/opz.py presets check engines/swarm` reports 20 presets, no warnings. Not yet tuned by ear on the
device.

## Facts

| | |
|---|---|
| Plug | 61202, `trackmask` 0xF0 (bass, lead, arp, chord), version 1 (engine version 1) |
| Memory slot | 40 (10 KB class): 9,364 of 10,240 bytes on Blackfin (8,328 B of code and tables, 1,036 B of `.bss`: the shared block buffers) |
| State | 232 B of the 280 the OS leaves per voice; `memRequest` 0; no libgcc |
| CPU | Blackfin simulator ([cycle-model.md](../../research/cycle-model.md), `tools/opz_cycles.py`), 4-note chord C3 G3 C4 E4 at velocity 127, mean / worst block per voice in cycles per sample: **P1 0** (sparse pings) **67 / 68**; P1 64 80–83 / 98; P1 128 107 / 116; P1 160 106–115 / 134; **P1 192** (crossfade) **126–138 / 169**; **P1 255** (densest, all twelve saws) **103 / 140 at P2 0–160, 131 / 177 at P2 255** (inharmonic: more wraps); high notes C6–E7 102–131 / 124–163; top notes 108–127 104 / 158; low notes C0–E1 103 / 117; release 114 / 132; **P1 and P2 moving every block** 114–115 / 203–205. **19 voices** densest (P1 255, P2 255): 125 per voice, ~151,600 cycles per block in all (worst block 183,400); 19 voices in the crossfade (P1 176, P2 255): 130 (158,100; worst 174,100); 19 voices C5–F♯6 with P1 and P2 jumping every block: 125 (151,400; worst 252,500, the one case near the OS's 265,000 line — the benchmark moves the knobs a sixth of their range per block, unsmoothed, which the OS's own parameter smoothing never does). Note-on 252 cycles |
| Level | per voice, velocity 127, one held note: over P1 64–255, every P2 and C1–C7, −20.3 … −25 dBFS RMS, peaks −7.5 … −14 dBFS; the saw swarm sits at ~−20.5 dBFS RMS like `super`. Sparse settings (P1 0–40) are quieter on average but each ping peaks at about −11 … −13 dBFS; on high notes the pings are shorter (constant Q, 40 ms at least). Exact unison (P2 0, P1 255) depends on the note's random phases (±3 dB). Scenarios: a 5-note "swarm pad" chord peaks −4.3 dBFS, fast knob moves over chords −3.0 dBFS, no clipping |

Simulator checks: six golden scenarios (P1 density sweep over a chord, P2 spread sweeps on the saws and on the
particles, sparse particles on the bass track with steals and a bend, chords with steals, extremes, knobs moving
every block while chords start, steal and release), all voices freed. [tests/fuzz.c](tests/fuzz.c) (build line in
the file, UBSan with bounds checks): the saw events against a per-sample sum of PolyBLEP saws (6.2e-5 of full scale,
the reciprocal approximations), every P1 × P2 byte at notes 0–127 (3.8 million blocks; the loudest sample 0.51 of
full scale with the envelope at 1.0, the event buffers left clean), and 200,000 fuzz blocks (random notes and
velocities, params NULL at note-on, knob jumps including out-of-range words, pitch 0 / 0xffffffff / random with
random slopes, random envelope stages and steps, voice re-use without init): deterministic, adds into out, no wraps,
envelope ≥ 0, nothing written past the state. On the Blackfin the particle and output loops are scheduled by hand;
every benchmark case renders bit-identically to the C loops (`-DSWARM_C_LOOP`, `opz_cycles.py --dump/--compare`).
P1/P2 sweeps show no extra energy at block edges (second difference at the block's first sample within ±1 dB of
the other positions).

## What to listen for on the device

Install it on the chord track (it starts on "swarm pad"), or on bass ("swarm bass"):

1. **P1 slowly from 0 to 255** on a held chord: single pings on the notes → a sparkling cloud → fizzing tonal
   noise → saws coming in one by one → a dense drone. No steps or holes on the way, about the same loudness
   throughout (sparse settings are naturally emptier). Say where it is most useful and where nothing happens.
2. **P2 slowly from 0 to 255** at P1 255: unison (slowly phasing), chorus, a buzzing cluster around the note, then
   bell-like/gong-like inharmonic. It should stay recognisably on the note the whole way.
3. **Sparse end** (P1 0–60, "drop arp", "glass rain"): each note starts with a ping on its pitch; stray pings
   follow at random times and pitches. Play soft and hard: hard notes add a crackle (clicks), soft ones none.
4. **Low and high notes:** C1–C2 on "swarm bass" (mono-ish, no rumble), C6–C8 on "swarm lead" (the swarm thins
   out at the very top, no whistling or aliasing).
5. **Stereo:** the swarm should be wide, particles hopping left and right; bass notes narrower.
6. **Turn P1 and P2 fast over chords** (the situation that crashed macro v3): no glitches, crashes or hangs; also
   with an LFO on P1.
7. **CPU:** a full chord track of "swarm pad" plus the other tracks should run clean; then all four synth tracks
   on swarm with P1 around 190 and P2 at 255 (the most expensive corner).
8. **Level:** about as loud as `super`; say if any preset jumps out.

## Credits

Ideas from the OP-XY's *dissolve*, the OP-1's *DNA* and Mutable Instruments Plaits' *particle* and *swarm* models
(their code is MIT-licensed; none of it is used here). The saw events follow [`super`](../super/README.md), the
sine table and its one-instruction multiplies [`modal`](../modal/README.md), log2 and the closed-form envelope
[`vowel`](../vowel/README.md) (all this project). The inharmonic ratios are a circular membrane's textbook mode
ratios (Bessel zeros). Original code, MIT licence, see [CONTRIBUTING.md](../../CONTRIBUTING.md).
