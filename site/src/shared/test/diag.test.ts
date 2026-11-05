/// <reference types="node" />
/** The problem log: saved line by line, survives a reload, bounded, and the report holds the sections and the log. */

import assert from "node:assert/strict";
import { test } from "node:test";
import { DIAG_KEYS, DiagLog, MAX_CHARS, MAX_LINES, type StorageLike, report, reportPart } from "../diag.ts";

class MemoryStorage implements StorageLike {
  data = new Map<string, string>();
  writes = 0;
  getItem(k: string) { return this.data.get(k) ?? null; }
  setItem(k: string, v: string) { this.writes++; this.data.set(k, v); }
  removeItem(k: string) { this.data.delete(k); }
}

const at = () => new Date(2026, 10, 5, 9, 8, 7, 6);

test("every line is saved as it is written, and a new visit reads them back", () => {
  const store = new MemoryStorage();
  const a = new DiagLog(store, at);
  a.mark("page opened");
  a.note("lab", "asked to play swirl");
  assert.equal(store.writes, 2, "saved at once, not on close: a frozen tab keeps what came before");
  const b = new DiagLog(store, at);
  assert.equal(b.lines.length, 2);
  assert.match(b.lines[1], /^09:08:07\.006 lab +asked to play swirl$/);
  b.note("error", "one\n\ntwo");
  assert.deepEqual(b.lines.slice(2).map((l) => l.slice(20)), ["one", "two"], "one line each, blank lines dropped");
});

test("bounded by lines and characters, oldest dropped first", () => {
  const store = new MemoryStorage();
  const log = new DiagLog(store, at);
  for (let i = 0; i < MAX_LINES + 50; i++) log.note("t", `line ${i}`);
  assert.equal(log.lines.length, MAX_LINES);
  assert.match(log.lines[0], /line 50$/);
  const big = new DiagLog(new MemoryStorage(), at);
  for (let i = 0; i < 200; i++) big.note("t", "x".repeat(1500));
  assert.ok(big.lines.join("\n").length <= MAX_CHARS);
});

test("broken or blocked storage: the log still works for the visit", () => {
  const junk = new MemoryStorage();
  junk.setItem(DIAG_KEYS.log, "{not json");
  assert.deepEqual(new DiagLog(junk, at).lines, []);
  const full: StorageLike = { getItem: () => null, setItem: () => { throw new Error("QuotaExceeded"); }, removeItem: () => {} };
  const log = new DiagLog(full, at);
  log.note("t", "kept in memory");
  assert.equal(log.lines.length, 1);
});

test("the report: browser, each module's section, then the log", () => {
  const drop = reportPart("loader", () => ["connection demo OP-Z", "bass 1:swirl"]);
  const broken = reportPart("broken", () => { throw new Error("boom"); });
  const text = report({ url: "https://x/#/synths", userAgent: "TestBrowser/1" }, ["a", "b"]);
  drop();
  broken();
  assert.match(text, /^OP-FREE problem report\n/);
  assert.match(text, /browser TestBrowser\/1/);
  assert.match(text, /\[loader\]\nconnection demo OP-Z\nbass 1:swirl/);
  assert.match(text, /\[broken\]\n\(failed: boom\)/, "a failing section doesn't stop the report");
  assert.match(text, /\[log: 2 lines, oldest first\]\na\nb$/);
  assert.doesNotMatch(report({ url: "u", userAgent: "u" }, []), /\[loader\]/, "removed with its module");
});
