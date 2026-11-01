/**
 * The engine catalogue: the contract between the synth lab (src/lab), the loader (src/loader) and the build step
 * that writes `public/catalogue.json` from `OP-FREE/engines/` and the built modules. It merges what sim/web's
 * catalog (wasm, scenarios) and web/app's catalogue (the built .engine, help) each carry.
 */

export type Kind = "synth" | "drum" | "fx";

export interface Preset {
  name: string;
  values: number[];
  slot?: number;
  index?: number;
  octave?: number;
}

export interface CatalogueEngine {
  /** The engine's folder name, e.g. "swirl". */
  dir: string;
  /** What the page shows. */
  label: string;
  kind: Kind;
  /** plug.json as written by the engine's author. */
  manifest: {
    engine: string;
    memoryslot: number;
    version?: number;
    presets?: string;
    plug: { id: number; name: string; trackmask: number; flags: number; parameter_names: string[] };
  };
  presets: Preset[];
  /** URL (relative to the site) of the WebAssembly module the lab plays; null when not built. */
  wasm: string | null;
  /** URL of the Blackfin `.engine` the loader installs; null when not built (the loader can't install it). */
  engine: string | null;
  /** One-line description and the P1/P2 help, from the engine's README. */
  summary: string | null;
  help: { p1?: string; p2?: string; listen?: string[] } | null;
  /** Author credit, from plug.json's optional "author" or the README. */
  author: string | null;
  /** True for engines on the default selection (the eight ticked per track group). */
  featured?: boolean;
}

export interface Catalogue {
  generated: string;
  engines: CatalogueEngine[];
}

let cached: Promise<Catalogue> | null = null;

/** Loads `catalogue.json` once. */
export function loadCatalogue(): Promise<Catalogue> {
  cached ??= fetch(new URL("catalogue.json", document.baseURI)).then((r) => {
    if (!r.ok) throw new Error(`catalogue.json: HTTP ${r.status}`);
    return r.json() as Promise<Catalogue>;
  });
  return cached;
}
