/** Port of tools/tests/test_opz_cli.py: the planning logic and flows end to end against the TypeScript FakeOpz. */

import assert from "node:assert/strict";
import { beforeEach, describe, it } from "node:test";

import {
  type Doc, type Engine, FakeOpz, FileSession, MASTER, MemoryBackupStore, Opz, PlanError, SLOTS, SessionError,
  VirtualTransport, assign, assignedTracks, buildPatch, devicePresets, fromUtf8, loads, makeEngine, planInstall,
} from "../src/index.ts";

function engine(options: {
  name?: string; pid?: number; pver?: number; slot?: number; ever?: number; mask?: number; presets?: Doc[];
  data?: Uint8Array; master?: unknown;
} = {}): Engine {
  const { name = "testeng", pid = 60950, pver = 1, slot = 86, ever = 1, mask = 0xf0, presets = [] } = options;
  const data = options.data ?? Uint8Array.from({ length: 256 }, (_, i) => [0x00, 0x50, 0x00, 0xad][i % 4]);
  const manifest: Doc = {
    engine: name, memoryslot: slot, engine_version: ever,
    plug: { id: pid, version: pver, name, trackmask: mask, flags: 0, parameter_names: ["a", "b"] },
  };
  if (options.master !== undefined) manifest.master_in_place = options.master;
  return makeEngine(manifest, presets, data);
}

let dev: FakeOpz;
let logs: string[];
let opz: Opz;
let store: MemoryBackupStore;
let confirmAnswer: boolean;

function settings(): [Doc, Doc] {
  return [loads(dev.files.get("settings/plugs.json")!), loads(dev.files.get("settings/slotConfiguration.json")!)];
}

function setUp(): void {
  dev = new FakeOpz();
  logs = [];
  store = new MemoryBackupStore();
  confirmAnswer = true;
  const t = new VirtualTransport(dev);
  opz = new Opz(t, { log: (l) => logs.push(l), backups: store, confirm: () => confirmAnswer, clock: t.clock });
}

describe("install", () => {
  beforeEach(setUp);

  it("first free slot, verified", async () => {
    const e = engine();
    assert.ok(await opz.install(e, [4], null, false, { yes: true }));
    const [plugs, slots] = settings();
    assert.deepEqual(dev.files.get("engines/testeng.engine"), e.data);
    assert.deepEqual(plugs.engines.find((x: Doc) => x.path === "engines/testeng.engine"),
      { memoryslot: 86, path: "engines/testeng.engine", version: 1 });
    assert.deepEqual(assignedTracks(slots, 60950), [[4, 2]]);
    const id = [...store.backups.keys()][0];
    assert.ok(store.backups.get(id)!.has("install.json"));
  });

  it("effect on two fx tracks", async () => {
    assert.ok(await opz.install(engine({ mask: 0x300 }), [8, 9], null, false, { yes: true }));
    assert.deepEqual(assignedTracks(settings()[1], 60950).sort(), [[8, 2], [9, 2]]);
  });

  it("full track refused, nothing written", async () => {
    assert.equal(await opz.install(engine(), [5], null, false, { yes: true }), false);
    assert.equal(dev.syncCount, 0);
    assert.ok(logs.some((l) => l.includes("no free slot")));
  });

  it("specific slot with replace writes the slot map", async () => {
    assert.ok(await opz.install(engine(), [5], 3, true, { yes: true }));
    assert.deepEqual(assignedTracks(settings()[1], 60950).filter(([t]) => t === 5), [[5, 3]]);
  });

  it("specific slot occupied without replace", async () => {
    assert.equal(await opz.install(engine(), [5], 3, false, { yes: true }), false);
    assert.equal(dev.syncCount, 0);
  });

  it("dry run writes nothing", async () => {
    assert.ok(await opz.install(engine(), [4], null, false, { dryRun: true }));
    assert.equal(dev.syncCount, 0);
    assert.ok(logs.some((l) => l.startsWith("dry run: nothing written. patch.json:\n{\"auto_import\"")));
  });

  it("declined confirmation writes nothing", async () => {
    confirmAnswer = false;
    assert.equal(await opz.install(engine(), [4]), false);
    assert.equal(dev.syncCount, 0);
    assert.ok(logs.includes("cancelled."));
  });

  it("reinstall needs a higher version, and a replaced engine needs a restart", async () => {
    assert.ok(await opz.install(engine(), [4], null, false, { yes: true }));
    assert.ok(!logs.some((l) => l.includes("RESTART")));
    assert.equal(await opz.install(engine(), [], null, false, { yes: true }), false);
    const data = new Uint8Array(300).fill(1);
    assert.ok(await opz.install(engine({ pver: 2, ever: 2, data }), [], null, false, { yes: true }));
    assert.ok(logs.some((l) => l.includes("RESTART THE OP-Z")));
    assert.deepEqual(dev.files.get("engines/testeng.engine"), data);
  });

  it("verification failure restores the slot map", async () => {
    const before = dev.files.get("settings/slotConfiguration.json")!;
    dev.importPatch = () => {}; // the device "ignores" the patch
    assert.equal(await opz.install(engine(), [4], null, false, { yes: true }), false);
    assert.deepEqual(settings()[1], loads(before));
    assert.ok(logs.some((l) => l.includes("restore verified")));
  });
});

