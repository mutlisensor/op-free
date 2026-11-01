/**
 * Python's `json.dumps`, `json.loads`, `repr` and `==` for the JSON values the OP-Z uses, so that files sent to the
 * device and messages shown to the user come out exactly as the Python tools produce them.
 *
 * Known gaps (none occur in OP-Z settings files): a JSON number with a zero fraction (`1.0`) parses to the JS number
 * 1 and is written back as `1`; integers beyond 2^53 lose precision; integer-like object keys ("0") are reordered
 * first by JS objects; `NaN` / `Infinity` literals are rejected by `loads`.
 */

import { fromUtf8 } from "./bytes.ts";

export type Json = null | boolean | number | string | Json[] | { [key: string]: Json };

export interface DumpOptions {
  sortKeys?: boolean;
  /** [item separator, key separator]; Python's default is [", ", ": "], or [",", ": "] with an indent. */
  separators?: [string, string];
  indent?: number | null;
  ensureAscii?: boolean;
}

const ESCAPES: Record<string, string> = {
  "\\": "\\\\", '"': '\\"', "\b": "\\b", "\f": "\\f", "\n": "\\n", "\r": "\\r", "\t": "\\t",
};

function u4(code: number): string {
  return "\\u" + code.toString(16).padStart(4, "0");
}

function encodeString(s: string, ensureAscii: boolean): string {
  let out = '"';
  for (let i = 0; i < s.length; i++) {
    const ch = s[i];
    const code = s.charCodeAt(i);
    if (ESCAPES[ch] !== undefined) out += ESCAPES[ch];
    else if (code < 0x20) out += u4(code);
    else if (ensureAscii && code > 0x7e) out += u4(code); // UTF-16 code units: astral characters become pairs
    else out += ch;
  }
  return out + '"';
}

/** Python `float.__repr__` (shortest round-trip digits; exponent form below 1e-4 and from 1e16). */
export function floatRepr(x: number): string {
  if (Number.isNaN(x)) return "NaN";
  if (!Number.isFinite(x)) return x > 0 ? "Infinity" : "-Infinity";
  if (x === 0) return Object.is(x, -0) ? "-0.0" : "0.0";
  const [mantissa, expText] = Math.abs(x).toExponential().split("e");
  const digits = mantissa.replace(".", "");
  const exp = Number(expText);
  const sign = x < 0 ? "-" : "";
  if (exp >= -4 && exp < 16) {
    let body: string;
    if (exp < 0) body = "0." + "0".repeat(-exp - 1) + digits;
    else if (digits.length > exp + 1) body = digits.slice(0, exp + 1) + "." + digits.slice(exp + 1);
    else body = digits + "0".repeat(exp + 1 - digits.length) + ".0";
    return sign + body;
  }
  const lead = digits.length > 1 ? digits[0] + "." + digits.slice(1) : digits;
  return sign + lead + "e" + (exp < 0 ? "-" : "+") + String(Math.abs(exp)).padStart(2, "0");
}

/** A JS number as Python would print it: integral values as ints (the JSON side never has floats there). */
export function numberRepr(x: number): string {
  if (Number.isInteger(x)) return BigInt(x).toString();
  return floatRepr(x);
}

function compareCodePoints(a: string, b: string): number {
  const ia = a[Symbol.iterator]();
  const ib = b[Symbol.iterator]();
  for (;;) {
    const ca = ia.next();
    const cb = ib.next();
    if (ca.done || cb.done) return ca.done ? (cb.done ? 0 : -1) : 1;
    const d = ca.value.codePointAt(0)! - cb.value.codePointAt(0)!;
    if (d !== 0) return d;
  }
}

/** `sorted(keys)` for str keys (code-point order). */
export function sortedKeys(obj: object): string[] {
  return Object.keys(obj).sort(compareCodePoints);
}

