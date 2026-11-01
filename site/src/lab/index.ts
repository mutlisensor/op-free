/**
 * The synth lab (workstream W1): sim/web retooled for the site. Placeholder until W1 lands; keep the export.
 */

import type { Mounted } from "../shared/mount.ts";

export function mountLab(el: HTMLElement): Mounted {
  el.innerHTML = `
    <div class="placeholder">
      <p class="label">Synth lab</p>
      <p>Play every engine in the browser: synth into FX1 and FX2, presets, on-screen keys and MIDI input.</p>
      <p class="dim">Being built.</p>
    </div>`;
  return { destroy() { el.replaceChildren(); } };
}
