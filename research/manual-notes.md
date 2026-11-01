# Notes from TE's OP-Z user guide (firmware 1.2.45)

Our own summary of TE's official manual for the firmware we study (`OP-Z_manual_1.2.45_final.pdf`, 81 pages; the PDF
page numbers equal the printed ones). The manual is TE's copyrighted work and is **not** in this repository: this page
paraphrases it, quotes at most a phrase, and cites pages as **p.N**. Evidence label **[manual]** = stated or drawn in
the manual; **[inferred]** = our reading of it; anything else keeps the labels of [CONTRIBUTING.md](../CONTRIBUTING.md).
Several of the manual's tables (parameter chart, step components, punch-in effects, MIDI) are pictures; they were read
from the rendered pages.

The manual describes the device as a *user* plays it, with TE's factory plugs. It says nothing about user plugs, SysEx,
engines or `patch.json`, so it never contradicts our ABI directly; it does correct several of our assumptions about
**how a person reaches** a sound, a preset or a parameter (see [What it changes](#what-it-changes-in-our-docs)).

## Contents

1. [Surface and vocabulary](#1-surface-and-vocabulary)
2. [Sound slots and presets](#2-sound-slots-and-presets)
3. [Parameter pages and the 18 parameters](#3-parameter-pages-and-the-18-parameters)
4. [Tracks: drums, synths, FX, tape, master](#4-tracks-drums-synths-fx-tape-master)
5. [Sequencing: steps, components, punch-ins, length, chains](#5-sequencing-steps-components-punch-ins-length-chains)
6. [MIDI and CC](#6-midi-and-cc)
7. [Content, samples, modes, startup, reset, safety](#7-content-samples-modes-startup-reset-safety)
8. [How to test an engine on the device (cheat sheet)](#8-how-to-test-an-engine-on-the-device-cheat-sheet)
9. [What it answers, what it changes, what still needs hardware](#9-what-it-answers-what-it-changes-what-still-needs-hardware)

## 1. Surface and vocabulary

| Thing | What the manual says | Page |
|---|---|---|
| Hierarchy | 10 projects × 16 patterns × 16 tracks × 16 steps; 14 saved pattern chains per project; 10 mute groups per project; 14 step components; 24 ticks per step | 12 |
| Controls | Four endless colour dials (green, blue, yellow, red) with an LED each; index buttons *project*, *mixer*, *tempo*, *screen*; *track*, *shift*, *rec*, *play*, *stop*, −/+ (octave and nudge); the top row of 16 buttons is track select, pattern select and step button, depending on what is held | 12–15 |
| Keyboard | Two octaves, **24 keys: 14 white, 10 black**. The black keys are the **value keys 1…0**, the white keys the **component keys** (the sixteen-button row carries the icons). Per our key map the 24 keys send MIDI 53–76, F3 first, so black keys 1…0 are F♯, G♯, A♯, C♯, D♯, F♯, G♯, A♯, C♯, D♯ and the white keys F G A B C D E F G A B C D E [manual drawing p.23; MIDI numbers from [Q-14](OPEN_QUESTIONS.md)] | 15, 23 |
| LED values | A dial LED shows a level by brightness; **neutral / default is green at 50 %**; static choices show as colour segments | 13 |
| Index lock | triple-click an index button to lock that screen; any index button leaves it | 13 |
| Hold vs tap | Almost everything is "hold a button, then press another". *Shift* differs between **press-and-release** (next parameter page) and **hold** (punch-in effects on the keys, temporary tweak on the dials: the value snaps back when shift is released) | 14–15 |
| Pitch bend | The bend control bends the selected audio track and also works on **tape** and **master**; with a lit step held it sets that step's velocity. FX1/FX2 are not mentioned | 16 |
| Panic | `stop` while stopped ends all notes; `stop` twice also clears audio buffers; `track`+`stop` fades hanging notes of the active track, twice kills it | 15, 17, 25 |
| Mic | built-in microphone on the left side, switched on by tilting the unit; a headset mic overrides it. A setting turns tilt activation off | 55, 73 |

## 2. Sound slots and presets

Every track has **10 sound slots**, matching the ten value keys (p.62 for the file layout, p.66 for the app's
configurator, which shows them as the "round slots").

| Do | Gesture | Page |
|---|---|---|
| Select a track | hold **track**, press the track's button in the top row (a white LED marks the active track while track is held) | 14 |
| Select a plug (sound slot 1…10) | hold **track**, press a **black** (value) key. A **lit** slot holds a plug (sample kit, synth engine or effect). On FX1/FX2 the chosen effect **blinks**; one effect per FX track | 25, 39 |
| Select a preset of the active plug | hold **track**, press a **lit white** key | 25 |
| Randomize | hold **track**, press **rec**: a new random sound (improved for chord and FX tracks in 1.2.31) | 25, 5 |
| Store a preset | hold **track**, hold a **white** key for **two seconds**: the current 18 values are stored on that key. **At most 14 presets per plug** | 25 |
| Remove a plug from a slot | 1.2.28's changelog adds "hold track + slot" for factory plugs; in sample mode a 3-second hold on a user slot removes the pack. Treat a long hold of track + slot key as dangerous, and **tap** | 6, 50 |
| Kill sounding notes | track + stop (see above) | 25 |

Consequences for us:

- **Presets live on the 14 white keys.** Lit white keys show which presets exist, so after a `presets push` the keys
  tell you at a glance how many our plug has — no `watch` needed. Black keys are *slots*, not presets.
- Our model has 14 presets **per synth track per plug** (`slot` = bass, lead, arp, chord); the manual's "14 per plug"
  is the same limit as the user sees it on one track [inferred].
- The mapping white key → `index` is not spelled out. The natural reading is the white keys in order, F3 = 0 … E5 = 13
  [inferred]; [presets.md](presets.md) inferred `(note − 53) mod 24` from decompiled code, which would put presets on
  black keys too. **Test H-3 decides it.**
- Presets are meant to be **stored by the user on the device** (2-second hold). That is a legitimate way to make a
  tuned sound survive restarts without writing `config/userPresets.dat` ourselves [inferred from the manual plus
  presets.md's persistence table; test H-2b below].
- A track that changes plug restores whatever the pattern last had for that plug, else applies a preset: the manual
  does not describe this, so presets.md's offline finding stands untested.
- "Randomize" will randomise our plug's P1/P2 and the other 16 values: a quick way to hear an engine across its range,
  but it overwrites the track's sound in the active pattern.

## 3. Parameter pages and the 18 parameters

Each track has up to **four pages**; **press and release shift** to move to the next one, the dial LEDs change
colour (p.14, p.20). Selecting another track resets to the first page unless the `disable_param_page_reset` setting is
on (p.73). The chart on **p.21** (drawn) gives:

| LED | Drum tracks 1–4 (green / blue / yellow / red dial) | Bass, lead, chord | Arp | FX1, FX2 | Tape | Master |
|---|---|---|---|---|---|---|
| **white** (page 1) | pitch · reverse · filter · resonance | **P1 · P2 · filter · resonance** | P1 · P2 · filter · resonance | **P1 · P2 · filter · resonance** | speed · fine tune · filter · resonance | chorus · drive · filter · resonance |
| **green** | attack · decay · sustain · release | same | same | — | — | — |
| **purple** | LFO amount · speed · target · shape | same | *(blue)* arp speed · pattern · style · range | — | — | — |
| **orange** | FX1 send · FX2 send · pan · level | same | same | — | FX1 send · FX2 send · pan · level | — |

Reading it with the changelog:

- **The chart shows only one page for FX1/FX2, but the same manual says FX tracks have an LFO (1.2.12, p.7), that
  sends can be edited from an FX track (1.2.38/1.2.39, p.5, p.39), that shift pages through FX parameters (p.39), and
  that all FX plugs got a "dry mix" setting (1.2.26, p.6).** Our hardware test already drove FX1 with pan as dry/wet
  and level as output [proven, see [engine-abi.md](../sdk/docs/engine-abi.md#renderfx--effects-once-per-fx-track-per-block)].
  Which dial and page the user reaches pan and level by on an FX track is therefore **unconfirmed** (checks below).
- The first page is "the sound": P1, P2 and the **OS filter and resonance**. They stay the same names for every
  plug; the *plug* only renames P1/P2. TE's own labels for the engines are in the reference pages (p.71–72): for
  example `crush`, `delay`, `dist`, `rymd` all use "amount / cutoff", `reverb` "decay / tone" and says the **piano keys
  set the pre-delay time**, `chorus-80` "speed / depth".
- Our storage order of the 18 values (`params[]`, `$0E`) is **not** the order the manual's tables use. For MIDI the
  order is P1, P2, filter, resonance, A, D, S, R, LFO depth, speed, target, shape, FX1, FX2, pan, volume, portamento,
  note style (CC 1…18, p.59) [manual]; ours is P1, P2, A, D, S, R, FX1, FX2, filter, resonance, pan, level,
  portamento, LFO depth/speed/target/shape, note style ([engine-abi.md](../sdk/docs/engine-abi.md#4-parameters)). Don't
  mix them when writing CC-based tests.
- **Units.** The dials are endless; the manual gives units only where they are shown: tempo 40–200 BPM (p.35); note
  length 1/64 note … one bar, fully clockwise = **drone** (p.23); quantize 0–100 % (p.23); portamento 0 (none) …
  100 (very slow) (p.23); swing 50 % = none, with a dead zone at the centre (p.36, p.5); MIDI CC values 0–127
  (p.59). Everything else is a bare 0–127 / LED scale in the manual; our bytes are 0–255 [notes].
- **Filter.** The manual describes the *master* filter as a combined high-pass/low-pass with a neutral centre
  (p.44). It does not describe the track filter; our hardware finding (bipolar, neutral at the centre, on instrument
  and FX tracks) stands.
- **Envelope (green page).** Attack, decay, sustain, release in that dial order on synth **and drum** tracks (p.21).
  On drums our ABI notes ([engine-abi.md](../sdk/docs/engine-abi.md#the-drum-envelope-updateparametersahd)) that TE's sample
  player reads these four values as start, attack, hold and decay: the label the user sees there is "attack, decay,
  sustain, release". An engine that follows the OS drum envelope should document its own meaning.
- **LFO (p.22, drawn).** *Depth* is bipolar: the centre does nothing (LFO off), clockwise adds, counter-clockwise
  subtracts; the dial is exponential since 1.2.17 (p.6). *Rate*: left of centre eight tempo-synced values — 1/64, 1/32,
  1/16, 1/8, 1/4, 1/2, 1/1, 2/1 — right of centre a free rate. *Destination*: **eight targets** — P1, P2, filter cutoff,
  filter resonance, attack, pitch, pan, volume. *Shape*: twelve, the first six free-running (sine, triangle, square, saw,
  random, gyro/accelerometer), the last six **retriggered by every note** (bell, triangle, square, saw, random, single
  saw). There is no LFO on the arp track (its page is the arpeggiator, p.42).
- **Temporary tweaks.** Shift + a dial changes a value only while shift is held (p.15). A "slight FX A send" builds up
  during such tweaks unless `temp_param_add_fx_a` is off (p.7, p.73).
- **Parameter locks.** Hold a lit step and turn a dial; hold **rec** and turn a dial while playing to record the
  movement (p.18–19). Clear a track's locks: hold rec + stop until all steps light; one step's locks: hold the step +
  stop (p.18, p.8).

## 4. Tracks: drums, synths, FX, tape, master

### Drum tracks 1–4 (kick, snare, perc, sample) — p.38, p.52

- Each is **sample based with 24 sounds across the keyboard** ("a kit", OP-1 drum format); four kits make a *sample
  pack*. Polyphony is **two notes per step**.
- Note styles (hold track + blue dial, p.23): **retrig** (green), **mono** (blue), **gate** (yellow), **loop** (red). The
  mono style for drum tracks arrived in 1.2.31 (p.5). Our engines receive this as `params[17]` and the OS decides
  voice stealing; `kit` ignores it today [not checked].
- The octave buttons shift the keyboard on every track since 1.2.12; drum tracks got the full octave range in 1.1.23
  (p.7, p.8). The manual gives no octave limits.
- Sample parameters on drums (only in sample mode, p.52): start, end, pitch, gain, direction, playmode (gate, trigger,
  loop). Source files are up to 12 s; the drum sampler is OP-1 compatible.

### Synth tracks 5–8 (bass, lead, arp, chord) — p.39, p.42

| Track | Polyphony stated | Notes |
|---|---|---|
| bass | **monophonic**, one note per step | main source for the master key analysis |
| lead | **three** notes per step | any note style |
| arp | **monophonic**: notes on one step are arpeggiated | no LFO; speed 8 settings, **fully left = arp off** (plays as a normal track), pattern manual/up/down/up-down/down-up/random, style (6 rhythms), range |
| chord | **four** notes per step | `generous_chords` in `general.json` raises the voice count to **six** (notes per step stay four) |

Note styles for synth tracks: **poly** (green), **mono** (blue), **legato** (yellow) (p.23). Portamento (hold track + red
dial) applies to them; a mono arp with portamento gives a sliding arp (p.42). These are "notes per step". Our docs' voice
counts (bass 2, lead 3, arp 2, chord 4, drums 2; [02](architecture/02-sound-engine-host.md)) are the OS table and
include a voice still releasing, so the stress test's "bass (2 notes)" is two *voices*, not two notes a person can sequence
on one step.

All synth tracks can load any synth engine or an OP-1 format sample (p.39, p.53; sample length 6 s). The manual lists
**12 synth engines** — bow, cluster, digital, electric, saw, shade, sample, uranus, volt, analog, organ, ep — and
**6 FX engines** — crush, delay, dist, rymd, reverb, chorus-80 (p.71–72). None of our four unreleased TE names (Lead, DxFm,
Fm, Swirl) appears, consistent with them being unreleased ([Q-11](OPEN_QUESTIONS.md)).

### FX1 / FX2 — p.39

- The two FX tracks are where the **send effects** are selected and controlled. Assign: on FX1 or FX2, hold track + a lit
  black key; **one effect per FX track**; more effects come in through the configurator (app) (p.39, p.66).
- Parameters can be set "using the musical keyboard as well as by turning the dials" (p.39). The reference page
  shows what the keys do for TE's reverb (pre-delay time). **So notes sequenced on FX tracks are control input for the
  plug, not audio notes**; what they do depends on the effect [manual + our `swirl`, `resonator`, `glue`].
- **Effect audition:** hold **shift** while on an FX track and play the keys: the keys play the previously selected
  drum/synth track so you can hear it through the effect (p.39). Plain key presses go to the effect.
- **Send levels:** normally set on the source track (orange page, FX1 send = green dial, FX2 send = blue). From an FX track:
  hold **shift**, press the source track's button — a tap toggles the send on/off, a one-second hold lets you set the
  level with the green dial (p.39). Changelog: track gain is applied before the FX sends (1.2.5, p.7); muting a track's
  *audio only* keeps it going to the effects and tape (p.34, 1.1.12 p.8); a pop with full FX send was fixed (1.2.38, p.5).
- FX plugs have a **dry mix** setting since 1.2.26 (p.6). We already reproduce it as pan = dry/wet.
- `delay`, `reverb`, `dist`, `crush` etc. are what the manual shows; no one-page description of user plugs.

### Tape — p.40, p.43

An audio buffer that records constantly during playback, for tape tricks and beat-repeat loops. Dials: speed (coarse:
regular speed at the left, faster to the right, then back down to a full stop), fine speed (centre = normal, left = half,
right = double), filter, resonance. **White keys** choose where in the buffer playback begins; **black keys** choose the loop
length (1 = shortest … 0 = longest). Sending CC to P1 and P2 together can scratch. Pitch bend gives a live tape stop (p.16).
In our terms: tape is an effect plug (`tapetwo`) whose notes are control input; the tape ducker (`sequencerSetValue` key 1) is
probably what the "duck" punch-in uses [inferred].

### Master — p.33, p.40, p.44–45 (the master track is fixed, not a plug slot)

*Corrected 2026-10-18 after the user's hardware observation: on the device you cannot choose a master effect. The manual
agrees.* Everything it says about master:

- **Role:** transpose selected tracks and build harmonic chord progressions (p.40, p.44). Nothing else; no sound of its own.
- **Dials (one page, p.21 and p.44):** green **chorus** (also adds a slight stereo effect), blue **drive** (subtle overdrive on
  the mix), yellow and red a combined **high-pass / low-pass filter with resonance**, neutral in the middle. That is a
  *fixed* chain; the chart gives master no P1/P2 names and no envelope, LFO or send pages. P1/P2 are chorus and drive. Our
  slot map's single master plug (39, "master chorus") is the engine behind them, so the green and blue dials are its P1/P2
  [inferred].
- **No selection gesture:** the "assign an effect" gesture (hold track + a lit black key) is described only for FX1 and FX2
  (p.39); the master page, the slot text and the parameter chart never offer a choice, and the FX engine list (p.72) names only
  crush, delay, dist, rymd, reverb and chorus-80, all FX-track effects [manual]. Our `trackmask` bit `0x800` for master exists in the plug format, but no user gesture was found that uses it.
- **Separate, also fixed:** the **mixer** has its own master compressor ("punch" dial) and master gain (p.33). It is not a plug.
- **Notes on master are musical commands, not triggers.** The OP-Z analyses bass, lead, arp and chord notes (bass counts
  most) to find the pattern's key and mode; keys on master change key or mode and **transpose only the tracks marked** with
  shift + track buttons (lit = included, drums allowed for fills); −/+ moves the transposition by octaves (p.45). The
  guide calls the result *chord progressions*; it does not use the term "magic chord" (our code's `magicChordUpdate`
  is that feature) [inferred].
- **Sequencing master:** program it like any track; its **own length**: track + shift + a value key (4 = four bars)
  (p.45). Note styles, set with track + blue dial: **latch** (green: quantized to whole steps, no micro timing, notes held
  to the end of the bar) and **free** (blue: micro timing, no latching) (p.45, 1.2.12 p.7). 1.2.45 lets the module track
  transpose too (p.5). Pitch bend works on master (p.16).
- **Consequence for our effects:** a plug on master would have to *replace* the chorus/drive/filter pair TE shows, and the manual
  gives no way to do that. Treat "put `glue` on master" as **not available to users** until a hardware check shows otherwise
  (tap track on master, then black keys 2…0 and 1: does anything light or switch?). `glue`, X-13 and X-14 should be used on
  FX1/FX2 (where note ducking *is* documented as control input, p.39), see [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md) Q-19/Q-20.

### Other control tracks

*Perform* (track 13) applies punch-in effects to all tracks at once and can record them (p.40, 48); *module* (14) is the
I/O track: expansion modules, a 16-CC MIDI track when no module is fitted, and **input selection** (shift + 1 mic,
2 headset, 3 USB, 0 monitor) (p.40, p.54); *lights* (15) DMX; *motion* (16) visuals and photomatic (p.41, 67–68).

**The external signal (mic, headset, USB audio, line module) has filter, LFO, pan and volume and is "routed through the
effects, the same way an instrumental track is"** (p.54), and the mic mode buttons choose FX1, FX2, both or none (p.55). So
an FX plug can already process the mic and line input without any special support — [Q-13](OPEN_QUESTIONS.md) / F-17.

## 5. Sequencing: steps, components, punch-ins, length, chains

### Steps, length, offset (relevant to F-18) — p.17–19, p.24

- A track holds **16 steps**; each step has 24 ticks (micro timing: hold a step, press −/+ to move it one tick).
- **Step count** (1–16): hold track + shift, press the top-row step button; the track loops over that many steps.
  Each track has its own count (p.24). Each track also has its own **step length multiplier**: hold track + shift, press a
  value key 1…0; "9" makes the track **16× longer**, **0** makes it trig driven (advances only on a trig in the oplab module
  or a jump component of value 0). A longer step lowers the timing resolution of its notes (p.24).
- Move all notes of the track one step left or right: hold track + −/+ (p.15, p.24). Quantize live-recorded notes
  per track 0–100 % (hold track + yellow dial, p.23).
- Editing basics (p.18): press an empty step to add the last played note; press a lit step to clear it; hold a lit step and
  press keys to change its notes; hold a lit step and press another step to set its length; hold a step + pitch bend for
  velocity; hold a lit step while stopped to preview and copy it; press an empty step to paste. Clear a track: hold track +
  stop until all steps light.
- Recording (p.19): hold **rec** while playing to record keys; rec + play locks record mode (armed when stopped); rec held
  while stopped = step-by-step entry; rec + dial = parameter lock; − with rec = subtractive recording.

### Patterns, chains, projects (p.29–32)

- Select a project: hold **project**, press a value key 1…0 (play first to delay the change to the end of the bar). Select a
  pattern: hold project, press a top-row button 1–16; while playing it switches **instantly and keeps the step position**.
- **Chain**: hold project + **play**, keep holding project and press **up to 32 patterns**; hold project + a white key
  saves the active chain (14 slots) (p.30). **Our F-18 text says
  chains hold up to 31 patterns**; the manual says 32 — check on the device before relying on either.
- A pattern can be copied (project + shift, then the target pattern(s)), its *settings* copied (project + shift twice), a
  *track* copied (project + shift three times), cleared (project + stop until the bar fills), or the whole project cleared
  (project + stop + shift); project + rec bounces ≤ 10 s to disk (max 5 bounces); project + value key **saves the project to
  that slot** (p.30–31). The same chord "project + value key" selects when tapped and copies when held (p.29, p.31): tap to
  select.
- **Saving is automatic** by default; hold project + track for a few seconds (or hold project while powering on) for
  **manual save mode**, where a project is saved by holding project and holding its value key (p.32).
- Pattern changes by MIDI program change and CC are in §6.

**What this means for F-18:** the manual offers no 64-step track. "Step count 16 × length 4" is four bars of 16 *slower*
steps, not 64 steps. All tracks also share the pattern switch ("all tracks keep their step position"), so the chain
emulation in features.md is still the only route; only the chain length (31 vs 32) differs.

### Step components (p.27–28, chart drawn)

Available on audio tracks 1–8 (p.27). Add: hold **shift**, tap the target steps (they turn green), still holding shift press a
white key (the component) and then a value key 1…0 for its setting; release shift. A tap on an applied component's white key
removes it; shift + stop clears the track's components. **Fourteen components**, in key order: pulse, pulse hold, multiply,
velocity, ramp up, ramp down, random, portamento, sweep, tonality, jump, parameter spark, component spark, trigger
spark. Settings of note: *multiply* ×1…×8 / broken chord / quantize; *velocity* −4…+3 / mute / random; *ramps* 2–6 steps over
1 or 3 octaves; *jump* start, 2/4, 3/4, 4/4, forward, back, random, stay, align to the global track, **gate step**; *sparks*
fire every 1…8 loops, random, or reset the counter.

### Punch-in effects (p.48, chart drawn)

Hold **shift** and hold keys: the **low octave acts on the current track, the high octave on the current track group** (drum
or synth). Twelve effects: duck (F), filter sweep (F♯), loop 1 (G), stereo (G♯), loop 2 (A), pitch (B♭), follow/echo (B),
ramp up / fill 1 (C), short (C♯), ramp down / fill 2 (D), long (D♯ — the chart prints C♯ twice, a typo), random (E). Record
them with rec or record lock; they land on the **perform** track and are editable like notes. They apply to tracks 1–8 only;
**shift on an FX track is the audition gesture instead** (§4). A bug that made the punch-in *loop* effect load the CPU was
fixed in 1.2.31 (p.5).

## 6. MIDI and CC

- Every track sends and receives MIDI, each on its own channel; the outgoing CC number per parameter and the channel are
  configurable (p.57–58). On-device settings: hold **tempo + screen**, then press 1 (channel one to active track), 2
  (incoming), 3 (outgoing), 4/5 (clock in/out), 6 (alt program change), 7 (echo), 8 (program change); track 1–16 mutes MIDI,
  track held + green dial sets the channel (p.57). The same keys exist in `midi.json` (p.58). Clock in switches the unit to
  external sync (all track LEDs 1–16 blink green in groups of four, p.36).
- **Incoming CC table (p.59–60, drawn):** absolute CC 1…18 (0–127) for the 18 sound parameters on track = channel 1–16, in
  the order *P1, P2, filter, resonance, attack, decay, sustain, release, LFO depth, LFO speed, LFO target, LFO shape, FX1
  send, FX2 send, pan, volume, portamento, note style*; relative twins on CC 32…49 (values 1 and 127 step up/down). System CCs:
  50 track gain, 51 relative gain, 52 reset gains, 53 mute, 54 audio mute, 55 mute group, 56 tempo, 57 swing, 103 pattern
  select (value 0–15, channels 1–10), 16 next / 17 previous pattern. Track CCs: 60 step count, 61 step length, 62 quantize, 63
  note length. UI CC 102: active track (0–15) and parameter page (0–3). Program change 1–10 selects patterns 0–15; 1, 2 select
  0–127; clock, start, stop, continue and pitch bend are handled; song pointer and active sensing are not.
- The module track is a 16-CC MIDI track when no module is attached (p.40). CC "parameter 1 and 2 together" gives scratching
  on tape (p.43).

## 7. Content, samples, modes, startup, reset, safety

| Topic | What the manual says | Page |
|---|---|---|
| Power | the yellow knob on the left: clockwise past a click = on (rainbow LEDs, startup sound), counter-clockwise past the click = off. There is **no soft restart** | 10 |
| Data | everything is stored on the fly; back up in disk mode now and then | 10 |
| Battery | hold *screen*: track LEDs 1–16 show the level. Hold *screen* + the last key (E) to **disable USB charging** (kills USB ground noise); the same combination with "trigger spark" in 20.1 | 10, 56 |
| **Content mode** | hold **track** while powering on: all track LEDs green, the unit appears as a USB disk; eject (or press *play*) to apply and it restarts. Holds samplepacks (10 slot folders per track 1–8), projects, bounces, `midi.json`, `dmx.json`, `general.json`, and the helper texts `how_to_import.txt` and `how_to_dmx.txt` | 61–63 |
| Importing sounds | OP-1 `.aif` drum or synth sample format; one pack per slot folder, up to **24 MB** of sample data; rejected files appear in a `rejected` folder at the next content mode | 62–63 |
| Backup / restore | copy files off and back in content mode | 62–63 |
| **Upgrade mode** | hold **screen** while powering on (kick LED blinking white, dial LEDs white); copy the firmware file onto the disk, eject, do not power off during the update | 63 |
| **Factory reset** | in upgrade mode hold **screen + stop** for a second; ends with a blinking white LED and four green LEDs; **all custom user content is removed**. 1.2.14 fixed user-plug import failing right after a factory reset | 64, 6 |
| Manual save mode | hold **project** while powering on | 32 |
| `general.json` | `backlit_keys`, `disable_headphone_db_reduction`, `disable_microphone_mode`, `disable_param_page_reset`, `disable_start_sound`, `disable_track_preview`, `generous_chords`, `latch_notes_with_shift`, `temp_param_add_fx_a`, `legacy_input_select` | 73 |
| Modules | oplab (CV/gate/MIDI/trig), rumble, line (line in/out, effect loop, headphone cue) on the four top ports; never feed phantom power into the audio jack | 11, 74–81 |
| App | pairing over Bluetooth, the *configurator* (drag plugs into the ten slots, commit), photomatic, motion, MIDI setup, file transfer | 65–70 |

No chapter describes importing **engines** or plug definitions: the manual knows only sample packs and the
configurator's slot arrangement. Whether content mode accepts engines ([Q-10](OPEN_QUESTIONS.md)) is therefore
undocumented; the `how_to_import.txt` on the content disk may say more (it is on the device, not in the manual).
The firmware changelog does mention "automatically add and assign the new engines to free slots" (1.2.28, p.6): the OS
puts *new factory engines* into free slots on update.

## 8. How to test an engine on the device (cheat sheet)

For the full testing session. All gestures are from the manual with the page given; "hold A, press B" means keep A down.
Assumptions: OP-Z in normal mode, **TE's app closed**, a **disposable project**, volume low, firmware 1.2.45.

| I want to… | Do this | Page |
|---|---|---|
| Know what the track buttons are | top row left → right: kick, snare, perc, sample, bass, lead, arp, chord, FX1, FX2, tape, master, perform, module, lights, motion | 38 |
| **Select a track** | hold **track**, press its top-row button; the white LED marks the active track | 14 |
| **Choose sound slot 1–10** | hold **track**, press black key **1…0** (F♯ = 1, G♯ = 2, A♯ = 3, C♯ = 4, D♯ = 5, F♯ = 6, G♯ = 7, A♯ = 8, C♯ = 9, D♯ = 0); lit = plug installed. **Tap; don't hold** (a long hold can remove a plug) | 25, 6 |
| Play it | press the keys (24 keys; on drums each key is its own sound; the −/+ buttons move the octave, the value keys light to show it) | 15 |
| Turn **P1 / P2** | select the track, make sure you are on the first parameter page (selecting a track resets to it), turn the **green** dial (P1) and **blue** dial (P2) | 21 |
| **Filter / resonance** | same page: **yellow** dial filter (centre = neutral, green LED), **red** dial resonance | 21, 13 |
| **Envelope page** | press and release **shift** once (green LEDs): green A, blue D, yellow S, red R | 14, 21 |
| **LFO page** | press and release shift again (purple LEDs): green depth, blue speed, yellow target, red shape; depth centre = off. On the arp track this page is the arp control | 21–22 |
| **Send / pan / level page** | shift once more (orange LEDs): green FX1 send, blue FX2 send, yellow pan, red level | 21 |
| Cycle back to page 1 | press and release shift again, or select the track again | 14, 20 |
| Do **temporary** tweaks | hold shift and turn a dial; releasing shift restores the value. Don't do this while testing a setting you want to keep | 15 |
| **Set an FX send** | on the source track, orange page, FX1 = green dial, FX2 = blue dial. Or on an FX track: hold shift, press the source track's button — tap = on/off, hold 1 s then turn the green dial | 21, 39 |
| **Sequence notes on an FX track** | hold track + FX1/FX2 button; play a key (that is "the last played note"); press an empty step to place it; hold a lit step and press keys to change its note, press another step while holding it to set its length; to record live hold rec and play keys while the sequencer runs | 18–19, 39 |
| **Hear an FX with an instrument** | easiest: instrument track, orange page, raise its FX send. Or stay on the FX track, hold **shift** and play keys to audition the previously selected drum/synth track through the effect | 39 |
| Put a **step** back to normal | hold the lit step + stop until all steps light (clears its locks); press a lit step to clear the note | 18 |
| **Restart the OP-Z** | turn the volume knob fully counter-clockwise past the click, wait a moment, turn it clockwise again. Needed when replacing an engine already loaded; a brand-new engine loads without it. A content-mode eject also restarts | 10, 61 |
| Stop a stuck note | stop, stop again (stopped sequencer), or track + stop (twice = kill) | 15, 17, 25 |
| **Select / save / recall presets** | hold track, press a **lit white** key = recall; hold track + white key 2 s = store (14 per plug); track + rec = randomize | 25 |
| **Reload a project** (H-5) | autosave is the default, so changes are already saved; to reload, hold project, tap another project key and tap back. Manual save mode (hold project + track a few seconds, or hold project at power-on) saves only on request — take care not to leave it on by accident | 29, 32 |
| Protect the project | project + stop clears the pattern and project + stop + shift the whole project: avoid these chords during tests | 31 |

Dangerous chords to avoid while testing: shift + keys on an instrument track (punch-in effects) and shift + dial
(temporary tweak, restored on release); rec + dial (records parameter locks); upgrade mode and factory reset (wipes custom
content — **never** in this project; use `opz.py backup` and `restore` instead).

Master track: keys played there **transpose the tracks marked on master** (shift + track buttons toggle which ones, p.45), so
don't play keys on master during other tests. Test note-ducking with `glue` on FX1/FX2 instead; master is a fixed effect (§4).

## 9. What it answers, what it changes, what still needs hardware

### Answered or sharpened ([manual])

| Item | What the manual gives | Status |
|---|---|---|
| How to select slots 1–10 | hold track + black key 1…0, tap (p.25) | **answered** |
| How to choose / store a preset | hold track + a lit **white** key; hold 2 s to store; max 14; track + rec randomizes (p.25) | **answered**, key → index mapping to confirm (H-3) |
| FX-track note behaviour | notes sequenced or played on an FX track are control input to the plug (reverb pre-delay, tape loop start/length); shift + keys on an FX track audition the previous track (p.39, 43, 72) | **answered** in principle; per-plug meaning is ours to define |
| [Q-13](OPEN_QUESTIONS.md) mic/line to an FX | the external signal runs through the effects like an instrument track; mic mode selects FX1/FX2/both (p.54–55) | **answered** (F-17 needs no engine change) |
| [Q-19](OPEN_QUESTIONS.md) master notes / [Q-20](OPEN_QUESTIONS.md) master effect | the master track is a fixed chorus + drive + filter with transposing notes; no user way to choose an effect (p.33, 39, 44–45) | **reframed**: master is not an effect slot for users; hardware confirms |
| Polyphony | drum 2 per step; bass 1; lead 3; arp 1 (arpeggiated); chord 4 (6 with `generous_chords`) (p.38–39, 73) | **answered** (notes per step) |
| Note styles | drums retrig/mono/gate/loop; synths poly/mono/legato; master latch/free (p.23, 45) | **answered**, maps to `params[17]` |
| LFO | 8 targets, 12 shapes (6 free + 6 note-triggered), 8 synced rates + free, depth centre = off (p.22) | **answered** |
| CC order | CC 1…18 listed (p.59) | **answered** |
| Octaves | −/+ buttons on every track, drum tracks have the full range; no limits given (p.7, p.8, p.15) | partial |
| Startup combinations | see §7 | **answered** |

### What it changes in our docs

1. **Presets are on the white keys, slots on the black keys** (p.25). [presets.md](presets.md)'s inferred index mapping
   and the first H-3 wording ("tap keys 0…3") are wrong or unconfirmed; the lit white keys also give a built-in readout
   for H-1/H-2. Added as [manual] notes there.
2. **FX tracks:** the manual's chart shows one page, but its changelog and text give FX tracks an LFO, send editing and a
   dry-mix setting; "FX tracks have no user-assignable presets" ([proven, user]) is **not** contradicted but the manual
   is silent on it; it explicitly mentions "improved random preset for … fx tracks" (p.5), so *randomize* exists there.
3. **F-18:** chains hold **32** patterns by the manual (our text: 31); the length multiplier is not a 64-step track.
4. **Stress test stage B** lists "bass (2 notes), arp (2 notes)": the manual says both are monophonic per step; 2 is the OS
   voice count including release overlap. Chord polyphony is 4 notes (6 voices with `generous_chords`).
5. **Drum labels:** the green page of a drum track reads attack/decay/sustain/release (p.21) while TE's player uses them as
   start/attack/hold/decay (ABI).
6. **MIDI CC order differs from `params[]` order** (§3).

### Still needs hardware

| # | Check | Why |
|---|---|---|
| H-1…H-5 | see [presets.md](presets.md#hardware-checks) with the manual gestures | white-key mapping, restart loss, audition |
| H-2b | store a preset on our plug with track + white key (2 s), restart, check the key is still lit | whether user-stored presets survive for our plugs |
| FX-page | on FX1 with `swirl`/`filter`: which shift page and dial give pan (dry/wet) and level; whether a page exists beyond the first | manual chart is incomplete |
| Master | on the master track, track + black keys: does anything select or light? (expected: no); only if a plug can be put there: do live keys and sequenced steps (latch and free) reach it | Q-19, Q-20 |
| Chains | how many patterns a chain really holds (31 or 32) | F-18 |
| Drums | what the green-page dials do on our `kit` and TE's kit | label mismatch |
| Pitch bend | does the physical bend reach FX1/FX2 (`renderFX` pitchBend)? | the manual lists only audio, tape, master |
| Long-hold | whether track + slot hold removes a plug on a user slot | safety |
| `generous_chords` | check the setting on the user's unit before the CPU test | chord voices 4 vs 6 |

## Manual oddities (so nobody trips on them)

- p.30: copy-track and copy-settings share one description; the drawing shows shift pressed three times for *track*.
- The punch-in chart prints "C♯" twice; the second is D♯ (p.48).
- p.10 and p.56 both describe disabling USB charging, with different keys (E2, "trigger spark"): they are the last key.
- p.22 calls the LFO destination count eight and lists eight; the arp has no LFO at all.
- p.38 says "kick, snare, perc, sample" while the p.21 chart says "kick / snare / hihat / perc".