describe("plan", () => {
  const d = new FakeOpz();
  const plugs = loads(d.files.get("settings/plugs.json")!);
  const slots = loads(d.files.get("settings/slotConfiguration.json")!);
  const errors = (e: Engine, tracks = [4], slot: number | null = null) => planInstall(plugs, slots, e, tracks, slot)[0];

  it("memory slot collision", () => {
    assert.ok(errors(engine({ slot: 79 })).some((x) => x.includes("memory slot 79")));
  });

  it("TE engine name and low plug id", () => {
    const errs = errors(engine({ name: "crush", pid: 200 }));
    assert.ok(errs.some((x) => x.includes("TE engine")) && errs.some((x) => x.includes("below 60000")));
    assert.ok(errs.includes("engine name 'crush' is a TE engine; pick another name"));
  });

  it("slot above 89 refused", () => {
    assert.ok(errors(engine({ slot: 95 })).some((x) => x.includes("outside")));
  });

  it("trackmask is only a warning", () => {
    const [errs, warnings] = planInstall(plugs, slots, engine({ mask: 0xf0 }), [0], null);
    assert.deepEqual(errs, []);
    assert.deepEqual(warnings, ["track kick is outside the plug's trackmask 0xf0 (the device does not enforce it)"]);
  });

  it("master refused without master_in_place", () => {
    for (const e of [engine({ mask: 0xf00 }), engine({ mask: 0xf00, master: false }), engine({ mask: 0xf00, master: "yes" })]) {
      const [errs, , plan] = planInstall(plugs, slots, e, [MASTER], null);
      assert.equal(errs.length, 1);
      assert.match(errs[0], /cannot go on track master/);
      assert.match(errs[0], /out = NULL/);
      assert.match(errs[0], /"master_in_place": true/);
      assert.deepEqual(plan.assign, []);
    }
    assert.ok(errors(engine({ mask: 0xf00 }), [10, MASTER], 3).length);
  });

  it("master allowed with master_in_place", () => {
    const [errs, , plan] = planInstall(plugs, slots, engine({ mask: 0x800, master: true }), [MASTER], null);
    assert.deepEqual(errs, []);
    assert.deepEqual(plan.assign, [[MASTER, 1]]);
  });

  it("synth and drum plugs only on tracks 1-8", () => {
    for (const mask of [0xf0, 0xf, 0]) {
      for (const t of [8, 9, 10, MASTER]) {
        const [errs, , plan] = planInstall(plugs, slots, engine({ mask, master: true }), [t], null);
        assert.equal(errs.length, 1, `mask ${mask} track ${t}`);
        assert.match(errs[0], /is a synth or drum engine/);
        assert.match(errs[0], /Use tracks kick\.\.chord \(1-8\)/);
        assert.deepEqual(plan.assign, []);
      }
    }
    assert.deepEqual(errors(engine({ mask: 0xf0 }), [0, 7]), []);
    assert.deepEqual(errors(engine({ mask: 0x300 }), [10]), []);
  });

  it("tape still allowed", () => {
    const [errs, , plan] = planInstall(plugs, slots, engine({ mask: 0x400 }), [10], null);
    assert.deepEqual(errs, []);
    assert.deepEqual(plan.assign, [[10, 1]]);
  });

  it("patch shape", () => {
    const doc = loads(buildPatch(engine({
      presets: [{ slot: 0, index: 1, name: "x", default_octave: -1, values: new Array(18).fill(1) }],
    }), [4]));
    const entry = doc.auto_import[0];
    assert.deepEqual(entry.tracks, [4]);
    assert.deepEqual(entry.engine, { memoryslot: 86, path: "engines/testeng.engine", version: 1 });
    assert.equal(entry.plug.presets[0].defaultOctave, -1);
    for (const key of ["id", "version", "engine", "samples", "flags"]) assert.ok(key in entry.plug);
  });

  it("preset validation", () => {
    assert.throws(() => devicePresets([{ values: new Array(17).fill(0) }]), PlanError);
    assert.throws(() => devicePresets([{ name: "hi", values: [...new Array(17).fill(0), 256] }]),
      { message: "preset 'hi' must have 18 values in 0..255" });
  });
});

