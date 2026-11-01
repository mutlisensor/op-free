/// <reference types="node" />
/**
 * The plan against an OP-Z: picks into slots, the visitor's samples never touched, original vs community, installs,
 * and the restore target. The device is the demo OP-Z (src/loader/demo.ts): every drum and synth slot filled, three
 * user samples on the sample track (slots 4, 9, 10).
 */

import assert from "node:assert/strict";
import { test } from "node:test";
import { type Doc, assign, loads, slotCells } from "../core.ts";
import { DEMO_USER_SAMPLES, demoFiles } from "../demo.ts";
import { type Choice, defaultChoice, setSource, toggleOriginalSlot } from "../model.ts";
import { type Device, blockedSlots, cellsToDoc, planLoader, plugLabel, restoreTarget, userSampleCells } from "../plan.ts";
import { catalogue } from "./fixtures.ts";

const engines = catalogue();
const id = (dir: string) => engines.find((e) => e.dir === dir)!.manifest.plug.id;

function demoDevice(): Device {
  const f = demoFiles();
  return { plugs: loads(f.get("settings/plugs.json")!), slots: loads(f.get("settings/slotConfiguration.json")!) };
}

function cell(slots: Doc, t: number, s: number): number | null {
  return slotCells(slots).get(`${t}:${s}`) ?? null;
}

/** The device after the plan was written (installs added, target slot map). */
function applied(d: Device, choice: Choice, original: Doc | null = null): Device {
  const plan = planLoader({ engines, choice, device: d, original, demo: true });
  assert.deepEqual(plan.errors, []);
  const plugs = structuredClone(d.plugs);
  for (const i of plan.installs) {
    plugs.plugs = [...plugs.plugs.filter((p: Doc) => p.id !== i.plugId),
      { engine: `engines/${i.dir}.engine`, flags: 0, id: i.plugId, samples: "", version: i.version }];
  }
  return { plugs, slots: plan.target };
}

test("the default picks fill slots 1–8 of every track in the group; 9 and 10 stay", () => {
  const d = demoDevice();
  const plan = planLoader({ engines, choice: defaultChoice(engines), device: d, original: d.slots, demo: true });
  assert.deepEqual(plan.errors, []);
  for (const t of [4, 5, 6, 7]) {
    for (let s = 1; s <= 8; s++) assert.equal(cell(plan.target, t, s), id(`syn${s}`), `synth track ${t} slot ${s}`);
    assert.equal(cell(plan.target, t, 9), cell(d.slots, t, 9));
    assert.equal(cell(plan.target, t, 10), cell(d.slots, t, 10));
  }
  assert.deepEqual(plan.installs.map((i) => [i.dir, i.action]), [
    ...["syn1", "syn2", "syn3", "syn4", "syn5", "syn6", "syn7", "syn8"].map((x) => [x, "install"]),
    ...["drm1", "drm2", "drm3"].map((x) => [x, "install"]),
    ...["fxa", "fxb", "fxonly1", "fxd"].map((x) => [x, "install"]),
  ]);
  assert.equal(cell(plan.target, 10, 1), cell(d.slots, 10, 1), "tape untouched");
  assert.equal(cell(plan.target, 11, 1), cell(d.slots, 11, 1), "master untouched");
});

test("a slot holding the visitor's sample is skipped on that track only, and never changes", () => {
  const d = demoDevice();
  const choice = defaultChoice(engines);
  choice.drum.picks = ["drm1", "drm2", "drm3", "drm4", "drm5"];
  const plan = planLoader({ engines, choice, device: d, original: d.slots, demo: true });
  assert.deepEqual(plan.errors, []);
  const [sampleAt4] = DEMO_USER_SAMPLES;
  assert.equal(cell(plan.target, 3, 4), sampleAt4[0], "sample track slot 4 keeps the user sample");
  for (const t of [0, 1, 2]) assert.equal(cell(plan.target, t, 4), id("drm4"), `drm4 on track ${t} slot 4`);
  assert.deepEqual(plan.skipped.map((c) => [c.track, c.slot]), [[3, 4]]);
  for (const [pid, s] of DEMO_USER_SAMPLES) assert.equal(cell(plan.target, 3, s), pid);
  assert.ok(!plan.changes.some((c) => c.why === "user-sample"));
});

