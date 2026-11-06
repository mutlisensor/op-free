/// <reference types="node" />
/**
 * The loader's whole flow against the core's FakeOpz with the demo OP-Z's files: back up, install the picks (one
 * confirm, then the core's flows), what the OP-Z holds afterwards, cancel, original, restore, and the bake on the
 * demo content-mode disk behind the user-sample gate.
 */

import assert from "node:assert/strict";
import { test } from "node:test";
import { type Doc, FakeOpz, type Transport, assign, dumps, loads, parseUserPresets, recordPlugId, slotCells, storedUserPresets, utf8 } from "../core.ts";
import { type LoaderConfirm, LoaderController, OPEN_WAIT, type OpenWait } from "../controller.ts";
import { DEMO_USER_SAMPLES, DemoDisk, demoFiles } from "../demo.ts";
import { type BakeConfirm, emptyUserPresets, runBake } from "../disk.ts";
import { type Choice, allOriginal, defaultChoice, featuredChoice, pickedDirs } from "../model.ts";
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

  const r = await install(c, featuredChoice(engines));
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

  const again = await install(c, featuredChoice(engines));
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
  const r = await install(c, featuredChoice(engines));
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
  const r = await c.install({ engines: clash, choice: featuredChoice(engines), bytes: async () => placeholder() });
  assert.equal(r.ok, false);
  assert.equal(asked.length, 0);
  assert.ok(r.highlights.some((l) => /memory slot 27/.test(l)));
  assert.deepEqual([...fake.files.keys()].sort(), [...before.keys()].sort());
});