describe("assign", () => {
  beforeEach(setUp);

  it("installed plug to a drum track", async () => {
    assert.ok(await opz.install(engine(), [4], null, false, { yes: true }));
    assert.ok(await opz.assign(60950, [0], null, false, { yes: true }));
    const at = assignedTracks(settings()[1], 60950);
    assert.ok(at.some(([t, s]) => t === 0 && s === 2));
    assert.ok(at.some(([t, s]) => t === 4 && s === 2));
  });

  it("unknown plug refused", async () => {
    assert.equal(await opz.assign(60999, [0], null, false, { yes: true }), false);
  });

  it("master needs master_in_place", async () => {
    const e = engine({ mask: 0xf00 });
    assert.ok(await opz.install(e, [8], null, false, { yes: true }));
    const before = dev.syncCount;
    assert.equal(await opz.assign(60950, [MASTER], null, false, { yes: true }), false);
    assert.equal(await opz.assign(60950, [MASTER], null, false, { yes: true, manifests: [e.manifest] }), false);
    assert.equal(dev.syncCount, before);
    assert.ok(logs.some((l) => l.includes("cannot go on track master")));
    assert.ok(await opz.assign(60950, [10], null, false, { yes: true, manifests: [e.manifest] }));
    const ok = engine({ mask: 0xf00, master: true }).manifest;
    assert.ok(await opz.assign(60950, [MASTER], null, false, { yes: true, manifests: [ok] }));
    const at = assignedTracks(settings()[1], 60950);
    assert.ok(at.some(([t, s]) => t === MASTER && s === 1));
    assert.ok(at.some(([t, s]) => t === 10 && s === 1));
  });

  it("synth plug not assigned to an effect track", async () => {
    const e = engine();
    assert.ok(await opz.install(e, [4], null, false, { yes: true }));
    const before = dev.syncCount;
    for (const t of [8, 10, MASTER]) {
      assert.equal(await opz.assign(60950, [t], null, false, { yes: true, manifests: [e.manifest] }), false);
      assert.equal(await opz.assign(60950, [t], null, false, { yes: true }), false);
    }
    assert.equal(dev.syncCount, before);
    assert.ok(logs.includes("error:   plug 60950 cannot go on track tape: its trackmask 0xf0 names no effect track, so it "
      + "is a synth or drum engine, and what one does on fx1, fx2, tape or master is untested (it may crash the OP-Z). "
      + "Use tracks kick..chord (1-8)"));
    assert.ok(logs.some((l) => l.includes("no local plug.json with that id names an effect track")));
    const base = settings()[1];
    assert.equal(await opz.writeSlotMap(assign(base, 9, 2, 60950), base, "slots", { yes: true, manifests: [e.manifest] }), false);
    assert.equal(dev.syncCount, before);
  });

  it("slot map write refuses a new plug on master", async () => {
    const e = engine({ mask: 0xf00 });
    assert.ok(await opz.install(e, [8], null, false, { yes: true }));
    const base = settings()[1];
    const before = dev.syncCount;
    assert.equal(await opz.writeSlotMap(assign(base, MASTER, 1, 60950), base, "slots", { yes: true }), false);
    assert.equal(dev.syncCount, before);
    assert.ok(logs.some((l) => l.startsWith("error:   plug 60950 cannot go on track master")));
    assert.ok(await opz.writeSlotMap(assign(base, 10, 1, 60950), base, "slots", { yes: true, manifests: [e.manifest] }));
    const now = settings()[1];
    const ok = engine({ mask: 0xf00, master: true }).manifest;
    assert.ok(await opz.writeSlotMap(assign(now, MASTER, 1, 60950), now, "slots", { yes: true, manifests: [ok] }));
    assert.ok(loads(dev.files.get(SLOTS)!).tracks.some((t: Doc) => t.index === MASTER));
  });

  it("full track refused", async () => {
    await opz.install(engine(), [4], null, false, { yes: true });
    const before = dev.syncCount;
    assert.equal(await opz.assign(60950, [5], null, false, { yes: true }), false);
    assert.equal(dev.syncCount, before);
  });
});

