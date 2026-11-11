# opzsim web

Play OP-Z synths and drums on eight tracks through effects on FX1 and FX2 in the browser before installing any of
them, from the computer keyboard or an OP-Z over MIDI (each track on its own channel, the OP-Z's dials moving the
knobs; opz-as-controller.md). The page runs each engine with opzsim's host C compiled
to WebAssembly (one module per engine, `make -C sim wasm`), in an AudioWorklet at 44.1 kHz with 64-frame blocks.
Scenario renders are bit-identical to the `opzsim` CLI. How it works and how it differs from the CLI:
[sim/README.md](../README.md#web-simulator). Setup from a fresh clone: [QUICKSTART.md](../../sdk/docs/QUICKSTART.md#play-it-in-the-browser).

```bash
make -C sim wasm
npm --prefix sim/web ci
npm --prefix sim/web run dev        # http://localhost:5174/?engine=engines/fm2
```

| File | Role |
|---|---|
| `catalog.ts` | Vite plugin: `/api/catalog` (engines and templates with plug.json, presets, scenarios, module URL) and `/files/…` (only `engines/`, `sdk/templates/`, `sim/build/wasm/`, `sim/build/samples/`) |
| `src/wasi.ts` | the WASI preview1 calls the modules import, over an in-memory file system |
| `src/opzsim.ts` | `runScenario` (opzsim's `main`) and `LiveEngine` (the `ws_*` entry points of `sim/wasm/websim.c`) |
| `src/worklet.ts` | the AudioWorklet: the eight instrument tracks and the FX1 and FX2 slots |
| `src/midimap.ts` | MIDI in: channel → track, OP-Z CC → parameter byte, MIDI learn (no DOM) |
| `src/mixer.ts` | the send/return model: each send crossfades, an effect's dry level keeps the dry path, FX1 and FX2 in parallel, the tracks summed |
| `src/main.ts`, `knob.ts`, `keyboard.ts`, `scope.ts` | the page |

| Test | Checks | Needs |
|---|---|---|
| `npm test` | every `engines/*/tests/*.scn` rendered in wasm hashes as its golden file; the live path gives the same samples as the scenario path (templates, fm2, kit, swirl, filter); the mixer model; the MIDI map and learn | `make -C sim wasm` (skips without) |
| `npm run test:e2e` | headless Chrome: each template, swirl, fm2 and kit played from the computer keyboard; effects on a generator; fm2 through a passthrough effect keeps its level as the send crossfades, and gains ~6 dB with dry level 255; through passthroughs on FX1 and FX2 at full send the returns add up (+6 dB); fm2 through swirl; a scenario run in the page matches its golden; MIDI: each channel plays its track, OP-Z CCs move the knobs and the page follows, learn survives a reload, the virtual OP-Z | the above and Chrome (`CHROME_PATH`) |
| `npm run typecheck` | TypeScript | |

Host CPU in the page is this browser's WebAssembly, not Blackfin cycles: for those run `python3 tools/opz_cycles.py`.
