/**
 * The web simulator's view of the repository, served by Vite's dev server:
 *
 * - `GET /api/catalog`: every engines/NAME and sdk/templates/KIND with its plug.json, presets, scenarios and whether
 *   its wasm module is built (`make -C sim wasm` -> sim/build/wasm/{engines,templates}/NAME.wasm);
 * - `GET /files/PATH`: a file under engines/, sdk/templates/, sim/build/wasm/ or sim/build/samples/ (scenarios,
 *   golden hashes, sample packs, the modules). Nothing else in the repository is served.
 */

import { existsSync, readFileSync, readdirSync, statSync } from "node:fs";
import { extname, join, normalize, resolve, sep } from "node:path";
import type { Connect, Plugin } from "vite";

export const REPO = resolve(import.meta.dirname, "../..");
const SERVED = ["engines", "sdk/templates", "sim/build/wasm", "sim/build/samples"].map((p) => join(REPO, p) + sep);

export type Kind = "synth" | "drum" | "fx";

export interface CatalogEntry {
  /** "engines/swirl" or "templates/synth" */
  id: string;
  /** directory, relative to the repository */
  dir: string;
  /** module name opzsim prints (the engine directory; template_KIND for templates) */
  name: string;
  label: string;
  kind: Kind;
  template: boolean;
  plug: { engine: string; memoryslot: number; plug: { id: number; name: string; trackmask: number; flags: number; parameter_names: string[] } };
  presets: Array<{ name: string; values: number[]; slot?: number; index?: number }>;
  scenarios: string[];
  /** URL of the module, null until `make -C sim wasm` has built it */
  wasm: string | null;
  readme: string | null;
}

/** synth tracks 4-7, drum tracks 0-3, FX 8-9: the plug's trackmask decides what it is. */
export function kindOf(trackmask: number): Kind {
  if (trackmask & 0xf00) return "fx";
  if (trackmask & 0xf0) return "synth";
  return "drum";
}

function entry(dir: string, id: string, name: string, label: string, template: boolean, wasm: string): CatalogEntry | null {
  const abs = join(REPO, dir);
  const plugPath = join(abs, "plug.json");
  if (!existsSync(plugPath)) return null;
  const plug = JSON.parse(readFileSync(plugPath, "utf8"));
  const presetPath = typeof plug.presets === "string" ? join(abs, plug.presets) : null;
  const presets = presetPath && existsSync(presetPath) ? JSON.parse(readFileSync(presetPath, "utf8")) : [];
  const tests = join(abs, "tests");
  const scenarios = existsSync(tests) ? readdirSync(tests).filter((f) => f.endsWith(".scn")).sort() : [];
  const readme = join(abs, "README.md");
  return {
    id, dir, name, label, template, plug, presets, scenarios,
    kind: kindOf(plug.plug.trackmask),
    wasm: existsSync(join(REPO, wasm)) ? "/files/" + wasm : null,
    readme: existsSync(readme) ? readFileSync(readme, "utf8") : null,
  };
}

export function buildCatalog(): CatalogEntry[] {
  const out: CatalogEntry[] = [];
  for (const kind of ["synth", "drum", "fx"]) {
    const e = entry(`sdk/templates/${kind}`, `templates/${kind}`, `template_${kind}`, `${kind} template`, true,
      `sim/build/wasm/templates/${kind}.wasm`);
    if (e) out.push(e);
  }
  for (const dir of readdirSync(join(REPO, "engines")).sort()) {
    if (!statSync(join(REPO, "engines", dir)).isDirectory()) continue;
    const e = entry(`engines/${dir}`, `engines/${dir}`, dir, dir, false, `sim/build/wasm/engines/${dir}.wasm`);
    if (e) out.push(e);
  }
  return out;
}

const TYPES: Record<string, string> = {
  ".wasm": "application/wasm", ".json": "application/json", ".scn": "text/plain; charset=utf-8",
  ".sha256": "text/plain; charset=utf-8", ".md": "text/markdown; charset=utf-8", ".aif": "audio/aiff",
};

const middleware: Connect.NextHandleFunction = (req, res, next) => {
  const url = new URL(req.url ?? "/", "http://x");
  if (url.pathname === "/api/catalog") {
    res.setHeader("Content-Type", "application/json");
    res.setHeader("Cache-Control", "no-store");
    res.end(JSON.stringify(buildCatalog()));
    return;
  }
  if (url.pathname.startsWith("/files/")) {
    const path = normalize(join(REPO, decodeURIComponent(url.pathname.slice("/files/".length))));
    if (!SERVED.some((root) => path.startsWith(root)) || !existsSync(path) || !statSync(path).isFile()) {
      res.statusCode = 404;
      res.end("not found");
      return;
    }
    res.setHeader("Content-Type", TYPES[extname(path)] ?? "application/octet-stream");
    res.setHeader("Cache-Control", "no-store");
    res.end(readFileSync(path));
    return;
  }
  next();
};

export function catalogPlugin(): Plugin {
  return {
    name: "opz-websim-catalog",
    configureServer(server) {
      server.middlewares.use(middleware);
    },
    configurePreviewServer(server) {
      server.middlewares.use(middleware);
    },
  };
}
