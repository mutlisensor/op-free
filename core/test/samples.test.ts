/**
 * The user-sample guards (tools/opz_samples.py, research/samples.md): the OP-Z deletes a removed plug's sample
 * file, and content mode drops user samples (flag 2) in no slot and deletes their files. Fixture as in
 * tools/tests/test_samples.py: the default fake plus two user samples, one in kick slot 3, one in no slot.
 */

import assert from "node:assert/strict";
import { beforeEach, describe, it } from "node:test";

import {
  type Doc, FakeOpz, MemoryBackupStore, OUR_SAMPLE_PREFIX, Opz, PLUGS, SLOTS, VirtualTransport, assign, crc32,
  defaultFiles, describe as describeStatus, dumps, loads, makeEngine, orphanErrors, planInstall, removalBlock,
  slotMapProblems, unassign, utf8,
} from "../src/index.ts";

const USER_KICKS = "samplepacks/user/ryantechnokicks-8336.aif"; // a path shape TE's app uses (id = crc32 of the path)
const USER_SYNTH = "samplepacks/user/user216445511.aif";
const KICKS_ID = crc32(utf8(USER_KICKS));
const SYNTH_ID = crc32(utf8(USER_SYNTH));

function userFiles(): Map<string, Uint8Array> {
  const files = defaultFiles();
  const plugs = loads(files.get(PLUGS)!);
  const slots = loads(files.get(SLOTS)!);
  plugs.engines.push({ memoryslot: 68, path: "engines/synthsampler.engine", version: 1 });
  for (const [path, eng] of [[USER_KICKS, "sampleplayergain"], [USER_SYNTH, "synthsampler"]]) {
    plugs.plugs.push({ engine: `engines/${eng}.engine`, flags: 2, id: crc32(utf8(path)), samples: path, version: 1 });
    files.set(path, utf8("FORM-user"));
  }
  slots.tracks[0].packs.push({ id: KICKS_ID, slot: 3 });
  files.set(PLUGS, utf8(dumps(plugs)));
  files.set(SLOTS, utf8(dumps(slots)));
  return files;
}

let dev: FakeOpz;
let logs: string[];
let opz: Opz;

const plugs = (): Doc => loads(dev.files.get(PLUGS)!);
const slots = (): Doc => loads(dev.files.get(SLOTS)!);
const logged = (text: string) => logs.some((l) => l.includes(text));

beforeEach(() => {
  dev = new FakeOpz(userFiles());
  logs = [];
  const t = new VirtualTransport(dev);
  opz = new Opz(t, { log: (l) => logs.push(l), backups: new MemoryBackupStore(), confirm: () => true, clock: t.clock });
});

describe("pure rules", () => {
  it("a user plug id is the crc32 of its path", () => {
    assert.equal(KICKS_ID, 717146855); // a plug id on the user's OP-Z
  });

  it("removalBlock: user samples, packs we did not write, shared packs", () => {
    const p = plugs();
    const byId = new Map<number, Doc>(p.plugs.map((x: Doc) => [x.id, x]));
    assert.equal(removalBlock(byId.get(KICKS_ID)!, p),
      `plug ${KICKS_ID} is a user sample (${USER_KICKS}); removing it deletes the sample file from the OP-Z`);
    assert.equal(removalBlock(byId.get(121)!, p),
      "plug 121 uses samplepacks/TeKicks.aif, which our tools did not install; removing it deletes that file");
    assert.equal(removalBlock(byId.get(10)!, p), null); // an engine plug without samples
    const ours = { id: 61400, samples: OUR_SAMPLE_PREFIX + "a.aif", flags: 0 };
    assert.equal(removalBlock(ours, { plugs: [ours] }), null);
    assert.equal(removalBlock({ ...ours, id: 60100 }, { plugs: [] })?.includes("did not install"), true); // id outside 61400-61499
    assert.equal(removalBlock({ ...ours, flags: 2 }, { plugs: [] })?.includes("is a user sample"), true);
    const twin = { ...ours, id: 61401 };
    assert.equal(removalBlock(ours, { plugs: [ours, twin] }),
      `plugs [61401] also use ${OUR_SAMPLE_PREFIX}a.aif; removing plug 61400 would delete it under them`);
  });

  it("orphanErrors: only a user sample assigned before and in no slot after", () => {
    const before = slots();
    assert.deepEqual(orphanErrors(plugs(), before, assign(before, 0, 3, 10)), [
      `slot change would leave user sample ${KICKS_ID} (${USER_KICKS}) in no slot; the OP-Z deletes unassigned user `
        + "samples in content mode. Pick another slot",
    ]);
    assert.deepEqual(orphanErrors(plugs(), before, assign(assign(before, 0, 3, 10), 0, 4, KICKS_ID)), []); // moved
    assert.deepEqual(orphanErrors(plugs(), before, unassign(before, 121)), []); // TE's pack: not a user sample
    assert.deepEqual(orphanErrors(plugs(), before, before), []); // the loose one was never assigned: not this change's doing
  });

  it("describe warns about user samples in no slot, and only then", () => {
    const text = describeStatus(plugs(), slots());
    assert.equal(text.split("\n").at(-1), "WARNING: user samples in no slot (content mode deletes them; assign them "
      + `again first): ${SYNTH_ID} ${USER_SYNTH}`);
    const base = loads(defaultFiles().get(PLUGS)!);
    assert.ok(!describeStatus(base, loads(defaultFiles().get(SLOTS)!)).includes("WARNING"));
    assert.ok(!describeStatus(plugs(), assign(slots(), 4, 2, SYNTH_ID)).includes("WARNING"));
  });

  it("planInstall with --slot --replace cannot displace a user sample", () => {
    const e = makeEngine({ engine: "testeng", memoryslot: 86, engine_version: 1,
      plug: { id: 60950, version: 1, trackmask: 1, flags: 0 } }, [], new Uint8Array(64));
    const [errors] = planInstall(plugs(), slots(), e, [0], 3, true);
    assert.equal(errors.length, 1);
    assert.match(errors[0], new RegExp(`^slot change would leave user sample ${KICKS_ID} `));
    assert.deepEqual(planInstall(plugs(), slots(), e, [0], 4, true)[0], []); // a free slot: fine
    assert.deepEqual(planInstall(plugs(), slots(), e, [0], null, false)[0], []); // first free slot: no slot map
  });

  it("slotMapProblems checks orphans only when given the map it replaces", () => {
    const want = unassign(slots(), KICKS_ID);
    assert.deepEqual(slotMapProblems(plugs(), want)[0], []);
    assert.equal(slotMapProblems(plugs(), want, slots())[0].length, 1);
  });
});