test("a missing engine file stops the batch before anything is asked or written", async () => {
  const { fake, c, asked } = setup();
  await c.backup();
  const before = new Map(fake.files);
  const r = await c.install({ engines, choice: featuredChoice(engines), bytes: async (e) => {
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
  assert.ok((await install(c, featuredChoice(engines))).ok);
  let choice = featuredChoice(engines);
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
  await install(c, featuredChoice(engines));
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
  await install(c, featuredChoice(engines));
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
  const picked = featuredChoice(engines);
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

/**
 * The OP-Z stops answering session opens: from its `from`-th `$34` (0 = the first) every `$34` is lost for `seconds`
 * of its clock (the re-sent adverts included).
 */
function setupUnanswered(from: number, seconds = Infinity) {
  const fake = new FakeOpz(demoFiles());
  let n = 0;
  let until: number | null = null;
  const transport: Transport = {
    send: (raw) => fake.send(raw),
    async receive(ms) {
      const raw = await fake.receive(ms);
      if (!raw || raw[5] !== 0x34) return raw;
      if (until === null && n++ === from) until = fake.now + seconds;
      return until !== null && fake.now < until ? null : raw;
    },
  };
  const store = new MemoryLoaderStore();
  const lines: string[] = [];
  const waits: Array<OpenWait | null> = [];
  const c = new LoaderController(store, { log: (l) => lines.push(l), confirm: async () => true, changed: () => {} });
  c.connect(transport, "demo", "demo", undefined, () => fake.now);
  const tracked = { c, fake, lines, waits };
  const changed = c.hooks.changed;
  c.hooks.changed = () => { waits.push(c.waiting); changed(); };
  return tracked;
}

test("a session open the OP-Z doesn't answer is tried once more, and the install goes on", async () => {
  // 0 the batch's backup, 1 the first engine's backup, 2 its push: unanswered (a friend's install, 2026-11-05)
  const { c, fake, lines, waits } = setupUnanswered(2, OPEN_WAIT + 1);
  const r = await install(c, featuredChoice(engines));
  assert.ok(r.ok, r.lines.join("\n"));
  assert.ok(lines.some((l) => l.startsWith(`the OP-Z did not answer within ${OPEN_WAIT} s`)));
  assert.ok(fake.files.has("engines/drm1.engine") && fake.files.has("engines/syn1.engine"));
  assert.ok(waits.some((w) => w?.attempt === 2), "the panel saw the second try");
  assert.equal(c.waiting, null);
  assert.equal(c.interrupted, null);
});

test("no answer at all before a push: nothing written, no interrupted-write banner", async () => {
  const { c, fake } = setupUnanswered(2);
  const before = new Map(fake.files);
  const r = await install(c, featuredChoice(engines));
  assert.ok(!r.ok);
  assert.match(r.error!, /no \$34 reply/);
  assert.ok(r.highlights.includes("nothing written: the OP-Z didn't answer, so nothing was sent."));
  assert.equal(c.interrupted, null, "nothing to check: nothing was sent");
  assert.deepEqual([...fake.files.keys()].sort(), [...before.keys()].sort());
});

test("no answer for the read-back after a push: that write is unchecked, the banner says so", async () => {
  const { c } = setupUnanswered(3);
  const r = await install(c, featuredChoice(engines));
  assert.ok(!r.ok);
  assert.ok(c.interrupted, "the push went out and was never read back");
  assert.equal(c.interrupted!.op, "install");
});

test("an engine installed and checked, then no answer: nothing half-written, no banner", async () => {
  // 0 batch backup; engine 1: 1 backup, 2 push, 3 read-back; engine 2: 4 backup… unanswered
  const { c, fake } = setupUnanswered(4);
  const r = await install(c, featuredChoice(engines));
  assert.ok(!r.ok);
  assert.ok(fake.files.has("engines/drm1.engine"), "the first engine of the batch");
  assert.ok(!fake.files.has("engines/drm2.engine"));
  assert.ok(r.highlights.some((l) => l.startsWith("nothing was left half-written")));
  assert.equal(c.interrupted, null);
});

test("the start (every key as it is): nothing to install, nothing asked, nothing written", async () => {
  const { fake, c, asked } = setup();
  const before = new Map(fake.files);
  const r = await install(c, defaultChoice());
  assert.ok(r.ok);
  assert.equal(asked.length, 0);
  assert.deepEqual([...fake.files.keys()].sort(), [...before.keys()].sort());
});

const bakeEngines = () => pickedDirs(featuredChoice(engines)).map((dir) => engines.find((e) => e.dir === dir)!)
  .map((e) => ({ name: e.dir, manifest: e.manifest, presets: presetDocs(e) }));

test("a new OP-Z's disk (no preset file): the bake makes one with only our presets, user bits set", async () => {
  const disk = DemoDisk.fresh();
  const bake = bakeEngines();
  const asked: BakeConfirm[] = [];
  let backedUp = 0;
  const r = await runBake(disk, { kind: "bake", engines: bake, overwriteUser: false }, {
    log: () => {}, backup: async () => (backedUp++, "b"), confirm: async (c) => (asked.push(c), true),
  });
  assert.ok(r.ok && r.written && r.created, r.lines.join("\n"));
  assert.equal(backedUp, 0, "nothing to back up");
  assert.equal(asked.length, 1);
  assert.ok(asked[0].created && asked[0].backupId === null);
  const data = disk.files.get("config/userPresets.dat")!;
  assert.equal(data.length, 351_202, "the size the OP-Z reads (2 + 200 × 1756)");
  assert.deepEqual([...data.subarray(0, 2)], [1, 0], "version 1");
  const records = parseUserPresets(data);
  const ours = records.filter((rec) => recordPlugId(rec) !== 0);
  assert.deepEqual(ours.map(recordPlugId).sort(), bake.filter((e) => e.presets.length).map((e) => e.manifest.plug.id).sort());
  for (const rec of ours) {
    const e = bake.find((x) => x.manifest.plug.id === recordPlugId(rec))!;
    assert.equal(storedUserPresets(rec).length, e.presets.length, `${e.name}: every preset with its user bit`);
  }
  // the next bake finds the file and changes nothing
  const again = await runBake(disk, { kind: "bake", engines: bake, overwriteUser: false }, { log: () => {}, backup: async () => "b", confirm: async () => true });
  assert.ok(again.ok && !again.written && !again.created);
});

test("a new OP-Z's disk: cancel writes nothing; a bad read-back deletes the new file; show and restore refuse", async () => {
  const disk = DemoDisk.fresh();
  const no = await runBake(disk, { kind: "bake", engines: bakeEngines(), overwriteUser: false }, { log: () => {}, backup: async () => "b", confirm: async () => false });
  assert.ok(no.cancelled && !disk.files.size);

  class Garbling extends DemoDisk {
    override async getDirectoryHandle(name: string) {
      const dir = (await super.getDirectoryHandle(name)) as DemoDisk;
      const getFile = dir.getFileHandle.bind(dir);
      dir.getFileHandle = async (n, o) => {
        const f = await getFile(n, o);
        return { ...f, createWritable: async () => { const w = await f.createWritable(); return { ...w, write: (d: Uint8Array) => w.write(d.subarray(1)) }; } };
      };
      return dir;
    }
  }
  const bad = new Garbling(new Map());
  const r = await runBake(bad, { kind: "bake", engines: bakeEngines(), overwriteUser: false }, { log: () => {}, backup: async () => "b", confirm: async () => true });
  assert.ok(!r.ok && !r.written);
  assert.match(r.error!, /deleted again, the disk is as it was/);
  assert.equal(bad.files.size, 0);

  for (const request of [{ kind: "show" } as const, { kind: "restore", data: emptyUserPresets(), source: "b1" } as const]) {
    const x = await runBake(disk, request, { log: () => {}, backup: async () => "b", confirm: async () => true });
    assert.ok(!x.ok && !disk.files.size, request.kind);
    assert.match(x.error!, /no preset was ever stored/);
  }
});

test("the bake takes the OP-Z disk's config folder (Chrome won't open the top of a disk)", async () => {
  const disk = new DemoDisk();
  const config = await disk.getDirectoryHandle("config");
  assert.equal(config.name, "config");
  const asked: BakeConfirm[] = [];
  const r = await runBake(config, { kind: "bake", engines: bakeEngines(), overwriteUser: true }, {
    log: () => {}, backup: async () => "b", confirm: async (c) => (asked.push(c), true),
  });
  assert.ok(r.ok && r.written, r.lines.join("\n"));
  assert.equal(asked[0].disk, "OP-Z");
  assert.ok(r.lines.some((l) => l.includes("OP-Z/config/userPresets.dat")), "named as on the disk, not config/config/…");
  assert.ok(parseUserPresets(disk.files.get("config/userPresets.dat")!).some((rec) => recordPlugId(rec) !== 0));

  const fresh = await DemoDisk.fresh().getDirectoryHandle("config");
  const made = await runBake(fresh, { kind: "bake", engines: bakeEngines(), overwriteUser: true }, { log: () => {}, backup: async () => "b", confirm: async () => true });
  assert.ok(made.ok && made.created, "a new OP-Z's config folder: the file is made in it");

  const elsewhere = new DemoDisk(new Map([["Documents/x.txt", new Uint8Array(1)]]), "Downloads", "", new Set());
  const wrong = await runBake(elsewhere, { kind: "bake", engines: bakeEngines(), overwriteUser: true }, { log: () => {}, backup: async () => "b", confirm: async () => true });
  assert.ok(!wrong.ok);
  assert.match(wrong.error!, /not the OP-Z's config folder/);
});