test("no choice ever changes a cell that holds a user sample", () => {
  const d = demoDevice();
  const samples = userSampleCells(d);
  const all = engines.filter((e) => e.kind === "drum").map((e) => e.dir);
  const choices: Choice[] = [];
  for (let n = 0; n <= all.length; n++) {
    const c = defaultChoice(engines);
    c.drum.picks = all.slice(0, n);
    choices.push(c, { ...c, drum: setSource(c.drum, "original") }, { ...c, drum: toggleOriginalSlot(c.drum, 4).choice });
  }
  for (const c of choices) {
    const plan = planLoader({ engines, choice: c, device: d, original: d.slots, demo: true });
    for (const [k, pid] of samples) assert.equal(slotCells(plan.target).get(k), pid, k);
  }
});

test("a slot with user samples on every track of the group is skipped by the picks", () => {
  const d = demoDevice();
  let slots = d.slots;
  for (const t of [0, 1, 2, 3]) slots = assign(slots, t, 2, 1_513_459_487);
  const device = { plugs: d.plugs, slots };
  assert.deepEqual(blockedSlots("drum", userSampleCells(device)), [2]);
  const plan = planLoader({ engines, choice: defaultChoice(engines), device, original: slots, demo: true });
  assert.deepEqual(plan.blocked.drum, [2]);
  for (const t of [0, 1, 2]) {
    assert.equal(cell(plan.target, t, 1), id("drm1"));
    assert.equal(cell(plan.target, t, 2), 1_513_459_487);
    assert.equal(cell(plan.target, t, 3), id("drm2"));
    assert.equal(cell(plan.target, t, 4), id("drm3"));
  }
});

test("original puts back what the first backup had; community and original per slot", () => {
  const first = demoDevice();
  const after = applied(first, defaultChoice(engines), first.slots);
  assert.equal(cell(after.slots, 5, 3), id("syn3"));

  const choice = defaultChoice(engines);
  choice.synth = toggleOriginalSlot(choice.synth, 3).choice;
  const plan = planLoader({ engines, choice, device: after, original: first.slots, demo: true });
  for (const t of [4, 5, 6, 7]) {
    assert.equal(cell(plan.target, t, 3), cell(first.slots, t, 3), "slot 3 is original again");
    assert.equal(cell(plan.target, t, 4), id("syn3"), "syn3 moves to the next free slot");
    assert.equal(cell(plan.target, t, 9), id("syn8"));
  }
  assert.ok(plan.installs.every((i) => i.action === "ready"), "nothing to install again");

  choice.synth = setSource(choice.synth, "original");
  const back = planLoader({ engines, choice, device: after, original: first.slots, demo: true });
  for (const t of [4, 5, 6, 7]) for (let s = 1; s <= 10; s++) assert.equal(cell(back.target, t, s), cell(first.slots, t, s));
  assert.ok(back.changes.every((c) => c.why === "original"));
  assert.ok(!back.installs.some((i) => i.dir.startsWith("syn")), "no synth is placed, so none is sent");
});

test("an engine no longer picked goes back to the original; others are left as they are", () => {
  const first = demoDevice();
  const after = applied(first, defaultChoice(engines), first.slots);
  const choice = defaultChoice(engines);
  choice.synth.picks = choice.synth.picks.slice(0, 6);
  const plan = planLoader({ engines, choice, device: after, original: first.slots, demo: true });
  for (const t of [4, 5, 6, 7]) {
    assert.equal(cell(plan.target, t, 7), cell(first.slots, t, 7));
    assert.equal(cell(plan.target, t, 8), cell(first.slots, t, 8));
  }
  assert.deepEqual(plan.changes.map((c) => c.why).filter((w, i, a) => a.indexOf(w) === i), ["revert"]);
  const unknown = planLoader({ engines, choice, device: after, original: null, demo: true });
  assert.equal(unknown.changes.length, 0, "without a first backup the original is unknown: nothing moves");
});

test("an engine made for fx1 only leaves fx2's slot alone", () => {
  const d = demoDevice();
  const plan = planLoader({ engines, choice: defaultChoice(engines), device: d, original: d.slots, demo: true });
  assert.equal(cell(plan.target, 8, 3), id("fxonly1"));
  assert.equal(cell(plan.target, 9, 3), cell(d.slots, 9, 3));
  assert.equal(plan.cells.find((c) => c.track === 9 && c.slot === 3)?.why, "not-for-track");
});

