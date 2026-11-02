/** Port of tools/tests/test_opz_presets.py: presets (patch.json plugs[]) and live sound ($0E) against FakeOpz. */

import assert from "node:assert/strict";
import { beforeEach, describe, it } from "node:test";

import {
  BOOT_PATCH, DEFAULT_SOUND, type Doc, type Engine, FakeOpz, MemoryBackupStore, Opz, PlanError, SessionError,
  StateSession, VirtualTransport, bankStream, bootEntries, decompress, project, buildPresetsPatch, fromUtf8, decodeFrame, decodeSound, findPreset, formatSound, loads, makeEngine,
  presetKeys, presetKeysText, presetProblems, presetSlot, soundFrame,
} from "../src/index.ts";

function preset(slot = 0, index = 0, name = "p", values: number[] = Array(18).fill(7), octave = 0): Doc {
  return { slot, index, name, default_octave: octave, values: [...values] };
}

function engine(presets: Doc[] = [], mask = 0xf0): Engine {
  return makeEngine({
    engine: "testeng", memoryslot: 86, engine_version: 1,
    plug: { id: 60950, version: 1, name: "testeng", trackmask: mask, flags: 0, parameter_names: ["a", "b"] },
  }, presets, Uint8Array.of(0, 0x50, 0, 0xad));
}

let dev: FakeOpz;
let t: VirtualTransport;
let logs: string[];
let store: MemoryBackupStore;
let opz: Opz;
let confirmAnswer: boolean;

function setUp(): void {
  dev = new FakeOpz();
  t = new VirtualTransport(dev);
  logs = [];
  store = new MemoryBackupStore();
  confirmAnswer = true;
  opz = new Opz(t, { log: (l) => logs.push(l), backups: store, confirm: () => confirmAnswer, clock: t.clock });
}

function record(): Doc {
  return loads(dev.files.get("settings/plugs.json")!).plugs.find((p: Doc) => p.id === 60950);
}

describe("preset checks", () => {
  it("clean synth presets", () => {
    assert.deepEqual(presetProblems([0, 1, 2, 3].map((s) => preset(s, 0, `s${s}`)), 0xf0), [[], []]);
  });

  it("errors", () => {
    const [errors] = presetProblems([preset(0, 0, "v", Array(17).fill(1)), preset(4, 0, "x"), preset(0, 14, "y"),
      preset(0, 0, "a"), preset(0, 0, "b")]);
    for (const text of ["18 values", "slot must be 0..3", "index must be 0..13", "share slot 0 index 0"]) {
      assert.ok(errors.some((e) => e.includes(text)), text);
    }
  });

  it("effects read slot 0 only", () => {
    const [, warnings] = presetProblems([preset(0, 0), preset(1, 0, "lead")], 0x300);
    assert.equal(warnings.length, 1);
    assert.match(warnings[0], /slot 1 \(lead\) are never used/);
  });

  it("effects use preset 0 only", () => {
    const [, warnings] = presetProblems([preset(0, 0), preset(0, 1, "b"), preset(0, 2, "c")], 0x300);
    assert.equal(warnings.length, 1);
    assert.match(warnings[0], /^2 presets are past slot 0 index 0 never used: effect tracks \(fx1, fx2\) have no preset keys/);
    assert.deepEqual(presetProblems([preset(0, 0), preset(0, 1, "b")], 0x301)[1], []);
  });

  it("missing index 0 and long names", () => {
    const [, warnings] = presetProblems([preset(0, 1, "a twelve chr")], 0x10);
    assert.ok(warnings.some((w) => w.includes("keeps 11 characters")));
    assert.ok(warnings.some((w) => w.includes("no preset at slot 0 index 0: bass starts")));
  });

  it("flags LFO depth 0 (bipolar dial, 128 = off)", () => {
    const v = Array(18).fill(7);
    v[13] = 0;
    assert.ok(presetProblems([preset(0, 0, "p", v)], 0x10)[1].some((w) => w.includes("LFO depth 0")));
    v[13] = 128;
    assert.deepEqual(presetProblems([preset(0, 0, "p", v)], 0x10), [[], []]);
  });

  it("slot per track, lookup, formatting", () => {
    assert.deepEqual(Array.from({ length: 12 }, (_, i) => presetSlot(i)), [0, 0, 0, 0, 0, 1, 2, 3, 0, 0, 0, 0]);
    const ps = [preset(0, 0, "Deep"), preset(1, 2, "bright")];
    assert.equal(findPreset(ps, "deep").name, "Deep");
    assert.equal(findPreset(ps, null, 1, 2).name, "bright");
    assert.throws(() => findPreset(ps, "nope"), PlanError);
    assert.match(formatSound(Array(18).fill(1), ["rate", "depth"]), /^rate=1 depth=1 attack=1 /);
  });

  it("presets patch has plugs[] only", () => {
    const rec = { engine: "engines/x.engine", flags: 0, id: 60950, samples: "", version: 3 };
    const doc = loads(buildPresetsPatch(rec, [preset(0, 0, "p", undefined, -2)]));
    assert.deepEqual(Object.keys(doc), ["plugs"]);
    assert.equal(doc.plugs[0].version, 3);
    assert.equal(doc.plugs[0].presets[0].defaultOctave, -2);
    assert.equal(loads(buildPresetsPatch(rec, [preset()], 5)).nonce, 5);
  });
});

