/** The slot-map write behind the web app's plug librarian: checks, the plan lines, verify and rollback. */

import assert from "node:assert/strict";
import { beforeEach, describe, it } from "node:test";

import {
  type Doc, FakeOpz, MemoryBackupStore, Opz, SLOTS, VirtualTransport, assign, clone, dumps, loads, sameSlots,
  slotCells, slotMapChanges, slotMapProblems, unassign, utf8,
} from "../src/index.ts";

let dev: FakeOpz;
let logs: string[];
let opz: Opz;
let store: MemoryBackupStore;
let asked: string[];

function slots(): Doc {
  return loads(dev.files.get(SLOTS)!);
}

function plugs(): Doc {
  return loads(dev.files.get("settings/plugs.json")!);
}

beforeEach(() => {
  dev = new FakeOpz();
  logs = [];
  asked = [];
  store = new MemoryBackupStore();
  const t = new VirtualTransport(dev);
  opz = new Opz(t, { log: (l) => logs.push(l), backups: store, confirm: (q) => (asked.push(q), true), clock: t.clock });
});

describe("pure helpers", () => {
  it("cells, sameness and the change lines ignore order and empty tracks", () => {
    const a = slots();
    const b = clone(a);
    b.tracks.reverse();
    b.tracks.push({ index: 3, packs: [] });
    assert.ok(sameSlots(a, b));
    const c = assign(a, 0, 3, 109);
    assert.ok(!sameSlots(a, c));
    assert.equal(slotCells(c).get("0:3"), 109);
    assert.deepEqual(slotMapChanges(a, c), ["kick slot 3: empty -> 109"]);
    assert.deepEqual(slotMapChanges(c, unassign(a, 121)), ["kick slot 1: 121 -> empty", "kick slot 3: 109 -> empty"]);
  });

  it("problems: slot range, doubles, uninstalled plugs, bad tracks", () => {
    const bad = { tracks: [
      { index: 0, packs: [{ id: 121, slot: 0 }, { id: 121, slot: 2 }, { id: 109, slot: 2 }, { id: 99999, slot: 4 }] },
      { index: 0, packs: [] },
      { index: 20, packs: [] },
      { index: 13, packs: [] },
    ] };
    const [errors, warnings] = slotMapProblems(plugs(), bad);
    assert.deepEqual(errors, [
      "track kick slot 0: slots are 1..10",
      "track kick slot 2 holds two plugs",
      "track kick slot 4: plug 99999 is not installed",
      "track kick appears twice",
      "track index 20 is outside 0..15",
    ]);
    assert.deepEqual(warnings, ["track 13 is not a sound track (0..11)"]);
  });
});

describe("writeSlotMap", () => {
  it("swap two slots: backup, plan lines, confirm, verified", async () => {
    const base = slots();
    const lead = base.tracks.find((t: Doc) => t.index === 5).packs;
    assert.equal(lead.length, 10);
    let want = assign(base, 4, 1, 109);              // bass slot 1: analoga -> crush
    want = assign(want, 4, 2, 10);                   // analoga moves to bass slot 2
    assert.ok(await opz.writeSlotMap(want, base, "slots"));
    assert.ok(sameSlots(slots(), want));
    assert.deepEqual(asked, ["Write the slot map (2 slots)?"]);
    assert.deepEqual(logs.filter((l) => l.startsWith("plan:")), ["plan: bass slot 1: 10 -> 109", "plan: bass slot 2: empty -> 10"]);
    assert.equal(logs.at(-1), "slot map written and verified.");
    assert.equal(store.backups.size, 1);
    assert.match([...store.backups.keys()][0], /-slots$/);
  });

  it("nothing to change: no confirm, no push", async () => {
    const syncs = dev.syncCount;
    assert.ok(await opz.writeSlotMap(slots(), slots()));
    assert.deepEqual(asked, []);
    assert.equal(dev.syncCount, syncs);
    assert.equal(logs.at(-1), "nothing to change: the OP-Z's slot map is already like this.");
  });

  it("refuses when the OP-Z's map changed since it was read", async () => {
    const base = slots();
    dev.files.set(SLOTS, utf8(dumps(unassign(base, 121))));   // e.g. a slot cleared on the device
    const syncs = dev.syncCount;
    assert.equal(await opz.writeSlotMap(assign(base, 0, 2, 109), base), false);
    assert.equal(dev.syncCount, syncs);
    assert.ok(logs.some((l) => l.includes("the slot map on the OP-Z changed since it was read: kick slot 1: 121 -> empty")), logs.join("\n"));
    assert.equal(logs.at(-1), "nothing written.");
  });

  it("refuses uninstalled plugs; dry run and cancel write nothing", async () => {
    const base = slots();
    assert.equal(await opz.writeSlotMap(assign(base, 0, 2, 4242), base), false);
    assert.ok(logs.includes("error:   track kick slot 2: plug 4242 is not installed"));
    const syncs = dev.syncCount;
    assert.ok(await opz.writeSlotMap(assign(base, 0, 2, 109), base, "slots", { dryRun: true }));
    assert.equal(logs.at(-1), "dry run: nothing written.");
    const no = new Opz(new VirtualTransport(dev), { log: (l) => logs.push(l), backups: store, confirm: () => false });
    assert.equal(await no.writeSlotMap(assign(base, 0, 2, 109), base), false);
    assert.equal(logs.at(-1), "cancelled.");
    assert.equal(dev.syncCount, syncs);
  });

  it("a read-back that differs rolls the slot map back", async () => {
    const base = slots();
    let once = true;
    // the device "drops" the new entry the first time it stores the slot map
    const files = dev.files;
    const original = files.set.bind(files);
    files.set = (path: string, data: Uint8Array) => {
      if (path === SLOTS && once) {
        once = false;
        return original(path, utf8(dumps(base)));
      }
      return original(path, data);
    };
    assert.equal(await opz.writeSlotMap(assign(base, 0, 2, 109), base), false);
    assert.ok(logs.includes("verify:  kick slot 2: 109 expected, read back empty"), logs.join("\n"));
    assert.ok(logs.some((l) => l.startsWith("restoring the slot map from")));
    assert.ok(sameSlots(slots(), base));
  });
});
