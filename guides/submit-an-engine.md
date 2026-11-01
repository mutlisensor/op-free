# Submit an engine

How to get your synth, drum engine or effect onto the OP-FREE website, where everyone can play it in the browser and
put it on their OP-Z. You need a Mac or Linux machine with Python 3 and a C compiler; an OP-Z is not required to
start (the simulator stands in for it). The [SDK quickstart](../sdk/docs/QUICKSTART.md) explains each command in
detail; this page is the path from fork to merged pull request.

## 1. Fork and check the simulator

Fork the repository on GitHub, clone your fork, and make a branch named after your engine:

```bash
git checkout -b engine/wobble
make -C sim test            # builds the simulator and every engine, ends with "N passed, 0 failed"
```

## 2. Scaffold

```bash
python3 tools/opz.py new wobble --kind synth      # or --kind drum, --kind fx
```

This copies a template into `engines/wobble/` with a free **test plug id** (60900–60999) and a free **memory slot**,
and records the slot as a row in [memory-slots.md](../sdk/docs/memory-slots.md). Names are lowercase letters,
digits and `_`, and must not be teenage engineering's engine names or other companies' trademarks.

## 3. Make the sound, in the simulator

Edit `engines/wobble/wobble.c`, then render, listen and test:

```bash
make -C sim golden ENGINE=wobble      # renders to sim/build/renders/wobble/, writes the golden hashes
make -C sim test                      # every engine's golden renders and the host checks
```

You can also play it live in the browser: `make -C sim wasm`, then `npm --prefix sim/web install` and
`npm --prefix sim/web run dev` ([quickstart](../sdk/docs/QUICKSTART.md#play-it-in-the-browser)). Write scenarios
(`tests/*.scn`) that cover the knob extremes and fast knob moves while notes play: that is where engines crash.

Keep to [the rules your code must keep](../sdk/docs/QUICKSTART.md#the-rules-your-code-must-keep): fixed point only
in audio loops, no 64-bit multiplies or division per sample, state within 280 bytes per voice (200 per effect),
instruments *add* into the output.

## 4. Device build, crash check, CPU

These need the Blackfin cross-compiler ([toolchain setup](../research/toolchain-setup.md)). If you can't install
it, say so in the pull request and a maintainer runs them for you.

```bash
python3 tools/opz_build.py engines/wobble                 # device build + structure check (opz_verify)
python3 tools/opz_crashcheck.py engines/wobble --full     # crash patterns, stack, bounds, torn state
python3 tools/opz_cycles.py engines/wobble                # cycles per sample in the Blackfin simulator
```

The budget is about **167 cycles per sample per voice** for instruments with every voice busy (aim for 150 or
less) and about 600 for effects; check the worst block, not only the mean ([CPU budget](../research/cpu-budget.md)).
The crash check must report no errors ([crash safety](../research/crash-safety.md)).

## 5. Golden tests

The hashes in `engines/wobble/tests/golden/` are a promise that the engine sounds exactly like this. Commit them with
the engine, re-render only when you meant to change the sound, and say why in the commit message. CI runs
`make -C sim test` on every pull request.

## 6. The README: your engine's manual

`engines/wobble/README.md` is what the website shows. Copy the layout of an engine of the same kind
([`swirl`](../engines/swirl/) effect, [`fm2`](../engines/fm2/) synth, [`kit`](../engines/kit/) drums):

- one paragraph on what it is;
- **P1 and P2** and every parameter that does something unusual;
- the **presets**;
- **CPU cost** (from step 4), memory slot and size;
- **what to listen for**: a short checklist a tester works through on a real OP-Z (fast knob moves over chords,
  high and low notes, levels next to the OP-Z's own sounds, anything specific to your engine);
- **credits**: where ported DSP comes from, with its licence.

## 7. Plug id and memory slot

Plug ids and memory slots are shared by every engine on the site: two engines in one memory slot overwrite each
other on the OP-Z, and a plug id can only mean one sound. So:

- **Submit with the test plug id and the slot `opz.py new` chose.** Keep the row it added to
  [memory-slots.md](../sdk/docs/memory-slots.md).
- **A maintainer assigns the released id** (from 61000 up, one block of ten per engine family) and confirms or
  changes the slot when your pull request is accepted, so parallel pull requests don't collide. You then bump
  `version` and `engine_version` in `plug.json` and rebuild.
- Aim for the **5 KB** memory slot class: there are few large slots (three 50 KB, a handful of 10 KB), and a
  50 KB slot needs a reason (wave tables, a patch bank). Slots above 89 are not proven safe and are refused.

## 8. Licences

- Your engine is published under the repository's **MIT** licence.
- Ported DSP code only from **MIT, BSD, ISC or public-domain** sources. Keep the original copyright notice in the
  source and credit it in the README. GPL code can't be used.
- Nothing of teenage engineering's: no code, tables, samples, or renders of their engines. Presets and patch data
  must be your own (no factory banks of other instruments).
- No "teenage engineering", "Mutable Instruments" or other companies' names in engine names.

## 9. Pull request

Open a pull request from your branch. The template asks for this checklist:

- [ ] `make -C sim test` passes, with the new engine's golden hashes committed
- [ ] `opz_build.py`, `opz_crashcheck.py --full` and `opz_cycles.py` run clean (or: please run them for me)
- [ ] CPU cost, memory slot and size in the README
- [ ] README with P1/P2, presets and **what to listen for**
- [ ] Row in `sdk/docs/memory-slots.md`; test plug id until a maintainer assigns the released one
- [ ] Ported code credited, MIT/BSD/ISC/public domain only; no TE material
- [ ] If you tested on an OP-Z: what you did and what you heard

**What happens next.** A maintainer reviews the code and runs the checks; then the engine is tested on hardware
(install, play through the README's checklist, remove, reinstall). Engines that pass go onto the website in the
next release. One engine per pull request, please.
