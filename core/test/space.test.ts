/** Port of tools/tests/test_opz_space.py: room on the OP-Z and removing every plug of ours, against the FakeOpz. */

import assert from "node:assert/strict";
import { beforeEach, describe, it } from "node:test";

import {
  BOOT_PATCH, type Doc, type Engine, FakeOpz, MemoryBackupStore, Opz, PLUGS, SLOTS, VirtualTransport, assignedTracks,
  describeSpace, dumps, installRoom, isOurPlug, loads, makeEngine, planRemoveOurs, spaceReport, utf8,
} from "../src/index.ts";

const USER: Doc = { engine: "engines/sampleplayergain.engine", flags: 2, id: 61005, samples: "samplepacks/user/mykit.aif", version: 1 };
const OTHER: Doc = { engine: "engines/cleantone.engine", flags: 0, id: 60100, samples: "", version: 1 };

function engine(name = "testeng", pid = 60950, slot = 86, mask = 0xf0): Engine {
  const data = Uint8Array.from({ length: 256 }, (_, i) => [0x00, 0x50, 0x00, 0xad][i % 4]);
  return makeEngine({
    engine: name, memoryslot: slot, engine_version: 1,
    plug: { id: pid, version: 1, name, trackmask: mask, flags: 0, parameter_names: ["a", "b"] },
  }, [], data);
}

let dev: FakeOpz;
let logs: string[];
let opz: Opz;
let store: MemoryBackupStore;
let confirmAnswer: boolean;
let first: string;

function settings(): [Doc, Doc] {
  return [loads(dev.files.get(PLUGS)!), loads(dev.files.get(SLOTS)!)];
}

async function setUp(): Promise<void> {
  dev = new FakeOpz();
  const [plugs, slots] = settings();
  plugs.plugs.push(USER, OTHER);
  plugs.engines.push({ memoryslot: 20, path: "engines/cleantone.engine", version: 1 });
  slots.tracks[0].packs.push({ id: 61005, slot: 2 }); // kick 2: the user's kit
  slots.tracks.splice(1, 0, { index: 1, packs: [{ id: 60100, slot: 1 }] }); // snare 1: someone else's plug
  dev.files.set(PLUGS, utf8(dumps(plugs)));
  dev.files.set(SLOTS, utf8(dumps(slots)));
  dev.files.set("samplepacks/user/mykit.aif", utf8("FORM-user"));
  logs = [];
  store = new MemoryBackupStore();
  confirmAnswer = true;
  const t = new VirtualTransport(dev);
  opz = new Opz(t, { log: (l) => logs.push(l), backups: store, confirm: () => confirmAnswer, clock: t.clock });
  first = (await opz.backup("first")).id;
  assert.ok(await opz.install(engine(), [4], null, false, { yes: true }));
  assert.ok(await opz.install(engine("fxeng", 61001, 85, 0x300), [8], null, false, { yes: true }));
  assert.ok(await opz.install(engine("leadeng", 61010, 84), [5], 3, true, { yes: true }));
  logs.length = 0;
}

describe("space report", () => {
  beforeEach(setUp);

  it("ours is by range and never a user sample", () => {
    assert.ok(isOurPlug({ id: 60900, flags: 0 }));
    assert.ok(isOurPlug({ id: 61999, flags: 4 }));
    assert.ok(!isOurPlug({ id: 60100, flags: 0 }));
    assert.ok(!isOurPlug({ id: 62000, flags: 0 }));
    assert.ok(!isOurPlug(USER));
  });

  it("counts like opz.py space", () => {
    const [plugs, slots] = settings();
    const r = spaceReport(plugs, slots, { "engines/testeng.engine": 256, "engines/fxeng.engine": 1024 });
    assert.deepEqual([r.plugs, r.free, r.ours.length, r.user_samples, r.te], [8, 192, 3, 1, 4]);
    assert.deepEqual(r.our_engine_files, ["engines/fxeng.engine", "engines/leadeng.engine", "engines/testeng.engine"]);
    assert.deepEqual([r.our_engine_bytes, r.unsized], [1280, 1]);
    const text = describeSpace(r);
    assert.ok(text.includes("plug definitions: 8 of 200 (192 free): 3 of ours, 1 user samples, 4 TE"));
    assert.ok(text.includes("our engine files: 3 (2 KB + 1 not built here)"));
  });

  it("orphans count as ours, TE's engines never", async () => {
    assert.ok(await opz.remove(60950, true, { yes: true }));
    const [plugs, slots] = settings();
    assert.ok(spaceReport(plugs, slots).our_engine_files.includes("engines/testeng.engine"));
    assert.ok(!spaceReport(plugs, slots).our_engine_files.includes("engines/sampleplayergain.engine"));
  });
});

