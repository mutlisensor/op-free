# @opz/core

The TypeScript core for the web configurator and the iOS app (roadmap 5b.1): the OP-Z SysEx file service, install
planning and the safety flows, ported behaviour for behaviour from the Python tools that are proven on hardware.
No runtime dependencies; it runs in browsers, a `WKWebView` and Node ≥ 23.6.

| Module | Port of | What it does |
|---|---|---|
| `src/sysex.ts` | `third_party/opzsysex/opzsysex/sysex.py` | TE header, pack7/unpack7, `frame`/`decodeFrame`, `stateSyncClientHello`, `MIDIStreamParser` |
| `src/fileprotocol.ts` | `opzsysex/fileprotocol.py` | file ids, `UploadState` (sync job, pacing, in-flight window), `DownloadState` |
| `src/session.ts` | `tools/opz_session.py` | `Transport`, `FileSession`: open/pull/push/close, keep-alives every second, re-advertise, the `$33 127` + grace completion rule, the pull re-request when a stream pauses. `StateSession`: the StateSync hello, the current sound per track from the bank the OP-Z sends at the start (`$09`/`$0A` parts acked with `$0B`, `$08` to start over; a `$0E` from the OP-Z counts too) and `$0E` writes; `soundFrame`/`decodeSound`, `bankAckFrame`/`bankRequestFrame`/`bankSounds` |
| `src/plan.ts` | pure helpers of `tools/opz.py` | `buildPatch` (presets with `defaultOctave`), `planInstall` collision checks, `verifyInstall`, `assign`/`unassign`, `orphanEngines`, `describe`; presets: `presetProblems`, `buildPresetsPatch` (top-level `plugs[]`), `presetSlot`, `findPreset`, `formatSound`, `PARAM_NAMES` |
| `src/opz.ts` | `Opz` in `tools/opz.py` | backup → plan → confirm → push → read-back verify → slot-map rollback, for install, assign, remove, restore, restore `touch`, `presetsPush`, `persistPresets` (the boot file `settings/plugs_patch.json`); `audition`, `setSound`, `watch` over `$0E`. **TypeScript only:** `writeSlotMap` (the web app's plug librarian: any slot-map edit, refused when the OP-Z's map changed since it was read; `slotMapProblems`, `slotMapChanges`, `sameSlots` in `plan.ts`), no `opz.py` command yet |
| `src/space.ts` | `space_report`, `install_room`, `plan_remove_ours` in `tools/opz.py` | room on the OP-Z: `spaceReport` (plug definitions of 200: ours, the user's samples, TE's; our engine files), `installRoom` (the install warning), `planRemoveOurs` + `Opz.removeOurs` (every plug of ours off: empty boot file, slot map with `putBack`, removals). docs/roadmap/restore-space.md |
| `src/restorecheck.ts` | `tools/opz_restorecheck.py` | `checkContentRestore` (the content-mode importer's rules on a backup folder's names and sizes), `describeRestoreCheck`, `parseImportLog`; same lines as `opz.py restore-check` (shared fixture `tools/tests/fixtures/restore-check.json`) |
| `src/userpresets.ts` | `tools/opz_userpresets.py`, the offline part of `opz.py presets bake` | `config/userPresets.dat` (the OP-Z's user-preset store on the content-mode disk): `parseUserPresets` with the strict checks, `serialiseUserPresets`, `bakeUserPresets` (our presets in as user presets; hand-stored ones kept unless `overwriteUser`), `readUserPresetsFile`, `planBake`, `planRestore` with the CLI's log lines. **TypeScript only:** the report's `replacedKeys` (which hand-stored presets a bake replaces, for the confirm sheet) |
| `src/fake.ts` | `tools/fake_opz.py` | an in-memory OP-Z for tests and a demo mode, including presets (merge, `restart()`, `applyPreset()`) and the StateSync session with its own clock |
| `src/virtual.ts` | `tools/record_vectors.py` | `VirtualTransport` (virtual clock + transcript), `ReplayTransport` (plays a recorded transcript) |
| `src/zlib.ts` | Python's `zlib` | deflate identical to zlib 1.2.12 level 6, inflate with Python's semantics, CRC-32, Adler-32 |
| `src/pyjson.ts` | Python's `json`, `repr`, `int`, `==` | so files and log lines come out exactly as the Python tools write them |
| `src/project.ts` (exported as `project`) | `tools/opz_project.py` | project files and pattern banks: `Project.parse`/`toBytes` (byte-exact), typed reads and edits, `problems()`, `euclid`, `writeRhythm`, `writePages`, `projectPath`/`projectFileId`; checked against `test-vectors/project.json` (`python3 tools/opz_project.py vectors`). No project transport yet: a 343 KB push needs more than `zlib.ts`'s 16 KB deflate ([research/patterns.md](../research/patterns.md)) |

## Plugging in a MIDI backend

```ts
import { Opz, makeEngine, type Transport } from "@opz/core";

const transport: Transport = {
  send(sysex) { /* Web MIDI: output.send(sysex); iOS: post to the CoreMIDI bridge */ },
  receive(timeoutMs) { /* resolve with the next complete F0..F7 message, or null after timeoutMs */ },
};
const opz = new Opz(transport, { log: console.log, confirm: (q) => window.confirm(q) });
await opz.install(makeEngine(plugJson, presetsJson, engineBytes), [4]);   // track bass
```

`receive` must queue SysEx messages that arrive between calls; `MIDIStreamParser` reassembles them from a raw byte
stream. Times inside the core are seconds from a `Clock` (`performance.now() / 1000` by default). Backups go to a
`BackupStore` (`MemoryBackupStore` by default; the web app will want IndexedDB or a download). Close TE's app first:
two clients on one session break both.

## Tests

```bash
cd core
npm test            # node:test with Node's built-in TypeScript type stripping; no install needed
npm install         # once, for the type checker (typescript, @types/node; dev only)
npm run typecheck
```

`test/vectors.test.ts` replays the **shared test vectors** in `test-vectors/`, recorded by
`python3 tools/record_vectors.py` from the Python tools driving `tools/fake_opz.py`: open and pull both settings files
(with a lost advert and a lost part), install through `patch.json`, install into a fixed slot, assign, remove with
unassign, restore, an unchanged restore (ends on the grace rule), restore `touch`, a presets push, a presets push to
an OP-Z that keeps `patch.json` (re-sent with a nonce), writing and emptying the boot file, an audition (waits for a live state session to expire), a
`$0E` sound set and a watch. Vectors carry the fake's state besides its files (clock, session, sounds, presets).
Each vector is checked twice:

- **replay**: the TypeScript host against the recorded device frames must send byte-identical frames, call
  `receive` with the same timeouts and log the same lines;
- **end to end**: the TypeScript host against the TypeScript `FakeOpz` must reproduce the whole transcript in both
  directions and leave the same device files and state.

`test/traces.test.ts` checks the **trace vectors** in `test-vectors/traces/`, written by `tools/import_trace.py` from
the web app's SysEx traces (web/app README, "SysEx traces and hardware vectors"): every recorded frame must decode
in TypeScript exactly as Python decoded it, and the files pulled and served must reassemble to the same SHA-256.
Hardware runs cannot be replayed byte for byte (real time decides the empty receives), so these pin the decoding of
what the device sent. `demo-first-run.json` is a demo-mode (FakeOpz) trace that keeps the format honest until the
first hardware traces join it.

`userpresets.json` is `opz.py presets bake` itself (`bake_presets`) on a temporary content disk: 13 cases (a bake
with a hand-stored preset kept, a plug that gets a free record, an engine left out for presets.json errors, the same
bake again, `--overwrite-user`, a re-bake that updates an earlier one, `--show`, a TE plug id, no free record,
`--restore` good and bad, and the four strict-check refusals), each with the file before (sparse: non-zero runs),
the log lines and the file after. `test/userpresets.test.ts` must give the same lines up to the write and the same
bytes, and ports `tools/tests/test_userpresets.py`.

`deflate.json` and `pyjson.json` pin the compressor and the JSON encoder to Python's output. The Python suite
(`python3 -m unittest discover -s tools/tests`) fails when the vectors are stale. After a deliberate protocol change:
re-record, then change the TypeScript port until `npm test` passes again.

## Why a hand-written zlib

The OP-Z checks the CRC of the inflated data, so any valid deflate stream installs. But browsers and Node use
Chromium's zlib fork, whose output differs from vanilla zlib for most 200-byte parts, and `DecompressionStream`
rejects trailing bytes that Python ignores. Byte-identical frames are what make the shared vectors a real check, so
`src/zlib.ts` reproduces zlib's level-6 `deflate_slow` for one block (inputs up to 16 382 bytes; the file service
compresses at most 200 bytes at a time). It matches Python on the 309 fixture cases (and matched 6 000 more in a one-off fuzz run during the port).

## Known differences from Python

- JSON numbers with a zero fraction (`1.0`) come back as `1`, integers beyond 2^53 lose precision, and integer-like
  object keys are reordered by JS objects. OP-Z settings files have none of these.
- `Transport.receive` takes milliseconds; the session converts from its internal seconds.
