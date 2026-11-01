/** sysex, zlib, pyjson and fileprotocol, checked against values from the Python implementation. */

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { describe, it } from "node:test";

import {
  DownloadState, MAX_COMPRESS_SIZE, MIDIStreamParser, ProtocolError, UploadState, ZlibError, compress, concat,
  decodeFrame, decompress, downloadFileId, dumps, fileId, frame, fromHex, heartbeat, pack7, pyRepr, readU16le,
  stateSyncClientHello, stateSyncMasterClaim, toHex, u16le, u32le, unpack7, utf8,
} from "../src/index.ts";
import * as zlib from "../src/zlib.ts";

const vector = (name: string) => JSON.parse(readFileSync(new URL(`../test-vectors/${name}.json`, import.meta.url), "utf8"));

describe("sysex", () => {
  it("frames like opzsysex", () => {
    const payload = Uint8Array.from({ length: 16 }, (_, i) => i * 17);
    assert.equal(toHex(frame(0x53, payload)), "f0002076015300001122334455667e7708192a3b4c5d036e7ff7");
    assert.equal(toHex(stateSyncClientHello(0x4242)), "f0002076010000424206f7");
    assert.equal(toHex(stateSyncMasterClaim(1, 0x1234)), "f000207601010000011234f7");
    assert.equal(toHex(heartbeat([3, 4])), "f0002076010001030406f7");
  });

  it("round-trips packed-7 and frames", () => {
    for (const n of [0, 1, 6, 7, 8, 13, 14, 15, 200, 257]) {
      const data = Uint8Array.from({ length: n }, (_, i) => (i * 151 + 7) & 0xff);
      assert.deepEqual(unpack7(pack7(data)), data);
      assert.deepEqual(decodeFrame(frame(0x35, data)), { messageId: 0x35, payload: data });
    }
  });

  it("rejects bad input", () => {
    assert.throws(() => unpack7(Uint8Array.of(0x80)), ProtocolError);
    assert.throws(() => frame(0x80), ProtocolError);
    assert.throws(() => decodeFrame(Uint8Array.of(0xf0, 0, 0x20, 0x76, 0x01, 0xf7)), ProtocolError);
    assert.throws(() => decodeFrame(Uint8Array.of(0xf0, 0, 0x20, 0x77, 0x01, 0x34, 0xf7)), ProtocolError);
  });

  it("parses a MIDI byte stream with realtime bytes inside SysEx", () => {
    const p = new MIDIStreamParser();
    assert.deepEqual(p.feed(Uint8Array.of(0x90, 0xf0, 0x01, 0xf8)), [{ type: "realtime", byte: 0xf8 }]);
    assert.deepEqual(p.feed(Uint8Array.of(0x02, 0xf7, 0xfe)), [
      { type: "sysex", data: Uint8Array.of(0xf0, 0x01, 0x02, 0xf7) }, { type: "realtime", byte: 0xfe }]);
    assert.deepEqual(p.feed(Uint8Array.of(0xf0, 0x05, 0xf0, 0x06, 0xf7)), [{ type: "sysex", data: Uint8Array.of(0xf0, 0x06, 0xf7) }]);
  });
});

describe("zlib", () => {
  it("compress() equals Python's zlib.compress for every case in deflate.json", () => {
    const v = vector("deflate");
    for (const [input, expected] of v.cases) assert.equal(toHex(compress(fromHex(input))), expected, `input ${input.length / 2} bytes`);
  });

  it("decompress() inverts Python's output", () => {
    for (const [input, expected] of vector("deflate").cases) assert.equal(toHex(decompress(fromHex(expected))), input);
  });

  it("follows Python's decompress semantics", () => {
    const z = compress(utf8("hello hello hello"));
    assert.deepEqual(decompress(concat(z, utf8("junk"))), utf8("hello hello hello")); // trailing bytes ignored
    assert.throws(() => decompress(z.subarray(0, z.length - 2)), ZlibError); // zlib.decompress: truncated
    const bad = Uint8Array.from(z);
    bad[bad.length - 1] ^= 1;
    assert.throws(() => decompress(bad), /incorrect data check/);
    assert.throws(() => decompress(Uint8Array.of(0x78, 0x9d)), /incorrect header check/);
    assert.throws(() => zlib.decompress(compress(new Uint8Array(300)), 200), zlib.OutputLimitError);
  });

  it("refuses inputs it cannot reproduce exactly", () => {
    assert.throws(() => compress(new Uint8Array(MAX_COMPRESS_SIZE + 1)), RangeError);
  });

  it("crc32 and adler32", () => {
    assert.equal(zlib.crc32(utf8("settings/patch.json")), 2809139765);
    assert.equal(zlib.adler32(utf8("Wikipedia")), 0x11e60398);
  });
});

