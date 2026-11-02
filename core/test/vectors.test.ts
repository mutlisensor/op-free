/**
 * Shared test vectors (tools/record_vectors.py): the Python tools against tools/fake_opz.py, every frame recorded.
 *
 * replay:     the TypeScript host against the recorded device frames must send byte-identical frames, call
 *             receive() with the same timeouts, and log the same lines.
 * end-to-end: the TypeScript host against the TypeScript FakeOpz must reproduce the whole transcript (both
 *             directions) and leave the device files exactly as the Python run did.
 */

import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { describe, it } from "node:test";

import {
  type BackupStore, FakeOpz, type FakeState, FileSession, Opz, PLUGS, ReplayTransport, SLOTS, SessionError, type Transport,
  type VectorEvent, VirtualTransport, fromHex, makeEngine, readU32le, toHex,
} from "../src/index.ts";

const DIR = new URL("../test-vectors/", import.meta.url);

interface Vector {
  name: string;
  description: string;
  clock: { start: number; latency: number };
  device: {
    variant: string; client_id: number; files_before: Record<string, string>; files_after: Record<string, string>;
    state_before: FakeState; state_after: FakeState;
  };
  inputs: Record<string, any>;
  result: Record<string, any>;
  backups?: Array<{ token: string; label: string }>;
  logs: string[];
  events: VectorEvent[];
}

const vectors: Vector[] = readdirSync(DIR)
  .filter((f) => f.endsWith(".json"))
  .map((f) => JSON.parse(readFileSync(new URL(f, DIR), "utf8")))
  .filter((v) => Array.isArray(v.events));

/** Hands out the recorded backup ids in order; restore vectors start with the backup they restore. */
class TokenStore implements BackupStore {
  readonly backups = new Map<string, Map<string, Uint8Array>>();
  private readonly pending: Array<{ token: string; label: string }>;

  constructor(v: Vector) {
    this.pending = [...(v.backups ?? [])];
    if (v.inputs.op === "restore") this.backups.set(v.inputs.backup, new Map([[SLOTS, fromHex(v.inputs.slots)]]));
  }

  async save(label: string, files: Map<string, Uint8Array>): Promise<string> {
    const next = this.pending.shift();
    assert.ok(next, `unexpected backup ${label}`);
    assert.equal(label, next.label);
    this.backups.set(next.token, new Map(files));
    return next.token;
  }

  async load(id: string): Promise<Map<string, Uint8Array>> {
    const files = this.backups.get(id);
    assert.ok(files, `no backup ${id}`);
    return files;
  }

  async annotate(id: string, name: string, data: Uint8Array): Promise<void> {
    (await this.load(id)).set(name, data);
  }
}

/** tools/record_vectors.py FlakyOpz: ignores the first advert, drops part 1 of the first plugs.json stream. */
class FlakyOpz extends FakeOpz {
  ignoredAdvert = false;
  droppedPart = false;

  protected override on51(p: Uint8Array): void {
    if (this.hello && !this.ignoredAdvert) {
      this.ignoredAdvert = true;
      return;
    }
    super.on51(p);
  }

  protected override on35(p: Uint8Array): void {
    const queued = this.out.length;
    super.on35(p);
    if (!this.droppedPart && readU32le(p, 2) === 1 && this.out.length - queued > 2) {
      this.out.splice(queued + 1, 1);
      this.droppedPart = true;
    }
  }
}

function device(v: Vector): FakeOpz {
  const files = new Map(Object.entries(v.device.files_before).map(([k, h]) => [k, fromHex(h)] as [string, Uint8Array]));
  const dev = v.device.variant === "flaky" ? new FlakyOpz(files, v.device.client_id)
    : new FakeOpz(files, v.device.client_id, v.device.variant === "keep-patch");
  dev.loadState(v.device.state_before);
  return dev;
}