describe("remove and restore", () => {
  beforeEach(setUp);

  it("remove of an assigned plug needs unassign", async () => {
    await opz.install(engine(), [4], null, false, { yes: true });
    assert.equal(await opz.remove(60950, false, { yes: true }), false);
    assert.ok(await opz.remove(60950, true, { yes: true }));
    const [plugs, slots] = settings();
    assert.ok(!plugs.plugs.some((p: Doc) => p.id === 60950));
    assert.deepEqual(assignedTracks(slots, 60950), []);
    assert.ok(logs.some((l) => l.startsWith("removed") && l.includes("'engines/testeng.engine'")));
  });

  it("dry run describes the unassign", async () => {
    await opz.install(engine(), [4], null, false, { yes: true });
    assert.ok(await opz.remove(60950, true, { dryRun: true }));
    assert.equal(logs.at(-1), "dry run: would unassign [(4, 2)] and remove plug 60950");
  });

  it("refuses a TE plug", async () => {
    assert.equal(await opz.remove(10, true, { yes: true }), false);
  });

  it("restore --touch forces a real upload", async () => {
    const b = await opz.backup();
    const before = dev.syncCount;
    assert.ok(await opz.restore(b.id, { yes: true, touch: true }));
    assert.ok(dev.log.some((l) => l.includes("wrote settings/slotConfiguration.json")));
    assert.deepEqual(settings()[1], b.slots);
    assert.equal(dev.syncCount, before + 1);
    assert.ok(fromUtf8(dev.files.get("settings/slotConfiguration.json")!).startsWith('{\n "tracks": [\n  {\n   "index": 0,'));
  });

  it("restore", async () => {
    const b = await opz.backup();
    await opz.install(engine(), [4], null, false, { yes: true });
    assert.ok(await opz.restore(b.id, { yes: true }));
    assert.deepEqual(settings()[1], b.slots);
  });
});

