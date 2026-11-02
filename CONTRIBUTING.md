# Contributing to OP-FREE

Thank you for helping. Contributions come in three kinds, each with its own guide:

| You want to | Start here |
|---|---|
| Add a synth, drum engine or effect | [Submit an engine](guides/submit-an-engine.md) (and the [SDK quickstart](sdk/docs/QUICKSTART.md)) |
| Answer a research question, or correct one | [Research index](research/README.md) and [open questions](research/OPEN_QUESTIONS.md) |
| Propose a change to the OP-Z firmware | [Propose a firmware mod](guides/propose-a-firmware-mod.md) (issue first) |
| Report a bug or a crash | The *Bug report* issue template |
| Improve the website | `site/` (Vite, TypeScript); `npm --prefix site install`, `npm --prefix site run dev` |

## How this repository works

OP-FREE is the public side of a private lab where the research and the hardware tests happen. Most folders here
(`sdk/`, `sim/`, `tools/`, `core/`, `engines/`, `research/`) are copied from the lab by a sync script with a guard
that refuses to publish anything forbidden. Pull requests are welcome on all of them: a maintainer carries an
accepted change back into the lab, and the next sync brings it here. The website (`site/`), the guides and the
files at the top level are maintained here directly.

## Workflow

- One branch and one pull request per engine, tool change or research question.
- Run what CI runs before you push (it needs no Blackfin toolchain):

  ```bash
  make -C sim test                                  # simulator host checks + every engine's golden renders
  python3 -m unittest discover -s tools/tests       # tools
  npm --prefix core ci && npm --prefix core test    # the USB MIDI core
  ```

- Re-rendering golden files (`make -C sim golden ENGINE=<name>`) changes the reference: say why in the commit
  message.
- Commit messages name the area: `engine(wobble): add a sub oscillator`, `tools(pack): …`, `docs(abi): …`,
  `research: answer Q-NN …`, `site: …`.

## Rules for every contribution

- **Nothing of teenage engineering's.** No firmware, no keys or key material, no decompiled or disassembled code
  beyond the few instructions needed to make a point, no extracted files, tables, samples, artwork, or renders of
  their engines. Describe what their code does in your own words, with addresses.
- **Evidence labels** on every factual claim about the OP-Z: [proven], [code] (with the address and binary),
  [notes], [manual] (with a page), [inferred], [idea] ([what they mean](research/README.md#evidence-labels)).
  Don't upgrade a label without new evidence.
- **Safety first** for anything that writes to an OP-Z: back up, show a plan, confirm, verify by reading back,
  roll back on failure ([SAFETY.md](SAFETY.md)). Behaviour changes in `tools/opz.py` go into `core/` too, and the
  other way round.
- **The engine ABI is a contract** (`sdk/include/opz_engine.h`, [engine-abi.md](sdk/docs/engine-abi.md)): changing it
  needs evidence and the matching change in `sim/`, `tools/` and `core/` in the same pull request.

## Naming and numbers

| Thing | Convention |
|---|---|
| Engine folder | `engines/<name>/`, lowercase letters, digits and `_`; no company or product names |
| Test plugs | ids 60900–60999 (what `tools/opz.py new` gives you) |
| Released plugs | ids from 61000, one block of ten per engine family, assigned by a maintainer at merge |
| Memory slots | one per engine, recorded in [memory-slots.md](sdk/docs/memory-slots.md) in the same pull request; never a slot teenage engineering uses, never above 89 |

## Licences

- Everything here is **MIT** ([LICENSE](LICENSE)); by contributing you agree to license your contribution under it.
  Vendored code (`third_party/`) keeps its own licence and notice.
- Ported DSP only from **MIT, BSD, ISC or public-domain** sources, with the original notice kept in the source and
  credited in the engine's README. GPL code (for example Mutable Instruments' AVR projects) can't be used; their
  STM32 firmware is MIT and can.

## Conduct

Be kind, assume good faith, and keep discussion about the work. This is a hobby project run in spare time;
maintainers may take a while to answer.
