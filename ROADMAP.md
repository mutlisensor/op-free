# OP-FREE roadmap and workstreams

OP-FREE is the public face of this project: a website, an SDK and the research, so that other people can build synths
and effects for the OP-Z, load them onto their own unit, and later propose firmware modifications. This file is the
plan. The private lab (the repository this folder lives in) keeps doing the research and the hardware tests; OP-FREE
gets what has been checked and is safe to publish.

## How the two repositories relate

```
op-z-engines (private lab)                    op-free (public, GitHub + GitHub Pages)
├─ sdk/ sim/ tools/ web/core/ engines/ ──sync──▶ ├─ sdk/        headers, templates, simulator, build tools
├─ docs/architecture, docs/sdk, research ──────▶ ├─ core/       the SysEx / install core the site uses
│                                                ├─ engines/    released community engines (source)
├─ OP-FREE/  ◀── this folder IS the public repo  ├─ research/   how the OP-Z works (evidence-labelled)
│   (git subtree push --prefix OP-FREE)          ├─ guides/     use the site, extract firmware, contribute
└─ tools/opfree_sync.py (whitelist copy + guard) ├─ firmware-mods/  coming soon: the mod format, proposals
                                                 └─ site/       the website (Vite, static, GitHub Pages)
```

- `OP-FREE/` is the root of the public repository. It is published with `git subtree push --prefix OP-FREE`, so
  only what is inside this folder ever leaves the lab.
- Shared code (SDK, simulator, `web/core`, engines) keeps living where it lives in the lab. `tools/opfree_sync.py`
  copies a **whitelist** of paths into `OP-FREE/` and refuses to run if anything forbidden would be copied (TE
  binaries, firmware, keys, dumps, decompiled TE code, `*.zfw`, `*.ldr`, files from `decrypted/`, `keys/`, …).
  Copies are never edited by hand: change the lab, then sync.
- Community contributions arrive as pull requests on the public repository and are brought back into the lab the
  same way in reverse (a contributor's engine folder is copied into `engines/`).

## What the website does

| Step | Page | What happens |
|---|---|---|
| 1 | Landing | The OP-FREE image and an **Enter** button |
| 2 | Risk notice | What can go wrong (crashes, lost user samples, bricking for firmware mods), not affiliated with teenage engineering, no warranty, we host no firmware or keys. A tick box, then *Continue* |
| 3 | Home | The welcome message, **how it works** (the steps below) and three sections |
| 4 | Synths & effects | Try every engine in the browser (synth + FX1 + FX2, presets, an on-screen and a MIDI keyboard); pick what goes on your OP-Z (8 picked by default); install |
| 5 | Firmware mods | *Coming soon*: what is planned, and how *bring your own firmware* will work |
| 6 | Source & research | The repository, the SDK, the research, how to submit an engine, how to extract your firmware |

### Getting sounds onto the OP-Z

The flow the site walks you through, each write asked for and verified:

1. **Try** the engines in the browser. Nothing touches the OP-Z.
2. **Pick** up to 10 sounds per track group; 8 are ticked by default so two slots stay free for your own samples.
   A toggle switches between **community** engines and the **original** OP-Z sounds.
3. **Connect** over USB (Chrome or Edge, Web MIDI). The site backs up your OP-Z's settings first.
4. **Install** the engines over USB MIDI (the OP-Z's file service, the same route TE's app uses). Engine code
   *cannot* go in through content mode: the OP-Z's content mode has no engine folder [code,
   `docs/research/p0-memory-content.md`].
5. **Content mode** (hold *track* while switching on): the site writes your presets into the OP-Z's own preset store
   (`config/userPresets.dat`) so they survive a restart, and can arrange which slot each sound sits in.
6. **Eject**: the OP-Z restarts with your sounds and presets. *Restore* puts the backup from step 3 back at any time.

**The original OP-Z sounds.** We never host or upload teenage engineering's engines. They don't need uploading:
every OP-Z already carries them, and a sound we displace stays installed, out of its slot. Choosing *original* for
a slot puts TE's sound back into it (a slot-map change, nothing uploaded). They can't be previewed in the browser,
because we don't have and won't copy their DSP code.

## Workstreams

Each workstream is its own branch and session. They touch separate folders so they can run at the same time.

| # | Workstream | Folder(s) | Depends on | Done when |
|---|---|---|---|---|
| W0 | **Foundation**: this plan, the folder, the site shell (landing, risk notice, home, sections, routing, design system) | `OP-FREE/` | — | the site builds and runs; the lab and loader mount points exist |
| W1 | **Synth lab**: the browser simulator (`sim/web`) retooled into the site: engine browser, presets, synth → FX1/FX2, on-screen + Web MIDI keyboard | `OP-FREE/site/src/lab/` | W0 | every released engine plays in the site; wasm modules come from the sync |
| W2 | **Loader**: pick 8 of 10 (community / original toggle), back up, install over USB MIDI, content-mode bake of presets and slots, restore; demo mode | `OP-FREE/site/src/loader/` | W0 (uses `web/core`) | the full flow runs against the demo OP-Z with the same safety flows as `web/app` |
| W3 | **SDK, research and guides export**: `tools/opfree_sync.py` with its guard, the public README/CONTRIBUTING, guides (use the site, extract your firmware, firmware explained, submit an engine, propose a firmware mod), issue and PR templates | `OP-FREE/{sdk,core,engines,research,guides,.github}`, `tools/opfree_sync.py` | W0 | a fresh clone of `OP-FREE/` builds an engine and runs the simulator with nothing from the lab |
| W4 | **Publishing**: the public GitHub repository, the GitHub Pages workflow, the first release | GitHub | W0–W3, the user's go-ahead | the site is live at `https://<user>.github.io/op-free/` |
| W5 | **Firmware mods** (later): the patch format (`tools/opz_fwpatch.py`), the browser builder that patches the user's own firmware file, the first mods | `OP-FREE/firmware-mods/`, `firmware/` | recovery proven on a spare unit (`firmware/README.md`) | the *Coming soon* block becomes a builder |

### Firmware mods on the list (W5)

All research first; none of this is built yet.

- Sequencer: 64 steps through pages.
- Button debouncing (some buttons double-trigger).
- Bluetooth audio: what the radio and the firmware can do.
- New arpeggiator and *player* note styles.
- Modules: updates for the line module and DMX out.

## Rules that carry over from the lab

- We host no firmware, no keys and nothing of teenage engineering's: no engines, no decompiled code, no artwork.
  The firmware builder will take **your own** firmware file, in your browser.
- Every write to an OP-Z is backed up, shown, confirmed, verified by reading back, and rolled back if it fails.
- Every factual claim in the research carries an evidence label ([proven], [code], [manual], [inferred], [idea]).
- Our code is MIT; ported DSP only from MIT, BSD, ISC or public-domain sources.