/** Run the vector's flow; returns its result and log lines. */
async function run(v: Vector, transport: Transport, clock: () => number): Promise<{ result: any; logs: string[] }> {
  const logs: string[] = [];
  const log = (line: string) => logs.push(line);
  const i = v.inputs;
  if (v.name === "session-pull") {
    const s = new FileSession(transport, { log, clock });
    await s.open();
    const plugs = await s.pull(PLUGS);
    const slots = await s.pull(SLOTS);
    try {
      await s.pull("engines/crush.engine");
    } catch (e) {
      if (!(e instanceof SessionError)) throw e;
      log(`error: ${e.message}`);
    }
    await s.close();
    return { result: { plugs: toHex(plugs), slots: toHex(slots) }, logs };
  }
  const opz = new Opz(transport, { log, backups: new TokenStore(v), confirm: () => true, clock });
  let ok: boolean;
  switch (i.op) {
    case "install": {
      const engine = makeEngine(i.engine.manifest, i.engine.presets, fromHex(i.engine.data), i.engine.name);
      ok = await opz.install(engine, i.tracks, i.slot, i.replace, { yes: true });
      break;
    }
    case "assign":
      ok = await opz.assign(i.plug_id, i.tracks, i.slot, i.replace, { yes: true });
      break;
    case "remove":
      ok = await opz.remove(i.plug_id, i.unassign, { yes: true });
      break;
    case "restore":
      ok = await opz.restore(i.backup, { yes: true, touch: i.touch });
      break;
    case "presets-push":
    case "presets-push-twice": {
      const engine = makeEngine(i.engine.manifest, i.engine.presets, fromHex(i.engine.data), i.engine.name);
      ok = await opz.presetsPush(engine, { yes: true });
      if (ok && i.op === "presets-push-twice") ok = await opz.presetsPush(engine, { yes: true });
      break;
    }
    case "presets-persist": {
      const engines = i.engines.map((e: any) => makeEngine(e.manifest, e.presets, fromHex(e.data), e.name));
      ok = await opz.persistPresets(engines, { yes: true, off: i.off });
      break;
    }
    case "audition":
      ok = await opz.audition(i.track, i.values, (s) => s.pump(i.hold), i.label, { yes: true });
      break;
    case "sound-set":
      ok = await opz.setSound(i.track, i.values, { yes: true });
      break;
    case "watch":
      ok = await opz.watch(i.tracks, i.seconds);
      break;
    default:
      throw new Error(`unknown op ${i.op}`);
  }
  return { result: { ok }, logs };
}

describe("shared test vectors", () => {
  assert.ok(vectors.length >= 8, "test-vectors/ is missing (run python3 tools/record_vectors.py)");
  for (const v of vectors) {
    describe(v.name, () => {
      it(`replays byte for byte: ${v.description}`, async () => {
        const t = new ReplayTransport(v.events, v.clock);
        let outcome: Awaited<ReturnType<typeof run>> | undefined;
        try {
          outcome = await run(v, t, t.clock);
        } catch (e) {
          throw t.mismatch ?? e;
        }
        if (t.mismatch) throw t.mismatch;
        assert.ok(t.finished, `transcript not finished: stopped at event ${t.position} of ${v.events.length}`);
        assert.deepEqual(outcome.result, v.result);
        assert.deepEqual(outcome.logs, v.logs);
      });

      it("reproduces the transcript against the TypeScript FakeOpz", async () => {
        const dev = device(v);
        const t = new VirtualTransport(dev, v.clock);
        const { result, logs } = await run(v, t, t.clock);
        assert.deepEqual(result, v.result);
        assert.deepEqual(logs, v.logs);
        assert.equal(t.events.length, v.events.length);
        t.events.forEach((e, n) => assert.deepEqual(e, v.events[n], `event ${n}`));
        assert.deepEqual(Object.fromEntries([...dev.files].map(([k, b]) => [k, toHex(b)])), v.device.files_after);
        assert.deepEqual(dev.state(), v.device.state_after);
      });
    });
  }
});
