## What this changes

<!-- One engine, tool change or research question per pull request. -->

## Kind

- [ ] New engine (see `guides/submit-an-engine.md`)
- [ ] Engine change
- [ ] Research (answers or corrects a question in `research/OPEN_QUESTIONS.md`)
- [ ] Firmware mod proposal (design note and a `DRAFT` patch record; issue first)
- [ ] Tools, simulator or core
- [ ] Website or guides

## Checklist

- [ ] `make -C sim test` passes (with new or re-rendered golden hashes committed, and the reason in the commit)
- [ ] `python3 -m unittest discover -s tools/tests` passes
- [ ] `npm --prefix core test` passes (if `core/` changed)
- [ ] Nothing of teenage engineering's: no firmware, keys, decompiled code, extracted files or renders of their engines
- [ ] Evidence labels on every factual claim about the OP-Z (research and docs)
- [ ] Ported code is MIT, BSD, ISC or public domain, and credited

### For a new engine

- [ ] `opz_build.py`, `opz_crashcheck.py --full` and `opz_cycles.py` run clean (or: please run them for me, I have no Blackfin toolchain)
- [ ] README with P1/P2, presets, CPU cost, memory slot and **what to listen for**
- [ ] Row in `sdk/docs/memory-slots.md`; test plug id until a maintainer assigns the released one
- [ ] Tested on an OP-Z? What you did and what you heard:
