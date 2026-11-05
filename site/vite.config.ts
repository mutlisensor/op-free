import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";
import { cataloguePlugin } from "./scripts/catalogue.ts";

// @opz/core (the loader's SysEx core): OP-FREE/core/src once the sync has copied it there (the public repo), else the
// lab's web/core/src. tsconfig.json's "paths" and src/loader/test/register.ts resolve it the same way.
const publicCore = fileURLToPath(new URL("../core/src", import.meta.url));
const labCore = fileURLToPath(new URL("../../web/core/src", import.meta.url));
const coreDir = existsSync(publicCore) ? publicCore : labCore;

// Relative base: the built site runs from any folder, including https://<user>.github.io/op-free/.
export default defineConfig({
  base: "./",
  plugins: [cataloguePlugin()],
  // the build the problem report names (src/shared/diag.ts): the commit on GitHub's runner, else "local"
  define: { __OPFREE_BUILD__: JSON.stringify((process.env.GITHUB_SHA ?? "local").slice(0, 7)) },
  resolve: { alias: { "@opz/core": `${coreDir}/index.ts` } },
  server: { port: 5180, fs: { allow: [".", coreDir] } },
  build: { target: "es2022" },
  worker: { format: "es" },
});
