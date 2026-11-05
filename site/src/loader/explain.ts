/**
 * Ported from web/app/src/explain.ts; the after-write advice names the loader's banner.
 *
 * What to tell the user when an operation fails: the core's error messages (SessionError and friends) and the
 * transport's, mapped to what probably happened, what state the OP-Z is in, and what to do next.
 */

import { OPEN_WAIT } from "./controller.ts";

export interface Advice {
  /** One line: what probably happened. */
  title: string;
  /** What to check or do, in order. */
  steps: string[];
  /** Whether the OP-Z's settings may have changed (a write was running): the app then offers a check against the backup. */
  checkDevice: boolean;
}

const WRITES = new Set(["install", "assign", "remove", "restore", "presets", "slots", "presets-all", "touch", "boot"]);

export function explain(error: string, op: string): Advice | null {
  const write = WRITES.has(op);
  const afterWrite = write
    ? ["Reconnect, then use “Check and put the backup back” in the banner: it shows what differs from the backup this operation made first and asks before writing."]
    : [];
  if (/MIDI port disconnected|MIDI connection closed|MIDI transport is closed/.test(error)) {
    return {
      title: "The OP-Z went away during the operation.",
      steps: [
        "Check the cable (or the Bluetooth pairing) and that the OP-Z is on and in normal mode.",
        "Files the OP-Z had not fully received are discarded: it only replaces a file after its CRC matches.",
        ...afterWrite,
      ],
      checkDevice: write,
    };
  }
  if (/no \$34 reply/.test(error)) {
    return {
      title: "The OP-Z did not answer the file service.",
      steps: [
        `It was asked twice (${OPEN_WAIT} s, then once more). The loader sends nothing until the OP-Z answers, so this step wrote nothing; the log says what came before.`,
        "Is TE's OP-Z app (or another tab of this app) connected? Close it: one client at a time.",
        "Is the OP-Z in normal mode (not content or update mode), and are the right ports picked?",
        "Unplug and replug the OP-Z, then connect again.",
      ],
      checkDevice: false,
    };
  }
  if (/no \$01 reply/.test(error)) {
    return {
      title: "The OP-Z did not answer the state-session hello.",
      steps: ["Close TE's app and any other tab of this app, check the ports, and try again."],
      checkDevice: false,
    };
  }
  if (/sync stalled/.test(error)) {
    return {
      title: "The upload stalled: the OP-Z stopped asking for parts.",
      steps: [
        "Keep this tab in front during writes and leave the OP-Z alone until it is done.",
        "Turn on trace SysEx and try again; save the trace for the hardware notes.",
        ...afterWrite,
      ],
      checkDevice: write,
    };
  }
  if (/incomplete: missing part|is not served/.test(error)) {
    return {
      title: "A settings file did not arrive completely.",
      steps: ["Read the status again. If it keeps failing, save a trace (trace SysEx) and the log."],
      checkDevice: write,
    };
  }
  if (/in use by another app/.test(error)) {
    return { title: "The MIDI port is busy.", steps: ["Close TE's app or the DAW holding the OP-Z's ports, then connect again."], checkDevice: false };
  }
  if (write) {
    return {
      title: "The operation stopped with an error.",
      steps: ["Read the log above for the last step that ran.", ...afterWrite],
      checkDevice: true,
    };
  }
  return null;
}
