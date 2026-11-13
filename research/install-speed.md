# Install speed: where the time goes, and the batched install

A tester's OP-FREE install of three engines, one preset refresh and a slot map took about two minutes (2026-11-11).
This note says why, what the loader does about it, and what device check H-IS1 showed.

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

1. **A backup reuses a fresh read-back** (`OpzOptions.reuseRead`, seconds; the OP-FREE site uses 5).
   - **When:** a flow's backup follows the previous flow's read-back by milliseconds. When nothing was pushed in
     between, that read-back *is* the device's state, so the backup takes it instead of opening another session.
   - **Safety:** any push drops it, and so does age. The default is 0 (always read), so `opz.py`, the web app and
     the recorded test vectors don't change.
   - **Saves:** one session (~7.5 s) per step [proven: H-IS1's log, `settings as read back 2.5 s ago`; test:
     `core/test/opz.test.ts` "read-back reuse"].
2. **One read-back for a whole batch** (`Opz.installMany`).
   - **Pushes:** one backup, then one push per engine (its file plus its own `patch.json`, the shape every single
     install uses [proven]), one push with every preset refresh (`plugs[]` only), and one read-back. The read-back
     checks every engine and plug record, and that the slot map didn't change; if it changed, the slot map is
     restored.
   - **The slot map:** it follows in its own push (`writeSlotMap`, whose backup takes the read-back).
   - **Why not one push for everything:** sending every engine in one push failed on the device (H-IS1, §3).
3. **A push has an overall time limit:** 30 s plus 1 s per 2 KB. When a push stops on it, or on the 15 s idle
   limit, the log ends with `push trace:` lines: the OP-Z's last `$35` requests and its part counts.

| Batch of N engines (+ slot map) | Sessions | About |
|---|---|---|
| before: `install` per engine, `presetsPush` per refresh, `writeSlotMap` | 3 per step | 22 s per step (the tester's five steps: ~1 min 50 s) |
| `installMany` + reuse (the OP-FREE site now) | **N + 4** (+1 with preset refreshes) | **~7.5 s per engine + ~30 s** [inferred from §1's timings] |

The site's test "a batch of N engines is N + 4 sessions" pins this
[test: `OP-FREE/site/src/loader/test/flow.test.ts`, 15 engines in the demo].

## 3. H-IS1: every engine in one push (device check, 2026-11-13): **no**

The first version of `installMany` sent every engine file and one `patch.json` in a single push. The push was 7
files: fmperc, swarm, clouds, swirl, plate and lofi, plus a `patch.json` with their six `auto_import[]` entries and
chip's presets. About 48 KB, ~240 parts of 200 bytes.

- **What happened:** the session opened. Then the OP-Z showed its transfer light (a blue sweep across the top row)
  for minutes, never reported the sync complete, and the site waited
  [proven: the tester's report, 2026-11-13 11:55:44 onward].
- **Why the site never gave up:** it only stopped a push after 15 s with no traffic, so the OP-Z must have kept
  sending requests or progress [inferred]. That's why pushes now have the overall time limit.
- **Recovery:** closing the tab ended the session. The sweep stopped, and the OP-Z worked normally.
- **What landed:** nothing. A read-back (`opz.py status`) showed the same state CRC `c85e860c`, plugs and slot map
  as before [proven].

**Cause: not known.** Every push before had at most 3 files.
- A single file of 230 parts (`table`, 45,844 B) installs fine [proven: installed 2026-11-11]. So the part count of
  one file isn't the limit.
- Two candidates [idea]:
  - The OP-Z limits how many files, or how many bytes, one sync job can carry.
  - Our flow control (`UploadState`: at most 64 parts in flight, counted as parts sent minus the OP-Z's reported
    count) disagrees with the OP-Z when its `$33` count runs per file rather than per job. Then we stop sending after
    a few files while the OP-Z keeps asking.
- **Q-88** asks which. The next stalled push's `push trace:` lines answer it.

## 4. Open

- **Q-86:** why ~6.5 s between sessions? Is there a close the OP-Z honours at once? If there is, every remaining
  session gets cheaper, the single-engine flows too.
- **Q-87:** in what order does the OP-Z apply the files of one sync job? If `patch.json` is imported before
  `slotConfiguration.json` is applied, the slot map could join the last engine's push and save two sessions.
- **Q-88:** why did a 7-file push never finish (§3)? Once a multi-file push is understood, a batch could drop back to
  ~5 sessions.
