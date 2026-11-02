# SDK

Everything you need to write, test and build your own OP-Z engine. Start with the
[quickstart](docs/QUICKSTART.md): from a fresh clone to an engine running in the simulator takes a few minutes.

| Part | Where |
|---|---|
| Engine ABI header and DSP helpers (`opz_engine.h`, `opz_dsp.h`, `opz_adsr.h`, `opz_tables.h`, `opz_tempo.h`, `opz_voices.h`) | [`include/`](include/) |
| Templates (synth, drum, fx), scaffolded by `python3 tools/opz.py new` | [`templates/`](templates/) |
| Linker script template, filled per memory slot | [`ld/`](ld/) |
| The spec: [engine ABI](docs/engine-abi.md), [plug format](docs/plug-format.md), [memory slots](docs/memory-slots.md), [development workflow](docs/development-workflow.md) | [`docs/`](docs/) |
| Host simulator (`opzsim`) and the web simulator | [`../sim/`](../sim/) |
| Build, pack, verify, crash and CPU checks, the installer CLI | [`../tools/`](../tools/) |
| Blackfin toolchain install | [`../research/toolchain-setup.md`](../research/toolchain-setup.md) |

When your engine is ready: [submit an engine](../guides/submit-an-engine.md).
