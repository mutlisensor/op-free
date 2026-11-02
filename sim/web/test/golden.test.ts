/**
 * The web simulator renders bit-identically to opzsim: every engines/NAME/tests/*.scn, run through the wasm build
 * (sim/build/wasm/engines/NAME.wasm, `make -C sim wasm`) over the in-memory WASI file system, must hash to the same
 * SHA-256 as its golden file (tests/golden/SCENARIO.wav.sha256, written by the native opzsim) and pass its `expect`
 * lines. Skips when the wasm build is missing.
 */

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { existsSync, readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { problems, runScenario, scenarioDependencies } from "../src/opzsim.ts";

const REPO = join(import.meta.dirname, "../../..");
const WASM = join(REPO, "sim/build/wasm/engines");
const built = existsSync(WASM);

/** The repository as the scenario sees it: absolute path "/x" is REPO/x. */
function load(files: Map<string, Uint8Array>, path: string): void {
  const disk = join(REPO, path);
  if (existsSync(disk)) files.set(path, readFileSync(disk));
}

const engines = built ? readdirSync(join(REPO, "engines")).filter((d) => existsSync(join(REPO, "engines", d, "plug.json"))).sort() : [];

test("wasm build present (make -C sim wasm)", { skip: built ? false : "no sim/build/wasm: run make -C sim wasm" }, () => {
  for (const name of engines) assert.ok(existsSync(join(WASM, name + ".wasm")), `${name}.wasm missing`);
});

for (const name of engines) {
  test(`${name}: every scenario hashes as opzsim's golden`, () => {
    const plug = JSON.parse(readFileSync(join(REPO, "engines", name, "plug.json"), "utf8"));
    const module = new WebAssembly.Module(readFileSync(join(WASM, name + ".wasm")));
    const tests = join(REPO, "engines", name, "tests");
    const scenarios = readdirSync(tests).filter((f) => f.endsWith(".scn")).sort();
    assert.ok(scenarios.length > 0, "no scenarios");
    for (const scn of scenarios) {
      const stem = scn.slice(0, -4);
      const path = `/engines/${name}/tests/${scn}`;
      const files = new Map<string, Uint8Array>();
      load(files, path);
      for (const dep of scenarioDependencies(path, readFileSync(join(REPO, path), "utf8"))) load(files, dep);
      const r = runScenario(module, { name, scenario: path, files, slot: plug.memoryslot, trackmask: plug.plug.trackmask });
      assert.equal(r.exitCode, 0, `${stem}: opzsim exit ${r.exitCode}\n${r.log}`);
      assert.deepEqual(problems(r.log), [], `${stem}: warnings\n${r.log}`);
      const want = readFileSync(join(tests, "golden", stem + ".wav.sha256"), "utf8").split(/\s+/)[0];
      const got = createHash("sha256").update(r.wav!).digest("hex");
      assert.equal(got, want, `${stem}: wasm render differs from opzsim's golden`);
    }
  });
}
