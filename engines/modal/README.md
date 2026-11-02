# modal

Struck and bowed resonating bodies for the bass, lead, arp and chord tracks: mallets, bars, bells, glass, drums and
bowed metal. Each voice is ten decaying partials whose frequency ratios, weights and decay times morph between six
structures (string, marimba, bar, membrane, bell, glass); a mallet strikes them, its hardness following the velocity,
or a bow keeps them sounding while the key is held. The OP-Z's own track filter (the filter and resonance knobs)
shapes the tone after it.

| Control | Function |
|---|---|
| P1 **structure** | a continuous morph through six anchors, 51 apart. **0 string**: harmonic partials 1, 2, 3 … 10, falling gently in level and decay (struck string, dulcimer, harp). **51 marimba**: a tuned wooden bar, 1 : 3.99 : 9.95 : 17.6 …, upper partials short and weak (marimba, xylophone, wood). **102 bar**: a free metal bar, 1 : 2.76 : 5.40 : 8.93 : 13.3 … plus two torsional modes (glockenspiel, vibes, celesta). **153 membrane**: an ideal circular membrane, 1 : 1.59 : 2.14 : 2.30 : 2.65 …, dense and short (tuned drums, tabla-like). **204 bell**: a minor-third church bell: hum 0.5 (rings longest), prime 1, tierce 1.18, quint 1.51, nominal 2, then 2.51 … 5.43 (bells, chimes, gamelan). **255 glass**: 1 : 2.32 : 4.25 : 6.63 : 9.38, each as a pair 0.3–0.5 % apart that beats slowly (wine glass, glass harp, crystal). Between two anchors every partial glides on a straight line in log frequency from its ratio at one to its ratio at the other, and its weight and decay are interpolated, so turning P1 under a ringing note bends the partials smoothly into the next structure (halfway sounds are inharmonic hybrids: 25 is a stretched, kalimba-like string, 178 a steel-drum-like plate). Mode 0 is always the note: the pitch never moves |
| P2 **damping** | two zones. **0–191 ring:** the fundamental's T60 is 0.08 s at 0 → 20 s at 191, exponential (0.3 s at 48, 1.3 s at 96, 6 s at 150); each partial's decay is that times its structure's factor, shortened further by a frequency damping that falls with P2 (a 1 kHz partial decays 3 × faster than the base at P2 0, 1.05 × at 191), so low P2 is dark and dead (a thud), high P2 bright and long. P2 also moves the mallet's brightness one octave either side of 96 (darker below, brighter above). **192–255 bow:** while the key is held, a sustained, slightly rough excitation (each partial's level follows its own low-passed noise, ×0.4–1.6) feeds every partial, from nothing at 192 to full at 255, with a darker spectrum than the strike (weights squared); the strike softens to 40 % on the way, so 255 is mostly swell. The bow lifts at note-off and the partials ring out (T60 20 s) under the release |
| velocity | level (linear: 64 is −6 dB); the mallet's hardness: a low-pass corner 12 dB/octave from 600 Hz at velocity 0 to 13.6 kHz at 127 (at P2 96), so soft notes are dark; the contact time over which the partials rise, 4 ms soft → 0.25 ms hard; a short click of noise at the strike (velocity squared, strongest on the membrane and marimba); the strike position, 0.22 → 0.15 of the length (each partial's weight × 0.4 + 0.6·\|sin(π · position · ratio)\|: hard notes are thinner). In the bow zone also the bow's level (×0.5 → 1) and how fast it takes hold (1.2 → 3.6 % of the way per block) |
| note-off | the OP-Z envelope releases the level as on every engine, and the bow lifts. Struck partials keep ringing under the release, as a mallet instrument does without its damper: short steps and a short release sound damped, a long release lets bells ring |
| attack, decay, sustain, release | the OP-Z envelope, applied to the level. The partials make their own decay, so the presets use attack 0, sustain 255; a slow attack fades a struck note in (pads), a lower sustain shortens the ring |
| filter, resonance | not used: they are the OS's own track filter, which runs after the engine. Presets leave it neutral (128, 0) |

