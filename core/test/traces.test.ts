/**
 * Trace vectors (test-vectors/traces/*.json, written by tools/import_trace.py from the web app's SysEx traces): the
 * TypeScript decoder must read every recorded frame exactly as the Python side did, and reassemble the same files
 * from the parts. Hardware runs cannot be replayed byte for byte (real time decides the empty receives), so this
 * pins the decoding of what the device really sent. The demo vector is FakeOpz's, until hardware traces join it.
 */

import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { describe, it } from "node:test";

import { ProtocolError, decodeFrame, decompress, fromHex, readU16le, readU32le, toHex } from "../src/index.ts";

const DIR = new URL("../test-vectors/traces/", import.meta.url);

interface TraceVector {
  name: string;
  kind: string;
  source: { transport: string };
  frames: Array<{ event: number; dir: "in" | "out"; t: number; id: number | null; payload: string | null }>;
  pulls: Array<{ file_id: number; sha256: string; size: number; t_start: number; t_end: number }>;
  pushes: Array<{ file_id: number; sha256?: string; size?: number; complete: boolean; t_start?: number }>;
  events: Array<{ t: number; send?: string; recv?: string | null }>;
}

const vectors: TraceVector[] = readdirSync(DIR).filter((f) => f.endsWith(".json"))
  .map((f) => JSON.parse(readFileSync(new URL(f, DIR), "utf8")));

async function sha256(data: Uint8Array): Promise<string> {
  return toHex(new Uint8Array(await crypto.subtle.digest("SHA-256", data as Uint8Array<ArrayBuffer>)));
}

it("there is at least the demo trace vector", () => {
  assert.ok(vectors.some((v) => v.name === "demo-first-run"));
});

for (const v of vectors) {
  describe(`trace ${v.name} (${v.source.transport})`, () => {
    it("every frame decodes as in Python", () => {
      assert.equal(v.kind, "hardware-trace");
      for (const f of v.frames) {
        const e = v.events[f.event];
        const raw = fromHex(f.dir === "out" ? e.send! : e.recv!);
        if (f.id === null) {
          assert.throws(() => decodeFrame(raw), ProtocolError, `event ${f.event} decoded in TypeScript only`);
          continue;
        }
        const d = decodeFrame(raw);
        assert.equal(d.messageId, f.id, `event ${f.event}`);
        assert.equal(toHex(d.payload), f.payload, `event ${f.event}`);
      }
    });

    it("the pulled and served files reassemble to the same SHA-256", async () => {
      const parts = (dir: "in" | "out", fileId: number, from: number, to: number | undefined) => {
        const got = new Map<number, Uint8Array>();
        for (const f of v.frames) {
          if (f.dir !== dir || f.id !== 0x53 || f.t < from || (to !== undefined && f.t > to)) continue;
          const p = fromHex(f.payload!);
          if (readU32le(p, 2) !== fileId) continue;
          got.set(readU16le(p, 6), decompress(p.subarray(9)));
        }
        const out: Uint8Array[] = [];
        for (let i = 0; got.has(i); i++) out.push(got.get(i)!);
        const n = out.reduce((a, b) => a + b.length, 0);
        const all = new Uint8Array(n);
        let o = 0;
        for (const b of out) {
          all.set(b, o);
          o += b.length;
        }
        return all;
      };
      for (const p of v.pulls) {
        const data = parts("in", p.file_id, p.t_start, p.t_end);
        assert.equal(data.length, p.size);
        assert.equal(await sha256(data), p.sha256);
      }
      for (const p of v.pushes.filter((x) => x.complete)) {
        const next = v.pushes.find((x) => x.file_id === p.file_id && (x.t_start ?? 0) > (p.t_start ?? 0));
        const data = parts("out", p.file_id, p.t_start!, next?.t_start === undefined ? undefined : next.t_start - 1e-9);
        assert.equal(data.length, p.size);
        assert.equal(await sha256(data), p.sha256);
      }
    });
  });
}
