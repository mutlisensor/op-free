/// <reference types="node" />
/**
 * Node's test runner has no Vite: resolve `@opz/core` as vite.config.ts does (OP-FREE/core/src when the sync has
 * copied it there, else the lab's web/core/src). Loaded with `node --import` before the tests.
 */

import { existsSync } from "node:fs";
import { registerHooks } from "node:module";

const publicCore = new URL("../../../../core/src/index.ts", import.meta.url);
const labCore = new URL("../../../../../web/core/src/index.ts", import.meta.url);
const core = existsSync(publicCore) ? publicCore : labCore;

registerHooks({
  resolve(specifier, context, next) {
    return next(specifier === "@opz/core" ? core.href : specifier, context);
  },
});