How it works: each partial is a sine (a 256-entry table, linearly interpolated, peak error −80 dBFS) with its own
phase increment (the note's pitch word × its ratio, Q16) and amplitude. The amplitude decays exactly per block (a
multiplier 2^−x per block, computed from the T60 with a table and a short series, to 2·10⁻⁷) and ramps linearly
across the block, so a 10 ms decay has no zipper. The strike sets each partial's level once: weight × strike position
× mallet low-pass (1 / (1 + (f/fc)²)) × velocity, normalised so the levels sum to a fixed total; the bow pulls each
level towards its own target. There is **no recursion**, so no setting of P1, P2 or pitch can make a partial grow,
and the amplitudes sum to at most 0.62 of full scale (strike and bow), the click adds at most 0.09: the output is
bounded by construction, no limiter is needed. Partials above 0.40 fs fade out and are silent from 0.45 fs; partials below 30 Hz
fade and are silent below 15 Hz (the bell's hum on very low notes); partials that decayed below −90 dBFS are skipped
and cost nothing, and a released voice that has gone silent is freed at once.

The coefficients are recomputed only when something changes: the ratios when P1 moves (every block), the decays when
P1 or P2 move (at most every other block, so an LFO on them costs half) or the pitch drifts 3 %; vibrato, bends and
glides retune the partials at block rate (the block's mid-point pitch). A note's decays are computed in its second
block (1.45 ms later), which keeps the first block, with the strike, under the budget. The envelope is applied at
block rate, as the host reads it: decay and release exactly per block (coef⁶⁴), sustain and attack as the host's
step taken per block (attack in two half-block steps), linear in between; the fade of a stolen voice per sample.

## Presets

Five per synth track. `slot` in `presets.json` is the track (0 bass, 1 lead, 2 arp, 3 chord); index 0 of each (in
**bold**) is the sound a track gets when it switches to modal, the others are on the preset keys.

| Track | Preset | Octave | P1 | P2 | A | D | S | R | Level | Sound |
|---|---|---|---|---|---|---|---|---|---|---|
| bass | **mallet bass** | −2 | 51 | 110 | 0 | 0 | 255 | 35 | 210 | round marimba bass, wooden attack |
| bass | string bass | −2 | 8 | 125 | 0 | 0 | 255 | 40 | 205 | struck string bass, harmonic and warm |
| bass | tom bass | −2 | 150 | 70 | 0 | 0 | 255 | 25 | 215 | short tuned drum, thump and click |
| bass | bowed bass | −2 | 20 | 240 | 30 | 0 | 255 | 50 | 200 | bowed string bass, swells and sustains |
| bass | metal bass | −2 | 110 | 130 | 0 | 0 | 255 | 35 | 205 | clangy metal-bar bass |
| lead | **vibes** | 0 | 102 | 170 | 0 | 0 | 255 | 90 | 190 | vibraphone-like metal bar, long ring |
| lead | glass lead | 0 | 250 | 160 | 0 | 0 | 255 | 70 | 190 | bright struck glass with beating pairs |
| lead | glass harp | 0 | 255 | 245 | 45 | 0 | 255 | 80 | 185 | bowed glass: a glass harmonica |
| lead | steel drum | 0 | 175 | 140 | 0 | 0 | 255 | 50 | 190 | between membrane and bell: steel-pan-like |
| lead | dulcimer | 0 | 0 | 150 | 0 | 0 | 255 | 70 | 190 | hammered string |
| arp | **marimba** | 0 | 51 | 120 | 0 | 0 | 255 | 45 | 195 | marimba, short and woody |
| arp | glock | +1 | 102 | 180 | 0 | 0 | 255 | 80 | 185 | glockenspiel, bright and ringing |
| arp | kalimba | 0 | 72 | 115 | 0 | 0 | 255 | 45 | 195 | between marimba and bar: thumb-piano tines |
| arp | celesta | +1 | 88 | 150 | 0 | 0 | 255 | 60 | 190 | soft metal bars |
| arp | crystal | +1 | 238 | 182 | 0 | 0 | 255 | 90 | 185 | small glass bells, shimmering |
| chord | **bell pad** | 0 | 204 | 240 | 60 | 0 | 255 | 120 | 150 | bowed bells: slow swell, sustains while held |
| chord | chimes | 0 | 204 | 185 | 0 | 0 | 255 | 120 | 155 | struck bells that ring on |
| chord | bowl | 0 | 228 | 226 | 50 | 0 | 255 | 130 | 150 | singing bowl: bowed, between bell and glass |
| chord | tine keys | 0 | 30 | 150 | 0 | 0 | 255 | 70 | 160 | struck, slightly stretched string: an electric-piano-like tine |
| chord | gamelan | 0 | 182 | 168 | 0 | 0 | 255 | 90 | 155 | inharmonic metal plates |

Every preset: filter 128, resonance 0, pan 128, sends 0, no LFO (the arp track keeps its own LFO values when a preset
applies). Chord presets sit lower in level because several voices add up. Names fit the OP-Z's 11 characters.
`python3 tools/opz.py presets check engines/modal` reports 20 presets, 0 errors, 0 warnings. Not yet tuned by ear on
the device.

## Facts

| | |
|---|---|
| Plug | 61017, `trackmask` 0xF0 (bass, lead, arp, chord), engine and plug version 1 |
| Memory slot | 52 (10 KB class), 6,948 of 10,240 bytes on Blackfin: 6,684 of code, tables and data, 264 of `.bss` (the shared 65-word partial buffer and the callback pointer). Tables: 1 KB sine, 692 B structure anchors, 260 B decay table |
| State | 268 B of 280 per voice: ten partials × (phase, amplitude, level, decay, ratio, bow roughness) and the strike's click and rise. **No voice pool**: everything a voice needs fits its state. `memRequest` 0 |
| Partials | 10 per voice at every pitch: those above 0.45 fs (high notes on bars, marimba) or below 15 Hz are silent and skipped |
| Tuning | exact by construction: mode 0 runs at the pitch word itself; the other ratios are exact Q16 values at the anchors (P1 0, 51, 102, 153, 204, 255) and within 0.01 % (0.17 cent) between them |
| Decay | the fundamental's T60 as P2 sets it, less the frequency damping: A3 string measured 0.27 / 1.20 / 6.0 / 20.0 s at P2 48 / 96 / 150 / 191 (set 0.32 / 1.28 / 6.1 / 19.9 s before the damping, which shortens them 1.17 / 1.07 / 1.02 / 1.01 × at 220 Hz) |
| Level | −10.4 to −15.9 dBFS peak per voice at velocity 127 (median −12.3, OPZ_VOICE_PEAK −12) over the six structures and C1–C7; the string sits ~1.5 dB under the others (its energy is spread over ten partials). Velocity 60: −19.6 dBFS. Bowed sustain (P2 255): −20 to −24 dBFS RMS, steady; at P2 230 ~4 dB lower; the glass beats ±5 dB |
| CPU | Blackfin simulator ([cycle-model.md](../../research/cycle-model.md), `python3 tools/opz_cycles.py engines/modal`), cycles per sample per voice, mean of 100 blocks: **all ten partials ringing 135, bowed 149**; short decays (P2 40) 58–101 as partials die and are skipped; high notes 108. 19 voices bowed 151 (9,645 cycles/block/voice); with P1 and P2 moving every block 140–150 (19 voices 150); 19 voices with P1 gliding over the whole range 173. The most a voice costs is its note's first block (the strike): 12,900–13,500 cycles (201–210 per sample); 13 notes (every synth voice) starting in the same block ~173,000, 19 ~253,000. Note-on 87 cycles. Every case is under the 218 budget in its worst block |

The partial loop (10 cycles per partial per sample) and the output loop (4 per frame) are hand-scheduled Blackfin
assembly: the table and accumulator loads and the stores issue in parallel with the arithmetic. The C versions next
to them are the same arithmetic; building the device engine with `MODAL_C_LOOP` defined (a `#define` at the top of
`modal.c`) gives bit-identical output in every benchmark case (`opz_cycles.py --dump` / `--compare`), and the host
build (opzsim) uses the C. `gen_tables.py` writes `modal_tables.h`; `tools/tests/test_modal.py` fails if it is stale.

Simulator checks (opzsim, `tests/`): eight golden scenarios (the six structures and the halfway points; a held chord
with P1 and P2 sweeping and bends; bowed glass harp and bell pad; mallet velocities on the arp; a bass line down to
the lowest key; every synth track at once with steals; the knobs at both ends with C0 and C9; P1 and P2 moving every
block while six chord voices start, steal and release); all pass the host's state guard and "adds into out" probe, and
every voice is freed after its release. Sweeping P1 and P2 under ringing notes and bending show no clicks.
`tests/fuzz.c` (UBSan with array bounds; how to run it is in its header) drives 13 voices for 10⁶ blocks with random
note-ons (velocity 0–255, params NULL), steals, note-offs, re-inits, pitch words 0 … 2³² − 1, slopes, P1/P2 words
beyond the host's range, a fake envelope jumping between every stage, and knob jumps every block, then holds a grid of
P1 × P2 × pitch for 40 blocks each: no sanitizer report, no voice above 0.75 FS, the amplitude sums within their
bound everywhere.

## What to listen for on the device

- **Structures:** hold a note and sweep P1 slowly from 0 to 255: string → marimba → bar → drum → bell → glass, the pitch
  steady, the partials gliding (no steps, no clicks), inharmonic hybrids in between. At 255 a held note should beat
  slowly (faster on high notes).
- **Mallet:** play vibes or marimba softly and hard: soft should be dark and round, hard bright with a small click.
- **Damping:** P2 from 0 (a dead thud) to 191 (rings for many seconds, brighter); one-step notes ring on under a long
  release, stop under a short one.
- **Bow:** P2 above 192 on a held note: it should swell and sustain with a slightly rough, breathing tone, and ring out
  when released (glass harp, bell pad, bowed bass). Move P2 down below 192 while holding: the bow fades and the note
  decays.
- **Range:** bell or bar on the bass track at octave −2 and −3 (the bell's hum drops out below ~30 Hz, the note stays);
  glock and crystal at the top octaves (high partials drop out, no aliasing whistle).
- **Knobs moving:** an LFO on P1 or P2 (and both knobs turned fast) while chords play and steal voices on all four
  tracks: no crackle, no crash (the macro v3 situation).
- **CPU:** modal on every synth track with full chords (generous chords on), bell pad or glass harp held: listen for
  crackles or dropped release tails (the runbook's step 11).

## Credits

Original code. The design follows the modal synthesis literature (Adrien's modal representation; Cook's modal bar and
PhISEM models; the textbook mode ratios of the free bar, the circular membrane and the church bell from Fletcher and
Rossing, *The Physics of Musical Instruments*), in the spirit of the modal resonators of Mutable Instruments' Rings,
Elements and Plaits (MIT licence). No code from them is used: they run banks of resonant filters, this engine runs
decaying partials. The exp/reciprocal helpers follow engines/pluck (same project). No TE code. MIT licence.