describe("device flows refuse to harm user samples", () => {
  it("remove refuses user samples and packs we did not write, writes nothing", async () => {
    const before = new Map(dev.files);
    for (const pid of [KICKS_ID, SYNTH_ID]) {
      assert.equal(await opz.remove(pid, true, { yes: true }), false);
      assert.ok(logged(`error:   plug ${pid} is a user sample`));
    }
    assert.equal(await opz.remove(121, true, { yes: true, allowTe: true }), false);
    assert.ok(logged("which our tools did not install; removing it deletes that file; refusing"));
    assert.deepEqual(dev.files.get(USER_KICKS), before.get(USER_KICKS));
    assert.equal(dev.syncCount, 0);
  });

  it("remove of a plain engine plug still works", async () => {
    const e = makeEngine({ engine: "testeng", memoryslot: 86, engine_version: 1,
      plug: { id: 60950, version: 1, trackmask: 1, flags: 0 } }, [], new Uint8Array(64));
    assert.ok(await opz.install(e, [0], null, false, { yes: true }));
    assert.ok(await opz.remove(60950, true, { yes: true }));
  });

  it("assign and install over a user sample's only slot are refused", async () => {
    assert.equal(await opz.assign(10, [0], 3, true, { yes: true }), false);
    assert.ok(logged(`error:   slot change would leave user sample ${KICKS_ID}`));
    const e = makeEngine({ engine: "testeng", memoryslot: 86, engine_version: 1,
      plug: { id: 60950, version: 1, trackmask: 1, flags: 0 } }, [], new Uint8Array(64));
    assert.equal(await opz.install(e, [0], 3, true, { yes: true }), false);
    assert.equal(dev.syncCount, 0);
    assert.ok(await opz.assign(10, [0], 4, true, { yes: true })); // another slot: fine
  });

  it("writeSlotMap: clear or overwrite refused; move and swap allowed", async () => {
    let base = slots();
    assert.equal(await opz.writeSlotMap(unassign(base, KICKS_ID), base, "slots", { yes: true }), false); // clear
    assert.equal(await opz.writeSlotMap(assign(base, 0, 3, 121), base, "slots", { yes: true }), false); // overwrite
    assert.equal(dev.syncCount, 0);
    assert.ok(logged("Pick another slot"));
    assert.ok(await opz.writeSlotMap(assign(unassign(base, KICKS_ID), 0, 5, KICKS_ID), base, "slots", { yes: true })); // move
    base = slots();
    assert.ok(await opz.writeSlotMap(assign(assign(base, 0, 5, 121), 0, 1, KICKS_ID), base, "slots", { yes: true })); // swap
    const kick = slots().tracks.find((t: Doc) => t.index === 0).packs;
    assert.deepEqual(kick, [{ id: KICKS_ID, slot: 1 }, { id: 121, slot: 5 }]);
  });
});

describe("FakeOpz.contentMode", () => {
  it("drops loose user samples and deletes unnamed files in samplepacks/user/", () => {
    dev.files.set("samplepacks/user/stray.aif", utf8("FORM"));
    const ours = OUR_SAMPLE_PREFIX + "testkit.aif";
    const p = plugs();
    p.plugs.push({ engine: "engines/sampleplayergain.engine", flags: 0, id: 61400, samples: ours, version: 1 });
    dev.files.set(PLUGS, utf8(dumps(p)));
    dev.files.set(ours, utf8("FORM-ours"));
    const gone = dev.contentMode();
    assert.deepEqual(gone, ["samplepacks/user/stray.aif", USER_SYNTH]); // the loose user sample is lost
    assert.ok(dev.files.has(USER_KICKS)); // assigned: kept
    assert.ok(dev.files.has(ours)); // ours (flag 0, named by a plug): kept even in no slot
    const ids = plugs().plugs.map((x: Doc) => x.id);
    assert.ok(!ids.includes(SYNTH_ID) && ids.includes(KICKS_ID) && ids.includes(61400));
    assert.ok(dev.log.includes(`found unassigned or dangling user sample ${SYNTH_ID}, removing`));
  });

  it("a user sample whose file is missing is dropped too", () => {
    dev.files.delete(USER_KICKS);
    dev.contentMode();
    assert.ok(!plugs().plugs.some((x: Doc) => x.id === KICKS_ID));
  });
});
