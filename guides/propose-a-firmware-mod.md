# Propose a firmware mod

Some features can't be added as an engine: 64-step patterns, button debouncing, new arpeggiator styles, more CPU
for instruments. They need changes to teenage engineering's OS itself. This page is how such a change goes from an
idea to something others can apply to their own firmware. **Firmware mods are research only today**: none has
been run on a device, and the [patch format](#the-patch-format-draft) below is a draft.

A bad firmware image can leave an OP-Z that only starts in update mode. Every rule below exists to keep that
recoverable.

## 1. Check it really needs the firmware

Most ideas don't. If it can be done by an engine (a new sound or effect), or by a tool on the computer talking to
the OP-Z over USB (patterns, presets, projects), it belongs in [`engines/`](../engines/) or the tools, not here.
Say in your proposal why it can't.

## 2. Open an issue first

Use the **Firmware mod proposal** issue template before writing code or patches. Describe what the user would see,
which firmware version you studied (the research covers **1.2.45**), and what you already know. A maintainer gives
it a question number (Q-NN) in [OPEN_QUESTIONS.md](../research/OPEN_QUESTIONS.md) so the research has one place to
live.

## 3. Do the research, with evidence

Write a design note that explains, in your own words, what the OS does today and what would change:

- every factual claim carries an [evidence label](../research/README.md#evidence-labels): **[code]** with the
  function address and which binary, **[proven]** for something observed on a unit, **[inferred]** and **[idea]**
  for the rest;
- addresses, not code: **no decompiled or disassembled listings** beyond the few instructions needed to make a
  point, no extracted firmware files, tables or keys. Readers check your claims in their own disassembler against
  their own copy of the firmware ([get your firmware](extract-your-firmware.md));
- say what still needs a hardware check, written as a numbered step with its expected result and the way back.

Start from [how the firmware works](how-the-firmware-works.md), the [research index](../research/README.md) and
the [firmware map](../research/OPZ_FIRMWARE_MAP.md).

## 4. Write the recovery plan before the patch

Every mod needs, written down before anyone tries it:

- **the revert**: normally reinstalling teenage engineering's official firmware file in update mode
  ([recovery runbook](../research/crash-recovery.md));
- **what could go wrong** and how it would show (crash, freeze, a unit that starts only in update mode);
- **where it is tried first**: on a spare unit, never someone's main OP-Z, and only after the project has proven
  that the official file reinstalls after a deliberately bad image. Until then, mods are designed and checked
  offline only.

Never send commands to the update-mode serial console ([SAFETY.md](../SAFETY.md), rule 1).

## The patch format (draft)

OP-FREE publishes **patches, never firmware images**. A patch is a JSON record that a patcher applies to the user's
own copy of the OS image. The lab's prototype (`tools/opz_fwpatch.py`, not published yet) uses this shape; the
values below are placeholders:

```json
{
  "name": "example-mod",
  "status": "DRAFT, do not apply: recovery not yet proven on a spare unit",
  "description": "What the user gets, and the design note that explains it.",
  "source": {
    "file": "opz.ldr",
    "firmware": "1.2.45",
    "size": 123456,
    "sha256": "<SHA-256 of the unmodified file>",
    "crc32": 1234567890
  },
  "edits": [
    {
      "what": "one-line description of this change",
      "device_address": "0x8xxxxxxx",
      "file_offset": "0x...",
      "expected": "<the bytes that must be there now, hex>",
      "new": "<the bytes written instead, same length>",
      "evidence": "[code] why this is the right place and why the change is safe (addresses of the code that reads it; link to the design note)"
    }
  ],
  "revert": "Reinstall teenage engineering's official firmware in update mode.",
  "risk": "What can go wrong and how it would show."
}
```

The patcher refuses unless the source file matches **exactly** (size, SHA-256, CRC-32), every edit's expected bytes
are where the record says, and each file offset maps to the stated device address in the image's block layout. It
writes the result to a local file and never talks to an OP-Z. A record whose `status` starts with `DRAFT` is
refused. Packaging the patched OS as an installable update, and the browser builder that does all of this with
your own file ([how it will work](extract-your-firmware.md#4-how-the-firmware-builder-will-use-your-file)), are
not written yet.

Keep each patch small and about one thing. The smallest useful one we know of changes two 8-byte numbers (the CPU
overload line; [CPU headroom](../research/cpu-headroom.md)).

## 5. Pull request

When the research is done, open a pull request with the design note (in `firmware-mods/`), the question's answer in
[OPEN_QUESTIONS.md](../research/OPEN_QUESTIONS.md), and the patch record marked `DRAFT`. A record leaves draft only
after its hardware check has passed on a spare unit and the revert has been shown to work.

Planned mods and their status: [firmware-mods/](../firmware-mods/README.md).
