# Using OP-FREE

How to try the community synths and effects in your browser and put the ones you like on your own OP-Z. The
website walks you through every step; this page says what each step does, so you know what to expect.

> OP-FREE is an independent community project, not affiliated with teenage engineering. New engines run inside the
> OP-Z's own OS with no memory protection: a faulty one can crash the unit until it is removed. Read the risk notice
> on the site, and [SAFETY.md](../SAFETY.md) if you want the details.

## What you need

- An OP-Z with **firmware 1.2.45** (the version everything here was built and tested against).
- A **USB cable** that carries data, and a computer.
- A browser with **Web MIDI**: Chrome or Edge on macOS, Windows, Linux or ChromeOS (see [browser
  support](#browser-support)). Trying sounds works in any modern browser; only connecting needs Web MIDI.
- **Close teenage engineering's OP-Z app** first: two programs talking to the OP-Z at once get in each other's way.

## The six steps

### 1. Try

Open the synth lab and play every engine in the browser: the synth, then two effects after it, with its presets,
the on-screen keyboard or a MIDI keyboard. This runs the engines' real code, compiled for the browser. Nothing
touches your OP-Z.

### 2. Pick

Choose what goes on your OP-Z: up to 10 sounds per track group. Eight are ticked by default, so two slots stay
free for your own samples. A toggle switches each slot between a **community** engine and the **original** OP-Z
sound. Choosing *original* only moves teenage engineering's own sound back into that slot; it is never uploaded
(every OP-Z already has it), and it can't be previewed in the browser because we don't have its code.

### 3. Connect and back up

Switch the OP-Z on normally, plug it in, and press *Connect*; the browser asks for permission to use MIDI devices.
The site then **backs up** the OP-Z's sound settings (which plugs are installed, which sound sits in which slot) and
keeps the backup in your browser. You can also download it. Every later step can be undone from this backup.

### 4. Install

The site shows the plan (what will be uploaded, which slots change) and asks you to confirm. It then sends each
engine over USB MIDI, the same file service the official app uses, and reads every file back to check it arrived
intact. If anything fails it puts the backup back. The OP-Z may restart once while it loads new engines.

### 5. Content mode (presets and slots)

Presets sent over USB only last until the next restart. To make them stick, the site asks you to switch the OP-Z
off and back on **holding *track***: it appears on your computer as a disk called *OP-Z Disk*. The site then
writes your presets into the OP-Z's own preset store and can arrange which slot each sound sits in. It only changes
the records of our sounds; presets you stored by hand stay as they are.

> In content mode the OP-Z deletes user samples that sit in no slot. Before you use it, make sure every sample of
> your own is in a slot.

### 6. Eject and play

Eject the disk (or press *play* on the OP-Z). It restarts with your sounds and presets. Pick a track, choose a slot
with the black keys, and play.

## Restore

*Restore* (on the site, at any time) uploads the backup from step 3: the slot map goes back to what it was and our
sounds leave their slots. Engine files themselves stay on the OP-Z's storage, out of any slot, where they do
nothing; a later install reuses them.

If something goes wrong:

| What you see | What to do |
|---|---|
| A track is silent after installing | Choose another sound in that slot, then *Restore* |
| The OP-Z restarts when you play a sound | Switch it off and on, connect, and *Restore* before playing again |
| It freezes | Hold the power switch to turn it off, then as above |
| It crashes at every start | [SAFETY.md](../SAFETY.md), *If something goes wrong*, and the [recovery runbook](../research/crash-recovery.md) |

Please report a crash as an issue (*Bug report*): which engine, which track, what you were doing.

## Browser support

| Browser | Try sounds | Connect and install |
|---|---|---|
| Chrome, Edge (desktop) | yes | yes |
| Other Chromium browsers (Opera, Brave, Arc) | yes | usually; they need Web MIDI with SysEx enabled |
| Firefox | yes | not supported by the site yet |
| Safari (macOS and iOS) | yes | no: Safari has no Web MIDI |
| Phones and tablets | yes | no |

The site needs no account and has no server of its own (static pages on GitHub Pages): the backup of your OP-Z
stays in your browser and in the file you download.

## Your own engines

Everything on the site is built with the [SDK](../sdk/docs/QUICKSTART.md). To add yours, see [submit an
engine](submit-an-engine.md).