test("installs: new, already there, upgrade with restart; refusals", () => {
  const d = demoDevice();
  const after = applied(d, defaultChoice(engines), d.slots);
  const newer = engines.map((e) => (e.dir === "syn1" ? { ...e, manifest: { ...e.manifest, engine_version: 2, plug: { ...e.manifest.plug, version: 2 } } } : e));
  after.plugs.engines = [...(after.plugs.engines ?? []), { memoryslot: engines.find((e) => e.dir === "syn1")!.manifest.memoryslot, path: "engines/syn1.engine", version: 1 }];
  const plan = planLoader({ engines: newer, choice: defaultChoice(engines), device: after, original: d.slots, demo: true });
  const syn1 = plan.installs.find((i) => i.dir === "syn1")!;
  assert.equal(syn1.action, "upgrade");
  assert.equal(syn1.restart, true);
  assert.equal(plan.installs.find((i) => i.dir === "syn2")!.action, "ready");

  const unbuilt = engines.map((e) => (e.dir === "syn2" ? { ...e, engine: null } : e));
  assert.match(planLoader({ engines: unbuilt, choice: defaultChoice(engines), device: d, original: d.slots, demo: false }).errors.join(),
    /syn2 is not built yet/);
  assert.deepEqual(planLoader({ engines: unbuilt, choice: defaultChoice(engines), device: d, original: d.slots, demo: true }).errors, [],
    "the demo installs a placeholder");

  const clash = engines.map((e) => (e.dir === "syn3" ? { ...e, manifest: { ...e.manifest, memoryslot: 27 } } : e));
  assert.match(planLoader({ engines: clash, choice: defaultChoice(engines), device: d, original: d.slots, demo: true }).errors.join(),
    /syn3: memory slot 27 is already used by engines\/analoga.engine/, "the core's own check, before anything is asked");
  const twice = engines.map((e) => (e.dir === "syn4" ? { ...e, manifest: { ...e.manifest, memoryslot: engines.find((x) => x.dir === "syn5")!.manifest.memoryslot } } : e));
  assert.match(planLoader({ engines: twice, choice: defaultChoice(engines), device: d, original: d.slots, demo: true }).errors.join(),
    /syn5: memory slot \d+ is already used by engines\/syn4.engine/, "two engines of one batch in one memory slot");
});

test("a restore puts the backup's cells back but keeps the visitor's samples and missing plugs' cells", () => {
  const first = demoDevice();
  const after = applied(first, defaultChoice(engines), first.slots);
  // since the first backup: a new user sample in kick slot 10, and plug 3 removed
  const plugs = structuredClone(after.plugs);
  plugs.plugs.push({ engine: "engines/sampleplayergain.engine", flags: 2, id: 1_600_000_000, samples: "samplepacks/user/new.aif", version: 1 });
  plugs.plugs = plugs.plugs.filter((p: Doc) => p.id !== 3);
  const device = { plugs, slots: assign(after.slots, 0, 10, 1_600_000_000) };
  const { target, kept, missing } = restoreTarget(first.slots, device);
  assert.equal(cell(target, 0, 10), 1_600_000_000, "the new sample stays in its slot");
  assert.deepEqual(missing, [3]);
  for (const t of [4, 5, 6, 7]) assert.equal(cell(target, t, 3), id("syn3"), "plug 3 is gone: the cell is left as it is");
  assert.equal(cell(target, 4, 1), cell(first.slots, 4, 1));
  assert.ok(kept.some((c) => c.why === "user-sample") && kept.some((c) => c.why === "original-gone"));
});

test("names: ours by label, TE's by engine or sample file, the visitor's samples as theirs", () => {
  const d = demoDevice();
  assert.equal(plugLabel(d.plugs, 1, engines), "analoga");
  assert.equal(plugLabel(d.plugs, 101, engines), "demo-1");
  assert.match(plugLabel(d.plugs, DEMO_USER_SAMPLES[0][0], engines), /^your sample my-sample-/);
  assert.equal(plugLabel(d.plugs, id("syn1"), engines), "syn1");
  assert.equal(plugLabel(d.plugs, null), "empty");
  assert.equal(cellsToDoc(new Map([["4:2", 7], ["4:1", 8]]), { tracks: [{ index: 0, packs: [] }] }).tracks[1].packs[0].slot, 1);
});

test("an engine with a caution is refused on a real OP-Z, allowed in the demo", () => {
  const d = demoDevice();
  const risky = engines.map((e) => (e.dir === "syn2" ? { ...e, caution: "Froze the OP-Z twice." } : e));
  const real = planLoader({ engines: risky, choice: defaultChoice(engines), device: d, original: d.slots, demo: false });
  assert.match(real.errors.join(), /syn2 can't go on a real OP-Z yet: Froze the OP-Z twice/);
  const demo = planLoader({ engines: risky, choice: defaultChoice(engines), device: d, original: d.slots, demo: true });
  assert.deepEqual(demo.errors, []);
  assert.ok(demo.warnings.some((w) => w.startsWith("syn2: Froze")));
});