describe("lit preset keys", () => {
  it("synth tracks group by equal sets", () => {
    const presets = [0, 1, 2, 3].flatMap((s) => Array.from({ length: s ? 5 : 4 }, (_, i) => preset(s, i, `${s}${i}`)));
    assert.deepEqual(presetKeys(presets, 0xf0), [[[4], [0, 1, 2, 3]], [[5, 6, 7], [0, 1, 2, 3, 4]]]);
    assert.equal(presetKeysText("table", presets, 0xf0),
      "check on the OP-Z: hold the track with table chosen; lit white keys = presets (bass: white keys 1-4; lead, "
      + "arp, chord: white keys 1-5). Only white key 1 lit (it blinks after every sound-slot change) = no presets "
      + "there: opz.py presets push engines/table");
  });

  it("drums read slot 0, FX have none", () => {
    const presets = [preset(0, 0), preset(0, 1, "b"), preset(0, 3, "d"), preset(1, 0, "lead")];
    assert.deepEqual(presetKeys(presets, 0x30f), [[[0, 1, 2, 3], [0, 1, 3]]]);
    assert.ok(presetKeysText("kit", presets, 0x30f).includes("kick, snare, perc, sample: white keys 1-2, 4)"));
    assert.equal(presetKeysText("swirl", presets, 0x300), "");
  });

  it("no trackmask: the filled slots; empty tracks listed", () => {
    assert.deepEqual(presetKeys([preset(2, 13)], 0), [[[6], [13]]]);
    assert.ok(presetKeysText("x", [preset(2, 13)], 0).includes("arp: white key 14)"));
    assert.ok(presetKeysText("x", [preset(0, 0)], 0x30).includes("(bass: white key 1; lead: none)"));
  });
});

