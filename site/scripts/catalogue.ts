/**
 * The engine catalogue, written at dev and build time: a Vite plugin that serves (dev) or emits (build)
 * `catalogue.json` in the shape of src/shared/catalogue.ts, plus the modules it points at.
 *
 * Where things come from, each looked up per engine:
 * - sources (plug.json, presets, README): `OP-FREE/engines/<dir>/` once tools/opfree_sync.py has run, else the lab's
 *   `engines/<dir>/`;
 * - the WebAssembly module the lab plays: `public/modules/<dir>.wasm` (synced), else the lab's
 *   `sim/build/wasm/engines/<dir>.wasm` (`make -C sim wasm`);
 * - the Blackfin `.engine` the loader installs: `public/modules/<dir>.engine` (synced), else the lab's
 *   `build/bfin/<dir>/<dir>.engine` (`tools/opz_build.py`).
 *
 * Files under public/ are served and copied by Vite itself. Fallback files from the lab are served by the dev server
 * from where they are and emitted into dist/ by the build: never copied into the source tree, never into git.
 * Released engines only: `opz_test_*` folders are skipped.
 */

import { existsSync, readFileSync, readdirSync, statSync } from "node:fs";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import type { Connect, Plugin } from "vite";
import type { Catalogue, CatalogueEngine, Kind, Preset } from "../src/shared/catalogue.ts";

const SITE = fileURLToPath(new URL("..", import.meta.url));
const OPFREE = resolve(SITE, "..");
const LAB = resolve(OPFREE, "..");
const PUBLIC_MODULES = join(SITE, "public", "modules");

/**
 * The default selection: eight per group, the best-tested on hardware (docs/STATUS.md). Only six drum engines are
 * released so far, so all six are featured. `macro` and `fold` are left out: both have crashed or frozen the OP-Z.
 */
export const FEATURED: Record<Kind, string[]> = {
  synth: ["table", "modal", "vowel", "super", "pluck", "dx", "fm2", "chip"],
  drum: ["kit", "fmperc", "mallet", "hats", "8bit", "snap"],
  fx: ["swirl", "filter", "echo", "plate", "resonator", "glue", "clouds", "shimmer"],
};

/** Engines that have hurt a real OP-Z: shown on their card, and the loader should warn before installing them. */
export const CAUTION: Record<string, string> = {
  macro: "Crashed the OP-Z when P1 or P2 moved while notes played. Being fixed: don't install it yet.",
  fold: "Froze the OP-Z twice in testing. Being investigated: don't install it yet.",
};

function enginesRoot(): string {
  const synced = join(OPFREE, "engines");
  const has = existsSync(synced) && readdirSync(synced).some((d) => existsSync(join(synced, d, "plug.json")));
  return has ? synced : join(LAB, "engines");
}

/** Where each module file comes from: a published URL path ("modules/x.wasm") -> a file on disk. */
function moduleSources(dir: string): { wasm: string | null; engine: string | null } {
  const pick = (...paths: string[]) => paths.find((p) => existsSync(p)) ?? null;
  return {
    wasm: pick(join(PUBLIC_MODULES, `${dir}.wasm`), join(LAB, "sim", "build", "wasm", "engines", `${dir}.wasm`)),
    engine: pick(join(PUBLIC_MODULES, `${dir}.engine`), join(LAB, "build", "bfin", dir, `${dir}.engine`)),
  };
}

/**
 * synth tracks 4-7, drum tracks 0-3, FX 8-9: the plug's trackmask decides what it is. Synths and drums stay on tracks
 * 1-8 and effects on 9-10, as on a stock OP-Z: a trackmask that crosses that line (or names no track, or a track past
 * FX2) is untested, so it gets null and the engine is left out of the catalogue.
 */
function kindOf(trackmask: number): Kind | null {
  if (!trackmask || trackmask & ~0x3ff) return null;
  if (trackmask & 0x300) return trackmask & 0xff ? null : "fx";
  return trackmask & 0xf0 ? "synth" : "drum";
}