describe("session", () => {
  it("unserved path", async () => {
    const t = new VirtualTransport(new FakeOpz());
    const s = new FileSession(t, { clock: t.clock });
    await s.open(2);
    await assert.rejects(s.pull("engines/crush.engine", 0.2), SessionError);
  });

  it("no reply without hello", async () => {
    class Mute extends FakeOpz {
      protected override on00(): void {}
    }
    const t = new VirtualTransport(new Mute());
    await assert.rejects(new FileSession(t, { clock: t.clock }).open(0.5), SessionError);
  });

  it("unchanged file is skipped", async () => {
    const d = new FakeOpz();
    const t = new VirtualTransport(d);
    const s = new FileSession(t, { clock: t.clock });
    await s.open(2);
    const same = d.files.get("settings/slotConfiguration.json")!;
    const result = await s.push([["settings/slotConfiguration.json", same]], null, 15, 0.2);
    assert.deepEqual(result.skipped, ["settings/slotConfiguration.json"]);
  });

  it("one-level file lands, then an identical push is skipped (the probe, Q-49)", async () => {
    const d = new FakeOpz();
    const t = new VirtualTransport(d);
    const s = new FileSession(t, { clock: t.clock });
    await s.open(2);
    const pack = Uint8Array.from({ length: 900 }, (_, i) => i % 251);
    assert.deepEqual((await s.push([["samplepacks/opzk_probe.aif", pack]], null, 15, 0.2)).requested,
      ["samplepacks/opzk_probe.aif"]);
    assert.deepEqual(d.files.get("samplepacks/opzk_probe.aif"), pack);
    assert.ok(d.inboxDirs.has("samplepacks"));
    assert.deepEqual((await s.push([["samplepacks/opzk_probe.aif", pack]], null, 15, 0.2)).skipped,
      ["samplepacks/opzk_probe.aif"]);
  });

  it("refuses a path two folders deep before sending (Q-49)", async () => {
    const d = new FakeOpz();
    const t = new VirtualTransport(d);
    const s = new FileSession(t, { clock: t.clock });
    await s.open(2);
    await assert.rejects(s.push([["samplepacks/user/opzk_x.aif", Uint8Array.of(1)]]), /two folders deep/);
    assert.equal(d.syncCount, 0);
  });

  it("fake: inbox/a/ must exist before a/b/x can be staged", async () => {
    class Raw extends FakeOpz {
      check(path: string): boolean { return this.stage(path); }
    }
    const d = new Raw();
    assert.equal(d.check("samplepacks/user/x.aif"), false);
    assert.equal(d.check("samplepacks/x.aif"), true);   // makes inbox/samplepacks/
    assert.equal(d.check("samplepacks/user/x.aif"), true);
  });

  it("works with a real clock too", async () => {
    const s = new FileSession(new FakeOpz());
    await s.open(2);
    assert.equal(fromUtf8(await s.pull("settings/plugs.json")).slice(0, 12), '{"engines": ');
    await s.close();
  });
});

