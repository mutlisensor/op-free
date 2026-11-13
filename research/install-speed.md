# Install speed: where the time goes, and the batched install

A tester's OP-FREE install of three engines, one preset refresh and a slot map took about two minutes (2026-11-11).
This note says why, what the loader does about it, and the device check the batched install still needs.

## 1. Every session costs a ~6.5 s wait

The loader's log from that install (OP-FREE site, every line timestamped) shows the same shape for every step:

| Session | Work inside | Then, until the next session's `$34` |
|---|---|---|
| backup read (plugs.json + slot map) | ~1.1 s | ~6.4 s |
| push (sync job) | 0.4–1.0 s | ~6.7 s |
| read-back to verify | ~1.1 s | ~6.7 s |

[proven: tester's log, 2026-11-11, e.g. `pd` 22:27:42.9 → 22:28:06.0, `sync` 22:28:06.0 → 22:28:28.2; each step
~22–23 s, about 20 s of it waiting]

- **No pause on our side.** The wait sits inside `FileSession.open()`: it sends `$51` and waits for `$34`, sending
  `$51` again every 1.5 s [code: `core/src/session.ts` `open()`; the site's `openSession()` adds only the retry
  after 15 s].
- **The likely cause.** The OP-Z drops a file-server session that gets no keep-alive for ~6 s
  [notes: `tools/opz_session.py` header]. We think it doesn't free a session on our close (`$52` with sync-active
  0, then `$55`), and only answers the next `$51` once that timeout has run [inferred]. That fits two observations:
  - The wait is the same after every close.
  - A run's first open answers in ~0.5 s [notes: `site/src/loader/index.ts`].
  
  Open question: **Q-86**.
- **What it means.** The single-engine flow (`install`: backup, push, read-back) costs ~22 s per engine. The
  printout of every track after each install is just a log line; it costs no device time.

## 2. What the loader does now

Both changes are offline and covered by tests. The second still needs H-IS1 (§3) before it is published.

1. **A backup reuses a fresh read-back** (`OpzOptions.reuseRead`, seconds; the OP-FREE site uses 5).
   - **When:** a flow's backup follows the previous flow's read-back by milliseconds. When nothing was pushed in
     between, that read-back *is* the device's state, so the backup takes it instead of opening another session.
   - **Safety:** any push drops it, and so does age. The default is 0 (always read), so `opz.py`, the web app and
     the recorded test vectors don't change.
   - **Saves:** one session (~7.5 s) per step [code + test: `core/test/opz.test.ts` "read-back reuse"].
2. **One push for the whole batch** (`Opz.installMany`).
   - **What it sends:** every new or upgraded engine file, plus one `settings/patch.json`
     (`buildBatchPatch`). That file has an `auto_import[]` entry per engine (no tracks) and a `plugs[]` entry per
     installed plug whose presets are refreshed. Then one read-back checks every engine and plug record, and that
     the slot map didn't change; if it changed, the slot map is restored.
   - **The slot map:** it still goes in its own push afterwards (`writeSlotMap`, whose backup takes the
     read-back). We don't know in what order the OP-Z applies the files of one sync job, and a slot map naming a plug
     that isn't imported yet could be dropped [idea].
   - **Why it should work:** `importPatch` walks `auto_import[]` and then `plugs[]` in one file
     [code: device `0x801001d6`, research/presets.md]. TE's own boot file carries several `auto_import`
     entries [code: crash-safety.md].
   - **Not shown yet:** several new engines in one push from us [idea] → **H-IS1**.

| Batch of N engines (+ slot map) | Sessions | Waits of ~6.5 s | About |
|---|---|---|---|
| before: `install` per engine, `presetsPush` per refresh, `writeSlotMap` | 3 per step | 3 per step | 22 s per step (the tester's five steps: ~1 min 50 s) |
| with reuse only | 2 per step + 1 | the same count | ~15 s per step |
| `installMany` + reuse (the OP-FREE site now) | **5 in all** | 5 | **~35 s, any N** [inferred from §1's timings] |

The site's test "a whole batch is 5 sessions and 2 pushes, however many engines" pins this
[test: `OP-FREE/site/src/loader/test/flow.test.ts`, 15 engines in the demo].

## 3. H-IS1: several engines in one push (device check)

**Asks:** does the OP-Z import every `auto_import[]` entry and `plugs[]` entry of one `patch.json`, sent in one sync
job together with all the engine files, and is the batch really ~35 s?

**Needs:** the OP-FREE site from `loader/fast-install`, run locally (`npm --prefix OP-FREE/site run dev`), with TE's
app closed. **Each write needs the user's go-ahead**, as for every install (SAFETY.md). The confirm sheet is the
dry run.

1. Take a status and a backup in the site (read only).
2. Pick two or three engines that aren't installed yet. Choose test plugs where you can; otherwise released plugs
   that the user agrees to add. Give one of them a slot on a synth track. Also pick one installed engine with
   presets, so its presets are pushed again.
3. In the confirm sheet, check the files: the engine files, then the `settings/patch.json` lines, then "(one
   patch.json and one push for all of them)", then the slot map. **User approves.**
4. Expected log:
   - one `sync: requested [...]` naming every engine and `settings/patch.json`;
   - `installed and verified` listing every new plug, with the slot map unchanged at that point;
   - `settings as read back 0.0 s ago (nothing written since)`, then the slot-map push and `slot map written and
     verified`;
   - `session open` five times in all, and about 35 s from the first open to `batch: done.`.
5. On the OP-Z, play the newly slotted engine. Hold its track: the lit white keys show the presets.
6. **Rollback:** "Put the original sounds back" (step 5) restores the slot map. The new plugs stay installed, as
   after every install (taking OP-FREE off removes them).

**If an engine is missing in the read-back:** the batch stops with `verify:` lines and the slot map isn't written.
That would answer H-IS1 *no*. Ship the read reuse only, and keep one engine per push.

## 4. Open

- **Q-86:** why ~6.5 s between sessions? Is there a close the OP-Z honours at once? If there is, every remaining
  session gets cheaper, the single-engine flows too.
- **Q-87:** in what order does the OP-Z apply the files of one sync job? If `patch.json` is imported before
  `slotConfiguration.json` is applied, the slot map could join the engine push, and a batch would drop to 3
  sessions (~20 s).