describe("remove every plug of ours", () => {
  beforeEach(setUp);

  it("plans which cells empty and what goes back", async () => {
    const [plugs, slots] = settings();
    const back = loads((await store.load(first)).get(SLOTS)!);
    const plan = planRemoveOurs(plugs, slots, back);
    assert.deepEqual(plan.ids, [60950, 61001, 61010]);
    assert.deepEqual([...plan.cleared].sort(), [[4, 2, 60950], [5, 3, 61010], [8, 2, 61001]]);
    assert.deepEqual(plan.refilled, [[5, 3, 10]]);
    assert.deepEqual(plan.errors, []);
  });

  it("dry run writes nothing", async () => {
    const n = dev.syncCount;
    assert.ok(await opz.removeOurs({ dryRun: true }));
    assert.equal(dev.syncCount, n);
    assert.ok(logs.includes("plan: remove 3 plugs of ours: 60950, 61001, 61010"));
    assert.ok(logs.includes("plan: empty bass 2, lead 3, fx1 2"));
    assert.ok(logs.includes("dry run: nothing written."));
  });

  it("removes them all, puts TE's back, and nothing comes back after a restart", async () => {
    const before = settings()[0];
    assert.ok(await opz.removeOurs({ yes: true, putBack: first }));
    const [plugs, slots] = settings();
    const ids = new Set(plugs.plugs.map((p: Doc) => p.id));
    for (const id of [60950, 61001, 61010]) assert.ok(!ids.has(id));
    for (const id of [10, 121, 109, 61005, 60100]) assert.ok(ids.has(id));
    assert.ok(assignedTracks(slots, 10).some(([t, s]) => t === 5 && s === 3));
    assert.deepEqual(assignedTracks(slots, 61005), [[0, 2]]);
    assert.ok(dev.files.has("samplepacks/user/mykit.aif"));
    assert.deepEqual(loads(dev.files.get(BOOT_PATCH)!).plugs, []);
    assert.ok(logs.some((l) => l.startsWith("removed 3 plugs of ours and verified")));
    dev.restart();
    const after = new Set(settings()[0].plugs.map((p: Doc) => p.id));
    for (const id of [60950, 61001, 61010]) assert.ok(!after.has(id));
    assert.equal(plugs.plugs.length, before.plugs.length - 3);
  });

  it("nothing of ours: nothing written", async () => {
    assert.ok(await opz.removeOurs({ yes: true }));
    logs.length = 0;
    const n = dev.syncCount;
    assert.ok(await opz.removeOurs({ yes: true }));
    assert.ok(logs.includes("no plugs of ours on the OP-Z: nothing to remove."));
    assert.equal(dev.syncCount, n);
  });

  it("cancel writes nothing", async () => {
    confirmAnswer = false;
    const n = dev.syncCount;
    assert.equal(await opz.removeOurs(), false);
    assert.equal(dev.syncCount, n);
  });
});

describe("room before an install", () => {
  const many = (n: number): Doc => ({ engines: [], plugs: Array.from({ length: n }, (_, i) => ({ id: i + 1, engine: "engines/x.engine", flags: 0 })) });

  it("matches opz.py install_room", () => {
    assert.equal(installRoom(many(100), 61001), null);
    assert.equal(installRoom(many(150), 61001), "room: 49 of 200 plug definitions free after this install; a content-mode "
      + "restore of your own samples needs one per sample (fewer than 60 left). opz.py remove --all-ours frees them again");
    assert.ok(installRoom(many(180), 61001)!.includes("too few for a typical backup"));
    assert.equal(installRoom(many(140), 5), null);
  });

  it("install logs it", async () => {
    await setUp();
    const [plugs] = settings();
    for (let i = 0; i < 150; i++) plugs.plugs.push({ engine: "engines/x.engine", flags: 0, id: 1000 + i, samples: "", version: 1 });
    dev.files.set(PLUGS, utf8(dumps(plugs)));
    assert.ok(await opz.install(engine("another", 60960, 83), [4], null, false, { dryRun: true }));
    assert.ok(logs.some((l) => l.startsWith("warning: room: 41 of 200 plug definitions free after this install")), logs.join("\n"));
  });
});
