/** Byte helpers: the `int.to_bytes` / `int.from_bytes` / `bytes.hex` idioms of the Python side. */

export const EMPTY = new Uint8Array(0);

export function concat(...parts: Uint8Array[]): Uint8Array {
  let size = 0;
  for (const p of parts) size += p.length;
  const out = new Uint8Array(size);
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

function checkUint(value: number, bytes: number): void {
  if (!Number.isInteger(value) || value < 0 || value >= 2 ** (8 * bytes)) {
    throw new RangeError(`${value} does not fit in ${bytes} unsigned byte(s)`);
  }
}

export function u16le(value: number): Uint8Array {
  checkUint(value, 2);
  return Uint8Array.of(value & 0xff, value >>> 8);
}

export function u16be(value: number): Uint8Array {
  checkUint(value, 2);
  return Uint8Array.of(value >>> 8, value & 0xff);
}

export function u32le(value: number): Uint8Array {
  checkUint(value, 4);
  return Uint8Array.of(value & 0xff, (value >>> 8) & 0xff, (value >>> 16) & 0xff, value >>> 24);
}

export function readU16le(data: Uint8Array, offset: number): number {
  return data[offset] | (data[offset + 1] << 8);
}

export function readU32le(data: Uint8Array, offset: number): number {
  return (data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) | (data[offset + 3] << 24)) >>> 0;
}

export function toHex(data: Uint8Array): string {
  let s = "";
  for (const b of data) s += b.toString(16).padStart(2, "0");
  return s;
}

export function fromHex(text: string): Uint8Array {
  if (text.length % 2 !== 0 || /[^0-9a-fA-F]/.test(text)) throw new TypeError("invalid hex string");
  const out = new Uint8Array(text.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(text.slice(2 * i, 2 * i + 2), 16);
  return out;
}

export function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true });

/** `str.encode()` */
export function utf8(text: string): Uint8Array {
  return encoder.encode(text);
}

/** `bytes.decode()` (strict UTF-8, a leading BOM is dropped as json.loads does). */
export function fromUtf8(data: Uint8Array): string {
  return decoder.decode(data);
}
