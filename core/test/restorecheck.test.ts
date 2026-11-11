/** The content-mode restore check against tools/tests/fixtures/restore-check.json: the same lines as opz_restorecheck.py. */

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { describe, it } from "node:test";

import { checkContentRestore, describeRestoreCheck, parseImportLog } from "../src/index.ts";

const fixture = JSON.parse(readFileSync(new URL("../../../tools/tests/fixtures/restore-check.json", import.meta.url), "utf8"));

describe("content-mode restore check", () => {
  for (const s of fixture.scenarios) {
    it(`matches opz.py restore-check: ${s.name}`, () => {
      assert.deepEqual(describeRestoreCheck(checkContentRestore(s.backup, s.device, s.disk)), s.expected);
    });
  }

  it("classifies like the importer", () => {
    const s = fixture.scenarios[0];
    const r = checkContentRestore(s.backup, s.device, s.disk);
    assert.equal(r.unchanged, 3);
    assert.deepEqual(r.imports.map((e) => e.path), ["samplepacks/1-kick/03/new.aif", "samplepacks/2-snare/01/big.wav"]);
    assert.deepEqual(r.dangling, [{ path: "samplepacks/5-bass/03/~61016.engi", id: 61016 }]);
    assert.deepEqual(r.ourSlots, [[0, 4], [4, 1]]);
    assert.equal(r.keptBytes, 500000);
    assert.equal(r.freeDefinitions, 195);
    assert.ok(r.removeOursHelps);
  });

  it("reads import.log like opz_restorecheck.parse_import_log", () => {
    const got = parseImportLog(fixture.import_log.text);
    const want = fixture.import_log.expected;
    assert.deepEqual({ used: got.used, memory_full: got.memoryFull, failed: got.failed, imports: got.imports }, want);
  });
});
