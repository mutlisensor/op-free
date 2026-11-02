# OP-FREE

**A first step into liberating your OP-Z.** New synths and effects for the teenage engineering OP-Z, built by the
community, that you can try in your browser and load onto your own unit; the SDK to build your own; and the research
into how the OP-Z firmware works, as the ground for firmware mods later.

> An independent community project. Not affiliated with, endorsed, sponsored or supported by teenage engineering.
> OP-Z is their product and trademark. This repository contains no OP-Z firmware, no keys, and none of teenage
> engineering's code, engines or sounds. Provided as is, without warranty: read the risk notice on the site first.

**Website:** <https://mutlisensor.github.io/op-free/>

## What is here

| Folder | What it is |
|---|---|
| [`site/`](site/) | The website: the landing, the synth lab (play every engine in the browser) and the loader (put them on your OP-Z). Static, hosted on GitHub Pages |
| [`sdk/`](sdk/) | Write your own engine: the engine ABI header, DSP helpers, templates, linker script and the [SDK docs](sdk/docs/) |
| [`sim/`](sim/) | The host simulator (`opzsim`): runs any engine on your computer, renders scenarios, checks the ABI rules; also in the browser (`sim/web`) |
| [`tools/`](tools/) | Build, pack, verify, crash and CPU checks, the engine scaffold, and the installer CLI (`tools/opz.py`, with an offline `--fake` OP-Z) |
| [`core/`](core/) | The TypeScript core that talks to the OP-Z over USB MIDI (the file service, backups, install plans, safety checks) |
| [`engines/`](engines/) | The community synths, drum engines and effects, each with its presets, tests and a one-page manual |
| [`research/`](research/) | How the OP-Z works, with an evidence label on every claim |
| [`guides/`](guides/) | Use the site, extract your own firmware, submit an engine, propose a firmware mod |
| [`firmware-mods/`](firmware-mods/) | Coming soon: the mod format and the builder that applies mods to *your own* firmware file |
| [`ROADMAP.md`](ROADMAP.md) | The plan and what's next |

## How engines get onto an OP-Z

Every OP-Z synth and effect is a small DSP module that the OS loads from its storage and calls through a few
functions. A new engine ships as a *user plug*, installed over USB with the same file protocol the official app uses,
and sits in a sound slot next to teenage engineering's own. Nothing is flashed, and a restore puts everything back.
The site walks you through it: try → pick → connect and back up → install → content mode (presets) → eject and play.

## Contributing

New synths and effects are welcome: start with the [SDK quickstart](sdk/docs/QUICKSTART.md), then
[submit an engine](guides/submit-an-engine.md). Firmware mods start as research and proposals:
[propose a firmware mod](guides/propose-a-firmware-mod.md). Rules for every contribution are in
[CONTRIBUTING.md](CONTRIBUTING.md).

## Licence

MIT ([LICENSE](LICENSE)). Engines that port DSP from other projects credit them in their README; only MIT, BSD, ISC
or public-domain sources are accepted.