describe("install many (one push per engine, one read-back)", () => {
  beforeEach(setUp);
  const opens = () => logs.filter((l) => l.includes("session open")).length;

  it("three engines: one backup, three pushes, one read-back; slot map untouched", async () => {
    const slotsBefore = dev.files.get("settings/slotConfiguration.json")!;
    const es = [engine({ name: "aa", pid: 60950, slot: 86 }), engine({ name: "bb", pid: 60951, slot: 85 }),
      engine({ name: "cc", pid: 60952, slot: 84, presets: [{ index: 0, slot: 0, name: "p", values: Array(18).fill(1) }] })];
    assert.ok(await opz.installMany(es, [], { yes: true }));
    assert.equal(dev.syncCount, 3);
    assert.equal(opens(), 5);
    const [plugs] = settings();
    for (const e of es) {
      assert.ok(plugs.plugs.some((p: Doc) => p.id === e.manifest.plug.id && p.engine === e.devicePath));
      assert.deepEqual(dev.files.get(e.devicePath), e.data);
    }
    assert.deepEqual(dev.files.get("settings/slotConfiguration.json"), slotsBefore);
    assert.equal(dev.presets.get(60952)?.size, 1);
    const id = [...store.backups.keys()][0];
    assert.equal(loads(store.backups.get(id)!.get("install.json")!).batch.length, 3);
  });

  it("presets of installed plugs go in one plugs[]-only push after the engines", async () => {
    assert.ok(await opz.install(engine({ name: "aa" }), [], null, false, { yes: true }));
    dev.syncCount = 0;
    const again = engine({ name: "aa", presets: [{ index: 1, slot: 0, name: "x", values: Array(18).fill(2) }] });
    const added = engine({ name: "bb", pid: 60951, slot: 85 });
    let patch: Doc = {};
    const real = dev.importPatch.bind(dev);
    dev.importPatch = (doc: Doc) => { patch = doc; real(doc); };
    assert.ok(await opz.installMany([added], [again], { yes: true }));
    assert.equal(dev.syncCount, 2);
    assert.equal(dev.presets.get(60950)?.size, 1);
    assert.equal(patch.auto_import, undefined, "the last push carries the presets only");
    assert.equal(patch.plugs[0].id, 60950);
    assert.ok(settings()[0].plugs.some((p: Doc) => p.id === 60951));
  });

  it("refuses a memory slot or plug id used twice in the batch, writes nothing", async () => {
    assert.equal(await opz.installMany([engine({ name: "aa" }), engine({ name: "bb", pid: 60951 })], [], { yes: true }), false);
    assert.ok(logs.some((l) => l.includes("memory slot 86 is also aa")));
    assert.equal(await opz.installMany([engine({ name: "aa" }), engine({ name: "bb", slot: 85 })], [], { yes: true }), false);
    assert.ok(logs.some((l) => l.includes("plug id 60950 is also aa")));
    assert.equal(dev.syncCount, 0);
  });

  it("a plug the device did not import fails verification", async () => {
    dev.importPatch = () => {};
    assert.equal(await opz.installMany([engine()], [], { yes: true }), false);
    assert.ok(logs.some((l) => l.includes("verify:  testeng: plug 60950 missing")));
  });

  it("dry run prints a patch.json per push and writes nothing", async () => {
    assert.ok(await opz.installMany([engine({ name: "aa" }), engine({ name: "bb", pid: 60951, slot: 85 })], [], { dryRun: true }));
    assert.equal(dev.syncCount, 0);
    const dry = logs.find((l) => l.startsWith("dry run"))!.split("\n").slice(1).map(loads);
    assert.deepEqual(dry.map((d) => d.auto_import.length), [1, 1]);
  });

  it("a push that never finishes stops at its deadline with a trace", async () => {
    const t = new VirtualTransport(dev);
    const lines: string[] = [];
    const s = new FileSession(t, { log: (l) => lines.push(l), clock: t.clock });
    await s.open();
    await assert.rejects(s.push([["engines/x.engine", new Uint8Array(4000).fill(7)]], null, 15, 2, 0.001), /not finished after/);
    assert.ok(lines.some((l) => l.startsWith("push trace: sent")));
  });
});

describe("read-back reuse", () => {
  beforeEach(setUp);
  const opens = () => logs.filter((l) => l.includes("session open")).length;

  function reusing(seconds: number): Opz {
    const t = new VirtualTransport(dev);
    return new Opz(t, { log: (l) => logs.push(l), backups: store, confirm: () => true, clock: t.clock, reuseRead: seconds });
  }

  it("off by default: every flow reads again", async () => {
    assert.ok(await opz.install(engine({ name: "aa" }), [], null, false, { yes: true }));
    assert.ok(await opz.install(engine({ name: "bb", pid: 60951, slot: 85 }), [], null, false, { yes: true }));
    assert.equal(opens(), 6);
  });

  it("the next flow's backup takes the read-back, the result is the same", async () => {
    const o = reusing(5);
    assert.ok(await o.install(engine({ name: "aa" }), [], null, false, { yes: true }));
    assert.ok(await o.install(engine({ name: "bb", pid: 60951, slot: 85 }), [4], null, false, { yes: true }));
    assert.equal(opens(), 5);
    assert.ok(logs.some((l) => l.includes("(nothing written since)")));
    assert.deepEqual(assignedTracks(settings()[1], 60951), [[4, 2]]);
    const ids = [...store.backups.keys()];
    assert.equal(ids.length, 2);
    // the second backup holds what the OP-Z had after the first install
    assert.ok(loads(store.backups.get(ids[1])!.get("settings/plugs.json")!).plugs.some((p: Doc) => p.id === 60950));
  });

  it("a push in between, or an old read, means a fresh read", async () => {
    const o = reusing(5);
    await o.readSettings();
    await o.push([]);
    await o.backup("x");
    assert.equal(opens(), 3);
    const t = o.clock() + 10;
    while (o.clock() < t) await o.transport.receive(1000);
    await o.backup("y");
    assert.equal(opens(), 4);
  });
});
