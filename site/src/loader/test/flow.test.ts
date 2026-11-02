/// <reference types="node" />
/**
 * The loader's whole flow against the core's FakeOpz with the demo OP-Z's files: back up, install the picks (one
 * confirm, then the core's flows), what the OP-Z holds afterwards, cancel, original, restore, and the bake on the
 * demo content-mode disk behind the user-sample gate.
 */

import assert from "node:assert/strict";
import { test } from "node:test";
import { type Doc, FakeOpz, assign, dumps, loads, parseUserPresets, recordPlugId, slotCells, utf8 } from "../core.ts";
import { type LoaderConfirm, LoaderController } from "../controller.ts";
import { DEMO_USER_SAMPLES, DemoDisk, demoFiles } from "../demo.ts";
import { runBake } from "../disk.ts";
import { type Choice, allOriginal, defaultChoice, pickedDirs } from "../model.ts";
import { presetDocs } from "../plan.ts";
import { gateOpen, sampleGate } from "../samplegate.ts";
import { MemoryLoaderStore, firstBackup } from "../store.ts";
import { catalogue, placeholder } from "./fixtures.ts";

const engines = catalogue();

function setup(files = demoFiles()) {
  const fake = new FakeOpz(files);
  const store = new MemoryLoaderStore();
  const asked: LoaderConfirm[] = [];
  let answer = true;
  const lines: string[] = [];
  const c = new LoaderController(store, {
    log: (l) => lines.push(l),
    confirm: async (r) => {
      asked.push(r);
      return answer;
    },
    changed: () => {},
  });
  c.connect(fake, "demo", "demo", undefined, () => fake.now);
  return { fake, store, c, asked, lines, say: (yes: boolean) => void (answer = yes) };
}

const docs = (fake: FakeOpz) => ({ plugs: loads(fake.files.get("settings/plugs.json")!), slots: loads(fake.files.get("settings/slotConfiguration.json")!) });
const install = (c: LoaderController, choice: Choice) => c.install({ engines, choice, bytes: async () => placeholder() });

test("back up, then install the default picks: one confirm, every engine and slot verified", async () => {
  const { fake, store, c, asked } = setup();
  const before = docs(fake);
  assert.ok((await c.backup()).ok);
  const first = firstBackup(await store.list(), "demo");
  assert.ok(first, "the first backup of this OP-Z");

  const r = await install(c, defaultChoice(engines));
  assert.ok(r.ok, r.lines.join("\n"));
  assert.equal(asked.length, 1, "asked once for the whole batch");
  const sheet = asked[0];
  assert.equal(sheet.op, "install");
  assert.ok(sheet.backupId);
  assert.equal(sheet.installs.length, 15);
  assert.equal(sheet.changes.length, 4 * 8 + 4 * 3 + 4 + 3, "synths 4 tracks × 8, drums 4 × 3, fx1 4, fx2 3 (one engine is fx1 only)");
  assert.deepEqual(sheet.skipped, [], "no user sample sits where the default picks go");

  const after = docs(fake);
  const cells = slotCells(after.slots);
  for (const t of [4, 5, 6, 7]) for (let s = 1; s <= 8; s++) assert.equal(cells.get(`${t}:${s}`), 61009 + s);
  for (const [pid, s] of DEMO_USER_SAMPLES) assert.equal(cells.get(`3:${s}`), pid, "the visitor's samples stay");
  const installed = new Set(after.plugs.plugs.map((p: Doc) => p.id));
  for (const i of sheet.installs) assert.ok(installed.has(i.plugId), `${i.dir} installed`);
  for (const p of before.plugs.plugs) assert.ok(installed.has(p.id), `plug ${p.id} (TE's or the visitor's) is still installed`);
  assert.ok(fake.files.has("engines/syn1.engine"));
  assert.equal(c.device?.slots && slotCells(c.device.slots).get("4:1"), 61010, "the status is the read-back");

  const again = await install(c, defaultChoice(engines));
  assert.ok(again.ok);
  assert.equal(asked.length, 2, "the presets are pushed again (the OP-Z forgets them at a restart)");
  assert.equal(asked[1].changes.length, 0);
  assert.ok(asked[1].installs.every((i) => i.action === "ready"));
});

test("cancel at the sheet: nothing written", async () => {
  const { fake, c, say } = setup();
  await c.backup();
  const before = new Map(fake.files);
  say(false);
  const r = await install(c, defaultChoice(engines));
  assert.equal(r.ok, false);
  assert.ok(r.cancelled);
  assert.deepEqual([...fake.files.keys()].sort(), [...before.keys()].sort());
  assert.deepEqual(docs(fake), { plugs: loads(before.get("settings/plugs.json")!), slots: loads(before.get("settings/slotConfiguration.json")!) });
});

test("a refused plan writes nothing and asks nothing", async () => {
  const { fake, c, asked } = setup();
  await c.backup();
  const clash = engines.map((e) => (e.dir === "syn3" ? { ...e, manifest: { ...e.manifest, memoryslot: 27 } } : e));
  const before = new Map(fake.files);
  const r = await c.install({ engines: clash, choice: defaultChoice(engines), bytes: async () => placeholder() });
  assert.equal(r.ok, false);
  assert.equal(asked.length, 0);
  assert.ok(r.highlights.some((l) => /memory slot 27/.test(l)));
  assert.deepEqual([...fake.files.keys()].sort(), [...before.keys()].sort());
});