describe("presets push", () => {
  beforeEach(setUp);

  async function install(presets: Doc[]): Promise<void> {
    assert.ok(await opz.install(engine(presets), [4], null, false, { yes: true }));
  }

  it("replaces presets without a version bump and merges", async () => {
    await install([preset(0, 0, "old"), preset(0, 1, "keep")]);
    const before = record();
    assert.ok(await opz.presetsPush(engine([preset(0, 0, "new", Array(18).fill(9))]), { yes: true }));
    assert.deepEqual(record(), before);
    assert.equal(dev.presets.get(60950)!.get("0/0")!.name, "new");
    assert.equal(dev.presets.get(60950)!.get("0/1")!.name, "keep");
    assert.ok([...store.backups.values()].at(-1)!.has("presets.json"));
  });

  it("not installed: nothing written", async () => {
    const n = dev.syncCount;
    assert.equal(await opz.presetsPush(engine([preset()]), { yes: true }), false);
    assert.equal(dev.syncCount, n);
    assert.ok(logs.some((l) => l.includes("not installed")));
  });

  it("a kept patch.json is re-sent with a nonce", async () => {
    dev.keepPatch = true;
    await install([preset()]);
    const e = engine([preset(0, 0, "again")]);
    assert.ok(await opz.presetsPush(e, { yes: true }));
    assert.ok(await opz.presetsPush(e, { yes: true }));
    assert.ok(logs.some((l) => l.includes("with a nonce")));
    assert.ok("nonce" in loads(dev.files.get("settings/patch.json")!));
  });

  it("restart loses presets; preset 0 applies with pan kept", async () => {
    await install([preset(0, 0, "bass", Array.from({ length: 18 }, (_, i) => i), -2)]);
    assert.ok(dev.applyPreset(4, 60950));
    assert.deepEqual(dev.sounds[4].slice(0, 10), [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
    assert.equal(dev.sounds[4][10], DEFAULT_SOUND[10]);
    assert.equal(dev.applyPreset(5, 60950), false);
    dev.restart();
    assert.equal(dev.applyPreset(4, 60950), false);
  });

  it("install and push say presets are RAM-only and which keys light", async () => {
    await install([preset(0, 0), preset(0, 1, "b")]);
    assert.ok(logs.includes("The OP-Z keeps the presets this install sent in RAM only: a restart or crash drops them "
      + "(research/presets.md); then run: opz.py presets push engines/testeng"));
    assert.ok(logs.some((l) => l.startsWith("check on the OP-Z") && l.includes("bass: white keys 1-2;")));
    logs.length = 0;
    assert.ok(await opz.presetsPush(engine([preset(0, 0)]), { yes: true }));
    assert.ok(logs.at(-1)!.includes("bass: white key 1;"));
  });

  it("an engine update says the restart drops the presets", async () => {
    await install([preset()]);
    const update = makeEngine({
      engine: "testeng", memoryslot: 86, engine_version: 2,
      plug: { id: 60950, version: 2, name: "testeng", trackmask: 0xf0, flags: 0, parameter_names: ["a", "b"] },
    }, [preset()], Uint8Array.of(1, 1, 1, 1));
    assert.ok(await opz.install(update, [], null, false, { yes: true }));
    assert.ok(logs.some((l) => l.startsWith("The restart also drops the presets this install sent")
      && l.includes("after it, run: opz.py presets push engines/testeng")));
  });

  it("no presets: no key line", async () => {
    await install([]);
    assert.ok(!logs.some((l) => l.includes("presets push")));
  });

  it("dry run shows the patch", async () => {
    await install([preset()]);
    const n = dev.syncCount;
    assert.ok(await opz.presetsPush(engine([preset(0, 0, "dry")]), { dryRun: true }));
    assert.equal(dev.syncCount, n);
    assert.ok(logs.some((l) => l.includes('"plugs":[')));
  });
});

describe("boot file (settings/plugs_patch.json)", () => {
  const eng = engine([preset(0, 0, "bass", [...Array(18).keys()], -2), preset(1, 3, "lead")]);
  beforeEach(async () => {
    setUp();
    assert.ok(await opz.install(eng, [4], null, false, { yes: true, boot: true }));
  });
  const bootDoc = () => loads(dev.files.get(BOOT_PATCH)!);

  it("survives exactly one restart", async () => {
    assert.ok(await opz.persistPresets([eng], { yes: true }));
    const doc = bootDoc();
    assert.deepEqual(Object.keys(doc), ["plugs"]);
    const { presets: _, ...entry } = doc.plugs[0];
    assert.deepEqual(entry, record());
    dev.restart();
    assert.ok(!dev.files.has(BOOT_PATCH), "the start deletes it");
    assert.ok(dev.applyPreset(4, 60950));
    dev.restart();
    assert.ok(!dev.applyPreset(4, 60950), "the file is gone after one start");
  });

  it("install and push say the boot file brings the presets back", async () => {
    assert.ok(logs.some((l) => l.startsWith("The OP-Z keeps the presets this install sent in RAM; the boot file")));
    assert.ok(logs.some((l) => l.startsWith("check on the OP-Z")));
    logs.length = 0;
    assert.ok(await opz.presetsPush(eng, { yes: true, boot: true }));
    assert.ok(logs.some((l) => l.includes("the boot file written next")));
  });

  it("only our installed plugs with presets; a copy in the backup", async () => {
    const plugs = loads(dev.files.get("settings/plugs.json")!);
    const te = makeEngine({ engine: "analoga", memoryslot: 27, plug: { id: 10, version: 1 } }, [preset()], new Uint8Array());
    const moved = makeEngine({ engine: "moved", memoryslot: 85, plug: { id: 60950, version: 1 } }, [preset()], new Uint8Array());
    assert.deepEqual(bootEntries(plugs, [te, eng])[1], ["testeng 60950 (2)"]);
    const [entries, , warnings] = bootEntries(plugs, [moved, eng]);
    assert.deepEqual(entries, []);
    assert.match(warnings[0], /plug 60950 is engines\/testeng.engine on the OP-Z, not engines\/moved.engine/);
    assert.ok(await opz.persistPresets([eng], { yes: true }));
    const id = [...store.backups.keys()].at(-1)!;
    assert.ok(id.endsWith("-boot-presets"));
    assert.equal(fromUtf8(store.backups.get(id)!.get("plugs_patch.json")!), fromUtf8(dev.files.get(BOOT_PATCH)!));
  });

  it("remove, then persist: the plug does not come back; off empties it", async () => {
    assert.ok(await opz.persistPresets([eng], { yes: true }));
    assert.ok(await opz.remove(60950, true, { yes: true }));
    assert.ok(await opz.persistPresets([eng], { yes: true }));
    assert.deepEqual(bootDoc(), { plugs: [] });
    dev.restart();
    assert.equal(record(), undefined);
    setUp();
    assert.ok(await opz.persistPresets([eng], { yes: true, off: true }));
    assert.ok(logs.includes("boot file emptied and verified."));
  });

  it("dry run, decline and unchanged", async () => {
    assert.ok(await opz.persistPresets([eng], { dryRun: true }));
    assert.ok(!dev.files.has(BOOT_PATCH));
    confirmAnswer = false;
    assert.ok(!(await opz.persistPresets([eng])));
    assert.ok(!dev.files.has(BOOT_PATCH));
    confirmAnswer = true;
    assert.ok(await opz.persistPresets([eng], { yes: true }));
    assert.ok(await opz.persistPresets([eng], { yes: true }));
    assert.ok(logs.some((l) => l.includes("already holds this boot file")));
  });

  it("an unparsable boot file is deleted at the start", () => {
    dev.files.set(BOOT_PATCH, new TextEncoder().encode("{not json"));
    dev.restart();
    assert.ok(!dev.files.has(BOOT_PATCH));
    assert.ok(dev.log.includes("Failed to import update file"));
  });
});

describe("$0E sound", () => {
  beforeEach(setUp);

  it("frame round trip", () => {
    const raw = soundFrame(5, Array.from({ length: 18 }, (_, i) => i));
    assert.equal(raw[5], 0x0e);
    assert.deepEqual(decodeSound(decodeFrame(raw).payload), [5, Array.from({ length: 18 }, (_, i) => i)]);
    assert.throws(() => soundFrame(16, Array(18).fill(0)), RangeError);
  });

  it("audition restores, even after an error", async () => {
    const seen: number[][] = [];
    assert.ok(await opz.audition(5, Array(18).fill(1), async () => {
      seen.push([...dev.sounds[5]]);
    }, "x", { yes: true }));
    assert.deepEqual(seen, [Array(18).fill(1)]);
    assert.deepEqual(dev.sounds[5], DEFAULT_SOUND);
    await assert.rejects(opz.audition(5, Array(18).fill(2), async () => {
      throw new Error("boom");
    }, "x", { yes: true }), /boom/);
    assert.deepEqual(dev.sounds[5], DEFAULT_SOUND);
  });

  it("declined audition writes nothing", async () => {
    confirmAnswer = false;
    assert.equal(await opz.audition(5, Array(18).fill(1), async () => {}), false);
    assert.ok(!t.events.some((e) => "send" in e && e.send.slice(10, 12) === "0e"));
  });

  it("set stays and logs the undo line; a live session waits for expiry", async () => {
    await opz.readSettings();
    assert.ok(await opz.setSound(4, Array(18).fill(3), { yes: true }));
    assert.deepEqual(dev.sounds[4], Array(18).fill(3));
    assert.ok(logs.some((l) => l.includes("waiting 6.5 s")));
    assert.ok(logs.some((l) => l.startsWith("set. to undo: opz.py presets set --track bass --values 128,")));
  });

  it("no $01: error", async () => {
    class Mute extends FakeOpz {
      protected override on00(): void {}
    }
    const mt = new VirtualTransport(new Mute());
    await assert.rejects(new StateSession(mt, { clock: mt.clock }).open([4]), SessionError);
  });

  it("watch sees the OP-Z apply a preset", async () => {
    dev.soundBurst = true; // the notes' description: $0E on a change (not 1.2.45)
    await opz.install(engine([preset(0, 0, "w", Array(18).fill(5))]), [4], null, false, { yes: true });
    const s = new StateSession(t, { clock: t.clock });
    await s.open([4]);
    dev.applyPreset(4, 60950);
    const want = [...Array(10).fill(5), DEFAULT_SOUND[10], ...Array(7).fill(5)];
    assert.ok((await s.pump(1)).some(([track, v]) => track === 4 && v.join() === want.join()));
  });
});

describe("1.2.45 bank burst (Q-44)", () => {
  beforeEach(setUp);

  const sentIds = (tr: VirtualTransport): number[] =>
    tr.events.filter((e) => "send" in e).map((e) => parseInt((e as { send: string }).send.slice(10, 12), 16));

  async function open(tr: VirtualTransport, tracks = [4]): Promise<StateSession> {
    logs.length = 0;
    const s = new StateSession(tr, { log: (l) => logs.push(l), clock: tr.clock });
    await s.open(tracks);
    return s;
  }

  it("bankStream inflates back", () => {
    const bank = dev.activeProject().bank;
    assert.deepEqual(decompress(bankStream(bank)), bank);
    const odd = Uint8Array.from({ length: 70_000 }, (_, i) => (i * 7919) % 251 % (i % 5 ? 256 : 3));
    assert.deepEqual(decompress(bankStream(odd)), odd);
    for (const n of [0, 1, 2, 3, 5]) assert.equal(decompress(bankStream(new Uint8Array(n))).length, n);
  });

  it("sounds come from the bank, every part but the last acked", async () => {
    dev.sounds[4] = Array.from({ length: 18 }, (_, i) => i);
    const s = await open(t, [4, 7]);
    assert.deepEqual([...s.sounds.keys()].sort((a, b) => a - b), Array.from({ length: 16 }, (_, i) => i));
    assert.deepEqual(s.sounds.get(4), dev.sounds[4]);
    assert.equal(s.bankAddress, 0x82);
    assert.equal(s.playAddress, 0x82);
    assert.ok(!s.seen.has(0x0e));
    const parts = (s.seen.get(0x09) ?? 0) + (s.seen.get(0x0a) ?? 0);
    assert.ok(parts > 2);
    assert.equal(sentIds(t).filter((m) => m === 0x0b).length, parts - 1);
    assert.ok(!sentIds(t).includes(0x08));
  });

  it("the bank is the active project's, pattern from the address", async () => {
    const pr = new project.Project();
    pr.setTempo(123);
    dev.files.set(project.projectPath(2), pr.toBytes());
    dev.project = 1;
    dev.pattern = 11;
    dev.sounds[5] = Array(18).fill(77);
    const s = await open(t, [5]);
    assert.equal(s.bankAddress, 0x1b);
    assert.deepEqual(s.sounds.get(5), Array(18).fill(77));
    assert.deepEqual(s.bank, dev.activeProject().bank);
  });

  it("a lost part is asked for again", async () => {
    class Lossy extends FakeOpz {
      dropped = false;
      protected override sendBankPart(): void {
        super.sendBankPart();
        if (this.bankOut && this.bankOut.next === 3 && !this.dropped) {
          this.dropped = true;
          this.bankOut.next += 1;
          super.sendBankPart();
        }
      }
    }
    const lossy = new Lossy();
    const lt = new VirtualTransport(lossy);
    const s = await open(lt);
    assert.deepEqual(s.sounds.get(4), DEFAULT_SOUND);
    assert.equal(sentIds(lt).filter((m) => m === 0x08).length, 1);
    assert.equal(lossy.banksSent, 2);
    assert.ok(logs.includes("bank part 4 out of order: asking for it again"));
  });

  it("a stalled transfer is asked for again", async () => {
    class Stall extends FakeOpz {
      ignored = false;
      protected override on0b(p: Uint8Array): void {
        if (!this.ignored) {
          this.ignored = true;
          return;
        }
        super.on0b(p);
      }
    }
    const st = new VirtualTransport(new Stall());
    const s = await open(st);
    assert.deepEqual(s.sounds.get(4), DEFAULT_SOUND);
    assert.ok(logs.includes("bank transfer stalled: asking for it again"));
  });

  it("a bad stream is refused after two re-requests", async () => {
    class Garbage extends FakeOpz {
      protected override sendBank(): void {
        this.bankOut = { chunks: [new Uint8Array(178), Uint8Array.of(1, 2)], next: 0 };
        this.banksSent += 1;
        this.sendBankPart();
      }
    }
    const gt = new VirtualTransport(new Garbage());
    await assert.rejects(open(gt), SessionError);
    assert.equal(sentIds(gt).filter((m) => m === 0x08).length, 2);
    assert.ok(!sentIds(gt).includes(0x0e));
  });

  it("a pattern change during the read is refused", async () => {
    class Moved extends FakeOpz {
      protected override playState(): Uint8Array {
        const state = super.playState();
        state[17] += 1;
        return state;
      }
    }
    await assert.rejects(open(new VirtualTransport(new Moved())), /not the bank's/);
  });

  it("the notes' $0E burst still works", async () => {
    const bt = new VirtualTransport(new FakeOpz(undefined, 21899, false, true));
    const s = await open(bt);
    assert.deepEqual(s.sounds.get(4), DEFAULT_SOUND);
    assert.equal(s.bank, null);
    assert.ok(!sentIds(bt).includes(0x0b));
  });
});