/** Inline Markdown to plain text: **bold**, `code`, [links](…); the lab's backlog ids ("(X-04)") are dropped. */
function plain(s: string): string {
  return s.replace(/\*\*([^*]+)\*\*/g, "$1").replace(/`([^`]+)`/g, "$1").replace(/\[([^\]]+)\]\([^)]*\)/g, "$1")
    .replace(/\s*\([XQH]-\d+\)/g, "").trim();
}

/** The README's first paragraph, the P1/P2 rows of its first control table and its "listen" list. */
function readme(text: string | null): { summary: string | null; help: CatalogueEngine["help"]; author: string | null } {
  if (!text) return { summary: null, help: null, author: null };
  const lines = text.replace(/\r\n?/g, "\n").split("\n");
  const para: string[] = [];
  for (const line of lines) {
    const t = line.trim();
    if (t.startsWith("# ")) continue;
    if (!t) {
      if (para.length) break;
      continue;
    }
    if (/^[|#`-]/.test(t)) break;
    para.push(t);
  }
  const help: NonNullable<CatalogueEngine["help"]> = {};
  for (const line of lines) {
    const m = /^\|\s*(P[12])\b(.*?)\|(.*)\|\s*$/.exec(line.trim());
    if (!m) continue;
    const key = m[1].toLowerCase() as "p1" | "p2";
    const desc = plain(m[3].split("|")[0]);
    help[key] ??= desc;
  }
  const at = lines.findIndex((l) => /^##\s+.*listen/i.test(l));
  if (at >= 0) {
    const items: string[] = [];
    for (const line of lines.slice(at + 1)) {
      if (/^##\s/.test(line)) break;
      const t = line.trim();
      if (/^[-*]\s/.test(t)) items.push(plain(t.slice(2)));
      else if (t && items.length && !/^[|#]/.test(t)) items[items.length - 1] += " " + plain(t);
    }
    if (items.length) help.listen = items;
  }
  const author = /^(?:author|by)\s*:\s*(.+)$/im.exec(text)?.[1] ?? null;
  return { summary: para.length ? plain(para.join(" ")) : null, help: Object.keys(help).length ? help : null, author };
}

export interface Built {
  catalogue: Catalogue;
  /** published path ("modules/fm2.wasm") -> file on disk, for files that aren't under public/ already */
  extra: Map<string, string>;
}

export function buildCatalogue(): Built {
  const root = enginesRoot();
  const engines: CatalogueEngine[] = [];
  const extra = new Map<string, string>();
  const featured = new Set(Object.values(FEATURED).flat());
  for (const dir of readdirSync(root).sort()) {
    if (dir.startsWith("opz_test_")) continue;
    const abs = join(root, dir);
    const plugPath = join(abs, "plug.json");
    if (!statSync(abs).isDirectory() || !existsSync(plugPath)) continue;
    const manifest = JSON.parse(readFileSync(plugPath, "utf8")) as CatalogueEngine["manifest"] & { author?: string };
    const presetPath = typeof manifest.presets === "string" ? join(abs, manifest.presets) : null;
    const presets: Preset[] = presetPath && existsSync(presetPath) ? JSON.parse(readFileSync(presetPath, "utf8")) : [];
    const readmePath = join(abs, "README.md");
    const info = readme(existsSync(readmePath) ? readFileSync(readmePath, "utf8") : null);
    const kind = kindOf(manifest.plug.trackmask);
    if (!kind) {
      console.warn(`catalogue: ${dir} left out: trackmask 0x${manifest.plug.trackmask.toString(16)} mixes synth or drum tracks with FX tracks`);
      continue;
    }
    const src = moduleSources(dir);
    const url = (file: string | null, ext: string) => {
      if (!file) return null;
      const published = `modules/${dir}.${ext}`;
      if (!file.startsWith(PUBLIC_MODULES)) extra.set(published, file);
      return published;
    };
    engines.push({
      dir,
      label: manifest.plug.name || dir,
      kind,
      manifest,
      presets,
      wasm: url(src.wasm, "wasm"),
      engine: url(src.engine, "engine"),
      summary: info.summary,
      help: info.help,
      author: manifest.author ?? info.author,
      featured: featured.has(dir),
      caution: CAUTION[dir] ?? null,
    });
  }
  return { catalogue: { generated: new Date().toISOString(), engines }, extra };
}

const TYPES: Record<string, string> = { wasm: "application/wasm", engine: "application/octet-stream" };

/** Vite plugin: `catalogue.json` and the fallback modules, in dev and in the build. */
export function cataloguePlugin(): Plugin {
  let base = "/";
  const middleware: Connect.NextHandleFunction = (req, res, next) => {
    const path = decodeURIComponent((req.url ?? "/").split("?")[0]);
    const rel = path.startsWith(base) ? path.slice(base.length) : path.replace(/^\//, "");
    if (rel === "catalogue.json") {
      res.setHeader("Content-Type", "application/json");
      res.setHeader("Cache-Control", "no-store");
      res.end(JSON.stringify(buildCatalogue().catalogue));
      return;
    }
    if (rel.startsWith("modules/")) {
      const file = buildCatalogue().extra.get(rel);
      if (file) {
        res.setHeader("Content-Type", TYPES[rel.split(".").pop()!] ?? "application/octet-stream");
        res.setHeader("Cache-Control", "no-store");
        res.end(readFileSync(file));
        return;
      }
    }
    next();
  };
  return {
    name: "opfree-catalogue",
    configResolved(config) {
      base = config.base.startsWith("/") ? config.base : "/";
    },
    configureServer(server) {
      server.middlewares.use(middleware);
    },
    configurePreviewServer(server) {
      server.middlewares.use(middleware);
    },
    generateBundle() {
      const { catalogue, extra } = buildCatalogue();
      this.emitFile({ type: "asset", fileName: "catalogue.json", source: JSON.stringify(catalogue, null, 1) });
      for (const [fileName, file] of extra) this.emitFile({ type: "asset", fileName, source: readFileSync(file) });
      const n = (k: Kind) => catalogue.engines.filter((e) => e.kind === k).length;
      const playable = catalogue.engines.filter((e) => e.wasm).length;
      this.info?.(`catalogue: ${n("synth")} synths, ${n("drum")} drums, ${n("fx")} effects; ${playable} playable`);
    },
  };
}