test("a missing engine file stops the batch before anything is asked or written", async () => {
  const { fake, c, asked } = setup();
  await c.backup();
  const before = new Map(fake.files);
  const r = await c.install({ engines, choice: defaultChoice(engines), bytes: async (e) => {
    if (e.dir === "syn5") throw new Error("syn5.engine: HTTP 404");
    return placeholder();
  } });
  assert.equal(r.ok, false);
  assert.equal(asked.length, 0);
  assert.deepEqual([...fake.files.keys()].sort(), [...before.keys()].sort());
});

test("original on every synth track puts TE's plugs back with a slot-map write only", async () => {
  const { fake, c, asked } = setup();
  const before = docs(fake);
  await c.backup();
  assert.ok((await install(c, defaultChoice(engines))).ok);
  let choice = defaultChoice(engines);
  for (const t of [4, 5, 6, 7]) choice = allOriginal(choice, t);
  const r = await install(c, choice);
  assert.ok(r.ok, r.lines.join("\n"));
  const sheet = asked.at(-1)!;
  assert.equal(sheet.changes.length, 32);
  assert.ok(sheet.installs.every((i) => !i.dir.startsWith("syn")));
  const cells = slotCells(docs(fake).slots);
  const orig = slotCells(before.slots);
  for (const t of [4, 5, 6, 7]) for (let s = 1; s <= 10; s++) assert.equal(cells.get(`${t}:${s}`), orig.get(`${t}:${s}`));
  assert.ok(docs(fake).plugs.plugs.some((p: Doc) => p.id === 61010), "our synth stays installed, out of its slots");
});

test("restore: the first backup's layout comes back through the core's writeSlotMap", async () => {
  const { fake, store, c, asked } = setup();
  const before = docs(fake);
  await c.backup();
  await install(c, defaultChoice(engines));
  const first = firstBackup(await store.list(), "demo")!;
  const r = await c.restore(first.id, engines.map((e) => e.manifest));
  assert.ok(r.ok, r.lines.join("\n"));
  const sheet = asked.at(-1)!;
  assert.equal(sheet.op, "restore");
  assert.ok(sheet.backupId && sheet.backupId !== first.id, "the restore backs up first");
  assert.ok(sheet.changes.length > 0);
  assert.ok(r.lines.includes("slot map written and verified."));
  const got = slotCells(docs(fake).slots);
  for (const [k, v] of slotCells(before.slots)) assert.equal(got.get(k), v, k);
});

test("restore keeps a sample the visitor made since the backup", async () => {
  const { fake, store, c } = setup();
  await c.backup();
  // the visitor records a new sample into kick slot 10 on the device
  const now = docs(fake);
  now.plugs.plugs.push({ engine: "engines/sampleplayergain.engine", flags: 2, id: 1_600_000_000, samples: "samplepacks/user/new.aif", version: 1 });
  fake.files.set("settings/plugs.json", utf8(dumps(now.plugs)));
  fake.files.set("settings/slotConfiguration.json", utf8(dumps(assign(now.slots, 0, 10, 1_600_000_000))));
  const first = firstBackup(await store.list(), "demo")!;
  const r = await c.restore(first.id);
  assert.ok(r.ok, r.lines.join("\n"));
  assert.equal(slotCells(docs(fake).slots).get("0:10"), 1_600_000_000);
});

test("content mode: the gate, then the bake of the picks' presets onto the demo disk", async () => {
  const { c } = setup();
  await c.backup();
  await install(c, defaultChoice(engines));
  const gate = sampleGate({ midi: true, device: c.device, last: null, confirmed: false });
  assert.equal(gate.kind, "checked");
  assert.ok(gate.kind === "checked" && gate.check.total === 3);
  assert.ok(gateOpen(gate));

  // a sample in no slot blocks it
  const loose = structuredClone(c.device!);
  loose.slots.tracks = loose.slots.tracks.map((t: Doc) => (t.index === 3 ? { ...t, packs: t.packs.filter((p: Doc) => p.slot !== 4) } : t));
  const blocked = sampleGate({ midi: true, device: loose, last: null, confirmed: false });
  assert.equal(blocked.kind, "blocked");
  assert.equal(gateOpen(blocked), false);

  const disk = new DemoDisk();
  const picked = defaultChoice(engines);
  const bake = pickedDirs(picked).map((dir) => engines.find((e) => e.dir === dir)!)
    .map((e) => ({ name: e.dir, manifest: e.manifest, presets: presetDocs(e) }));
  const backups: Uint8Array[] = [];
  const r = await runBake(disk, { kind: "bake", engines: bake, overwriteUser: false }, {
    log: () => {}, backup: async (data) => (backups.push(data), "b1"), confirm: async () => true,
  });
  assert.ok(r.ok && r.written, r.lines.join("\n"));
  assert.equal(backups.length, 1, "backed up before writing");
  const records = parseUserPresets(disk.files.get("config/userPresets.dat")!);
  const ids = new Set(records.map(recordPlugId).filter(Boolean));
  for (const e of bake) assert.ok(ids.has(e.manifest.plug.id), `${e.name} baked`);

  const put = await runBake(disk, { kind: "restore", data: backups[0], source: "b1" }, { log: () => {}, backup: async () => "b2", confirm: async () => true });
  assert.ok(put.ok && put.written);
  assert.ok(parseUserPresets(disk.files.get("config/userPresets.dat")!).every((rec) => recordPlugId(rec) === 0), "put back");
});
