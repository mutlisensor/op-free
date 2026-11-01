/**
 * The loader (workstream W2): pick the sounds, back up, install over USB MIDI, then content mode for presets and
 * slots. Placeholder until W2 lands; keep the export.
 */

import type { Mounted } from "../shared/mount.ts";

export function mountLoader(el: HTMLElement): Mounted {
  el.innerHTML = `
    <div class="placeholder">
      <p class="label">Your OP-Z</p>
      <p>Choose what goes on your OP-Z: eight sounds per track group are ticked for you. Community or original.</p>
      <p class="dim">Being built.</p>
    </div>`;
  return { destroy() { el.replaceChildren(); } };
}
