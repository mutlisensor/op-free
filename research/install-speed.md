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

## 2. What the loader does now (all of it ran on the OP-Z, §3c)

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

## 3. H-IS1 on the OP-Z (2026-11-13)

### 3a. Every engine in one push: **no**

The first `installMany` sent every engine file and one `patch.json` in a single push: 7 files (fmperc, swarm,
clouds, swirl, plate, lofi and the patch), ~48 KB, ~240 parts of 200 bytes.

- **What happened:** the session opened, then the OP-Z showed its transfer light (a blue sweep across the top row)
  for minutes, never reported the sync complete, and the site waited with it [proven: tester's report, 11:55:44 on].
- **Recovery:** closing the page ended the session; the sweep stopped and the OP-Z worked normally. No power cycle.
- **What landed:** nothing. A read-back (`opz.py status`) gave the same state CRC `c85e860c`, plugs and slot map as
  before [proven].
- **Why the site waited:** it only gave up after 15 s with no traffic, so the OP-Z kept sending something. Pushes
  now have an overall time limit (§2.3).
- **Cause: not known.** A single file of 230 parts (`table`, 45,844 B) installs fine [proven, 2026-11-11], so one
  file's part count is not the limit. Either the OP-Z limits the files or bytes of one sync job, or our flow control
  (at most 64 parts in flight, counted as parts sent minus the OP-Z's reported count) disagrees with it when the
  count runs per file rather than per job [idea]. **Q-88.**

### 3b. An unfinished job outlives its session

The two attempts after the stuck push both failed at once with `unknown hosted file id`: `4061517770` is
`clouds.engine` and `3164615262` is `swirl.engine`, the 3rd and 4th files of the **stuck job**
[proven: the ids are `crc32` of those paths].

- The OP-Z kept the interrupted job's file list and asked for its next file in each new session, one per session,
  although the new push hosted neither [proven].
- It cleared by itself after a few sessions and ~3 minutes; the OP-Z was never restarted [proven].
- The loader now recognises such a request (`StaleJobError`): nothing of that push has gone out, so `Opz.push` opens
  a new session and tries again, up to 6 times, logging `trying again (n of 5)`. **Q-89** asks what exactly clears
  the list.

### 3c. A push per engine: **yes**

The same six engines, chip's presets and 10 slot changes, run again with one push per engine:

- **Every push was fetched and applied**: six `sync: requested ['engines/<name>.engine', 'settings/patch.json']`,
  then one for chip's presets, each with `OP-Z reports sync complete` [proven: 12:13:12–12:13:54].
- **One read-back verified all of them**: `installed and verified`, 18 user plugs, slot map unchanged at that point
  [proven: 12:14:01].
- **Sessions: 10 = N + 4** for N = 6, as the site's test predicts. The read reuse fired twice
  (`settings as read back 4.7 s ago`, `0.0 s ago`) [proven].
- **Time: 65 s** from the first push's session to `batch: done.` (12:13:10.6 → 12:14:16.1). Each session's wait was
  6.3 s, steady with §1 [proven].
- **The old flow for the same work** would be 3 sessions per engine + 3 for the presets + 3 for the slot map = 24
  sessions, about 3 minutes.

## 4. Open

- **Q-86:** why ~6.5 s between sessions? Is there a close the OP-Z honours at once? If there is, every remaining
  session gets cheaper, the single-engine flows too.
- **Q-87:** in what order does the OP-Z apply the files of one sync job? If `patch.json` is imported before
  `slotConfiguration.json` is applied, the slot map could join the last engine's push and save two sessions.
- **Q-88:** why did a 7-file push never finish (§3a)? Once a multi-file push is understood, a batch could drop back
  to ~5 sessions.
- **Q-89:** what clears an unfinished sync job (§3b)? Time, a number of sessions, or draining its file list? The
  answer sets how many retries `Opz.push` should make.