describe("pyjson", () => {
  const v = vector("pyjson");
  it("dumps() equals Python's json.dumps", () => {
    for (const c of v.dumps) {
      const o = c.options;
      const got = dumps(c.value, { sortKeys: o.sort_keys, separators: o.separators, indent: o.indent, ensureAscii: o.ensure_ascii });
      assert.equal(got, c.expected, `${JSON.stringify(c.value)} ${JSON.stringify(o)}`);
    }
  });

  it("pyRepr() equals Python's repr", () => {
    for (const c of v.repr) assert.equal(pyRepr(c.value), c.expected);
  });
});

describe("fileprotocol", () => {
  it("file ids", () => {
    assert.equal(fileId("settings/patch.json"), 2809139765);
    assert.equal(fileId("Engines/Kit.engine"), 2197871376);
    assert.equal(downloadFileId("settings/plugs.json"), 1);
    assert.equal(downloadFileId("Settings/SlotConfiguration.json"), 0);
    assert.equal(downloadFileId("engines/kit.engine"), 2197871376);
  });

  it("UploadState serves the sync job like Python", () => {
    const u = new UploadState([["a.bin", new Uint8Array(450).fill(0x78)]], [61000]);
    assert.equal(new TextDecoder().decode(u.syncJob),
      '{"files":[{"crc":783327941,"fileId":2013000210,"path":"a.bin","size":450},{"operation":"remove","plugId":61000}]}');
    u.acceptRequest(concat(u16le(0x5589), u32le(2), u16le(0)), 0x5589);
    assert.equal(toHex(u.nextFrame(0x4242, 0.0)!),
      "f000207601530042420200000000280001781c1d4c41620e02301046613b477c6b464c5b041d381b780643226200624d1036055958743d6e"
      + "56576f4b4b18597d221b781e31246a015c5d2c355d3523516a5f6e4f3021266d09486852086e5b1f417027075f211f30790f001b331575"
      + "15112224377b3026561064150e2922062e6f693769755d03143e7c01393b210058f7");
    assert.equal(u.nextFrame(0x4242, 0.005), null);
    assert.equal(u.activeFileId, null);
    assert.equal(u.sentPartCount, 1);
    assert.equal(u.sentAllFiles, false);
  });

  it("UploadState paces by parts in flight and refuses bad files", () => {
    const u = new UploadState([["big.bin", new Uint8Array(200 * 70).fill(1)]]);
    u.acceptRequest(concat(u16le(7), u32le(fileId("big.bin")), u16le(0)), 7);
    let now = 0;
    for (let i = 0; i < 64; i++, now += 1) assert.notEqual(u.nextFrame(1, now), null);
    assert.equal(u.inFlightParts, 64);
    assert.equal(u.nextFrame(1, now + 10), null); // window full
    u.observeCompletedParts(30);
    assert.notEqual(u.nextFrame(1, now), null); // 35 in flight now: 50 ms pacing
    assert.equal(u.nextFrame(1, now + 0.03), null);
    assert.notEqual(u.nextFrame(1, now + 0.051), null);
    assert.throws(() => new UploadState([["e", new Uint8Array(0)]]), ProtocolError);
    assert.throws(() => new UploadState([["x", Uint8Array.of(1)], ["X", Uint8Array.of(2)]]), /collision/);
    assert.throws(() => u.acceptRequest(new Uint8Array(7), 7), ProtocolError);
    assert.throws(() => u.acceptRequest(concat(u16le(8), u32le(1), u16le(0)), 7), /unexpected file client/);
  });

  it("DownloadState reassembles parts and asks for the next file", () => {
    const d = new DownloadState([[1, "settings/plugs.json"], [0, "settings/slotConfiguration.json"]]);
    const first = decodeFrame(d.begin(0x4242)!);
    assert.equal(first.messageId, 0x35);
    assert.deepEqual(first.payload, concat(u16le(0x4242), u32le(1), u16le(0)));
    const part = (id: number, n: number, last: boolean, text: string) =>
      concat(u16le(99), u32le(id), u16le(n), Uint8Array.of(last ? 1 : 0), compress(utf8(text)));
    const [none, next] = d.acceptPart(part(1, 0, false, "ab"), 99, 0x4242);
    assert.equal(none, null);
    assert.equal(readU16le(decodeFrame(next!).payload, 6), 1);
    assert.throws(() => d.acceptPart(part(1, 0, false, "ab"), 99, 0x4242), /out-of-order/);
    const [done, following] = d.acceptPart(part(1, 1, true, "cd"), 99, 0x4242);
    assert.deepEqual(done, ["settings/plugs.json", utf8("abcd")]);
    assert.deepEqual(decodeFrame(following!).payload, concat(u16le(0x4242), u32le(0), u16le(0)));
    assert.throws(() => d.acceptPart(part(0, 0, true, "x".repeat(201)), 99, 0x4242), /exceeds 200 bytes/);
  });
});