/** Python `json.dumps(value, sort_keys=..., separators=..., indent=..., ensure_ascii=...)`. */
export function dumps(value: unknown, options: DumpOptions = {}): string {
  const indent = options.indent ?? null;
  const [itemSep, keySep] = options.separators ?? (indent === null ? [", ", ": "] : [",", ": "]);
  const ensureAscii = options.ensureAscii ?? true;
  const sortKeys = options.sortKeys ?? false;

  const encode = (v: unknown, level: number): string => {
    if (v === null || v === undefined) return "null";
    if (v === true) return "true";
    if (v === false) return "false";
    if (typeof v === "number") return numberRepr(v);
    if (typeof v === "string") return encodeString(v, ensureAscii);
    const isArray = Array.isArray(v);
    if (typeof v !== "object") throw new TypeError(`Object of type ${typeof v} is not JSON serializable`);
    const entries: string[] = [];
    if (isArray) {
      for (const item of v as unknown[]) entries.push(encode(item, level + 1));
    } else {
      const keys = sortKeys ? sortedKeys(v as object) : Object.keys(v as object);
      for (const k of keys) entries.push(encodeString(k, ensureAscii) + keySep + encode((v as Record<string, unknown>)[k], level + 1));
    }
    const [open, close] = isArray ? ["[", "]"] : ["{", "}"];
    if (entries.length === 0) return open + close;
    if (indent === null) return open + entries.join(itemSep) + close;
    const inner = "\n" + " ".repeat(indent * (level + 1));
    return open + inner + entries.join(itemSep + inner) + "\n" + " ".repeat(indent * level) + close;
  };
  return encode(value, 0);
}

/** Python `json.loads(bytes or str)` (see the known gaps above). */
export function loads(data: Uint8Array | string): any {
  return JSON.parse(typeof data === "string" ? data : fromUtf8(data));
}

/** A deep copy that keeps key order: `json.loads(json.dumps(doc))`. */
export function clone<T>(doc: T): T {
  return JSON.parse(JSON.stringify(doc));
}

/** Python `==` on JSON values: dicts ignore key order, True == 1. */
export function pyEqual(a: unknown, b: unknown): boolean {
  const scalar = (v: unknown) => typeof v === "number" || typeof v === "boolean";
  if (scalar(a) && scalar(b)) return Number(a) === Number(b);
  if (a === null || b === null || typeof a !== "object" || typeof b !== "object") return a === b;
  if (Array.isArray(a) !== Array.isArray(b)) return false;
  if (Array.isArray(a)) {
    const bb = b as unknown[];
    return a.length === bb.length && a.every((x, i) => pyEqual(x, bb[i]));
  }
  const ka = Object.keys(a);
  const ob = b as Record<string, unknown>;
  if (ka.length !== Object.keys(ob).length) return false;
  return ka.every((k) => Object.prototype.hasOwnProperty.call(ob, k) && pyEqual((a as Record<string, unknown>)[k], ob[k]));
}

/** Python `repr()` of a JSON value, for log messages (`%r`, and `%s` of lists and dicts). */
export function pyRepr(v: unknown): string {
  if (v === null || v === undefined) return "None";
  if (v === true) return "True";
  if (v === false) return "False";
  if (typeof v === "number") return numberRepr(v);
  if (typeof v === "string") {
    const quote = v.includes("'") && !v.includes('"') ? '"' : "'";
    let out = quote;
    for (const ch of v) {
      const code = ch.codePointAt(0)!;
      if (ch === "\\" || ch === quote) out += "\\" + ch;
      else if (ch === "\n") out += "\\n";
      else if (ch === "\r") out += "\\r";
      else if (ch === "\t") out += "\\t";
      else if (code < 0x20 || code === 0x7f) out += "\\x" + code.toString(16).padStart(2, "0");
      else out += ch;
    }
    return out + quote;
  }
  if (Array.isArray(v)) return "[" + v.map(pyRepr).join(", ") + "]";
  if (typeof v === "object") {
    return "{" + Object.entries(v as object).map(([k, x]) => pyRepr(k) + ": " + pyRepr(x)).join(", ") + "}";
  }
  return String(v);
}

/** Python `str()` of a JSON value. */
export function pyStr(v: unknown): string {
  return typeof v === "string" ? v : pyRepr(v);
}

/** Python `int()` of a JSON value: numbers truncate, booleans are 0/1, strings must hold an integer. */
export function pyInt(v: unknown): number {
  if (typeof v === "boolean") return v ? 1 : 0;
  if (typeof v === "number") {
    if (!Number.isFinite(v)) throw new TypeError(`cannot convert ${v} to integer`);
    return Math.trunc(v);
  }
  if (typeof v === "string" && /^\s*[+-]?\d+(_\d+)*\s*$/.test(v)) return Number(v.trim().replaceAll("_", ""));
  throw new TypeError(`invalid literal for int(): ${pyRepr(v)}`);
}
