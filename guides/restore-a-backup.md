# Restoring a backup of your OP-Z

How to put an old backup of your OP-Z (a copy of its content-mode disk: `samplepacks`, `projects`, …) back on, with
or without OP-FREE installed, and **without a factory reset**.

## The short version

1. **Copy the OP-Z disk as it is now** to your computer first (content mode: hold *track* while switching on).
2. **Take OP-FREE off**: switch on normally, connect on the site, step 5, *Take OP-FREE off…*. One sheet shows every
   slot that changes; teenage engineering's sounds go back where your first backup had them. Your samples, projects
   and presets are not touched.
3. **Check the backup**: step 5, *Check a backup folder…*. It reads only file names and sizes and tells you whether
   the OP-Z will take it.
4. **Copy it back in content mode, replacing folders** rather than adding files next to what is there. If the
   computer says there isn't enough space, delete the old files on the OP-Z disk for good first (empty the Trash
   while the disk is still connected), then copy. Eject.
5. Install OP-FREE again whenever you like.

## Why a backup can fail to go back on

The OP-Z has three limits when it takes files from its disk, and it doesn't warn about any of them on the device:

| Limit | What fills it | OP-FREE |
|---|---|---|
| **24 MB of samples** | your own samples (a WAV counts at its full size) | uses none |
| **33 MB disk** in content mode | your samples, your projects (about 335 KB each) | uses none: our sounds are empty placeholder files there |
| **200 sound definitions** | every sound: about 49 of teenage engineering's, each of ours, each of your samples | one per OP-FREE sound (about 40 for all of them) |

A sample refused for memory simply isn't there after the eject (it is not put in `rejected`). A sample refused
because the definitions are full is even quieter: the OP-Z's log says *SUCCESS* and then removes it. The log
(`import.log`, at the top of the OP-Z disk) is written at the **next** content mode.

Two things to know when OP-FREE is installed: if a sample you copy into a slot that holds an OP-FREE sound is
refused, the OP-FREE sound stays there; and a backup made **while** OP-FREE was installed contains small
`~61020.engi`-style files, which make a slot silent on an OP-Z that no longer has that sound. The check in step 5
finds both.

## The factory reset is safe, but you rarely need it

In update mode, holding *screen* + *stop* for a second resets the OP-Z (manual p.64): it ends with a blinking white
light and four green ones. It is teenage engineering's own recovery and leaves the OP-Z like new, so it is safe. It
also removes **everything custom**: projects, samples, presets you stored, and every OP-FREE sound. Afterwards copy
your backup back in content mode, then install OP-FREE again from the site. If that backup was made while OP-FREE was
installed, delete its `~6xxxx.engi` files first (or install OP-FREE before you restore it).

The evidence behind each claim is in the lab repository's `docs/roadmap/restore-space.md`.
