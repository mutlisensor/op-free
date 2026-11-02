/**
 * zlib streams, written to match Python's `zlib` module (zlib 1.2.12) byte for byte.
 *
 * Why not CompressionStream: browsers and Node ship Chromium's zlib fork, whose deflate output differs from vanilla
 * zlib for most 200-byte parts (measured: 118 of 151), and DecompressionStream rejects trailing bytes that Python
 * ignores. The OP-Z only checks the CRC of the inflated data, so any valid stream would install, but the shared test
 * vectors require identical frames. So:
 *
 * - `compress` is vanilla zlib's level-6 `deflate_slow` and `trees.c` for one block (inputs up to 16 382 bytes; the
 *   file service never compresses more than 200 bytes at a time). Checked against `test-vectors/deflate.json`.
 * - `decompress` is a small RFC 1950/1951 inflater with Python's semantics: header and Adler-32 checked, trailing
 *   bytes ignored; `maxSize` behaves like `sysex.decompress(max_size=...)` (decompressobj: a truncated stream is
 *   returned as far as it goes, more than maxSize bytes is an error).
 */

export class ZlibError extends Error {
  override name = "ZlibError";
}

/** decompress() produced more than maxSize bytes. */
export class OutputLimitError extends ZlibError {
  override name = "OutputLimitError";
}

// ---------------------------------------------------------------------------------------------------------------
// Checksums
// ---------------------------------------------------------------------------------------------------------------

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

/** `zlib.crc32(data) & 0xFFFFFFFF`. */
export function crc32(data: Uint8Array, crc = 0): number {
  let c = ~crc >>> 0;
  for (let i = 0; i < data.length; i++) c = CRC_TABLE[(c ^ data[i]) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}

export function adler32(data: Uint8Array): number {
  let a = 1;
  let b = 0;
  for (let i = 0; i < data.length; ) {
    const end = Math.min(i + 5552, data.length);
    for (; i < end; i++) {
      a += data[i];
      b += a;
    }
    a %= 65521;
    b %= 65521;
  }
  return ((b << 16) | a) >>> 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Shared tables (trees.c tr_static_init)
// ---------------------------------------------------------------------------------------------------------------

const LENGTH_CODES = 29;
const LITERALS = 256;
const L_CODES = LITERALS + 1 + LENGTH_CODES;
const D_CODES = 30;
const BL_CODES = 19;
const HEAP_SIZE = 2 * L_CODES + 1;
const MAX_BITS = 15;
const MAX_BL_BITS = 7;
const END_BLOCK = 256;
const REP_3_6 = 16;
const REPZ_3_10 = 17;
const REPZ_11_138 = 18;

const EXTRA_LBITS = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const EXTRA_DBITS = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
const EXTRA_BLBITS = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7];
const BL_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

const LENGTH_CODE = new Uint8Array(256);
const DIST_CODE = new Uint8Array(512);
const BASE_LENGTH = new Int32Array(LENGTH_CODES);
const BASE_DIST = new Int32Array(D_CODES);

class Tree {
  freq: Int32Array;
  code: Int32Array;
  dad: Int32Array;
  len: Int32Array;
  constructor(size: number) {
    this.freq = new Int32Array(size);
    this.code = new Int32Array(size);
    this.dad = new Int32Array(size);
    this.len = new Int32Array(size);
  }
}

const STATIC_LTREE = new Tree(L_CODES + 2);
const STATIC_DTREE = new Tree(D_CODES);

function biReverse(code: number, len: number): number {
  let res = 0;
  do {
    res |= code & 1;
    code >>>= 1;
    res <<= 1;
  } while (--len > 0);
  return res >>> 1;
}

function genCodes(tree: Tree, maxCode: number, blCount: Int32Array): void {
  const nextCode = new Int32Array(MAX_BITS + 1);
  let code = 0;
  for (let bits = 1; bits <= MAX_BITS; bits++) {
    code = (code + blCount[bits - 1]) << 1;
    nextCode[bits] = code & 0xffff;
  }
  for (let n = 0; n <= maxCode; n++) {
    const len = tree.len[n];
    if (len === 0) continue;
    tree.code[n] = biReverse(nextCode[len]++, len);
  }
}

(function trStaticInit() {
  let length = 0;
  let code: number;
  for (code = 0; code < LENGTH_CODES - 1; code++) {
    BASE_LENGTH[code] = length;
    for (let n = 0; n < 1 << EXTRA_LBITS[code]; n++) LENGTH_CODE[length++] = code;
  }
  LENGTH_CODE[length - 1] = code;
  let dist = 0;
  for (code = 0; code < 16; code++) {
    BASE_DIST[code] = dist;
    for (let n = 0; n < 1 << EXTRA_DBITS[code]; n++) DIST_CODE[dist++] = code;
  }
  dist >>= 7;
  for (; code < D_CODES; code++) {
    BASE_DIST[code] = dist << 7;
    for (let n = 0; n < 1 << (EXTRA_DBITS[code] - 7); n++) DIST_CODE[256 + dist++] = code;
  }
  const blCount = new Int32Array(MAX_BITS + 1);
  let n = 0;
  while (n <= 143) (STATIC_LTREE.len[n++] = 8), blCount[8]++;
  while (n <= 255) (STATIC_LTREE.len[n++] = 9), blCount[9]++;
  while (n <= 279) (STATIC_LTREE.len[n++] = 7), blCount[7]++;
  while (n <= 287) (STATIC_LTREE.len[n++] = 8), blCount[8]++;
  genCodes(STATIC_LTREE, L_CODES + 1, blCount);
  for (n = 0; n < D_CODES; n++) {
    STATIC_DTREE.len[n] = 5;
    STATIC_DTREE.code[n] = biReverse(n, 5);
  }
})();

function dCode(dist: number): number {
  return dist < 256 ? DIST_CODE[dist] : DIST_CODE[256 + (dist >>> 7)];
}

// ---------------------------------------------------------------------------------------------------------------
// Deflate (deflate.c deflate_slow + trees.c, level 6, one block)
// ---------------------------------------------------------------------------------------------------------------

/** The largest input `compress` reproduces exactly: zlib flushes its first block after 16 383 symbols. */
export const MAX_COMPRESS_SIZE = 16382;

const MIN_MATCH = 3;
const MAX_MATCH = 258;
const W_SIZE = 32768;
const W_MASK = W_SIZE - 1;
const HASH_SIZE = 32768;
const HASH_MASK = HASH_SIZE - 1;
const HASH_SHIFT = 5;
const MIN_LOOKAHEAD = MAX_MATCH + MIN_MATCH + 1;
const MAX_DIST = W_SIZE - MIN_LOOKAHEAD;
const TOO_FAR = 4096;
// configuration_table[6]
const GOOD_LENGTH = 8;
const MAX_LAZY = 16;
const NICE_LENGTH = 128;
const MAX_CHAIN = 128;

interface TreeDesc {
  tree: Tree;
  stree: Tree | null;
  extra: number[];
  extraBase: number;
  elems: number;
  maxLength: number;
  maxCode: number;
}

class BitWriter {
  out: number[] = [];
  private buf = 0;
  private valid = 0;
  bits(value: number, length: number): void {
    this.buf |= value << this.valid;
    this.valid += length;
    while (this.valid >= 8) {
      this.out.push(this.buf & 0xff);
      this.buf >>>= 8;
      this.valid -= 8;
    }
  }
  windup(): void {
    if (this.valid > 0) this.out.push(this.buf & 0xff);
    this.buf = 0;
    this.valid = 0;
  }
  byte(value: number): void {
    this.out.push(value & 0xff);
  }
}

class Deflater {
  readonly window: Uint8Array;
  readonly head = new Uint16Array(HASH_SIZE);
  readonly prev = new Uint16Array(W_SIZE);
  insH = 0;
  strstart = 0;
  lookahead: number;
  matchStart = 0;
  matchLength = MIN_MATCH - 1;
  prevLength = MIN_MATCH - 1;
  prevMatch = 0;
  matchAvailable = false;
  syms: number[] = []; // (dist, lc) pairs
  readonly dynLtree = new Tree(HEAP_SIZE);
  readonly dynDtree = new Tree(2 * D_CODES + 1);
  readonly blTree = new Tree(2 * BL_CODES + 1);
  readonly heap = new Int32Array(2 * L_CODES + 1);
  heapLen = 0;
  heapMax = 0;
  readonly depth = new Int32Array(2 * L_CODES + 1);
  readonly blCount = new Int32Array(MAX_BITS + 1);
  optLen = 0;
  staticLen = 0;
  readonly w = new BitWriter();
  readonly input: Uint8Array;

  constructor(input: Uint8Array) {
    this.input = input;
    // fill_window zeroes WIN_INIT (= MAX_MATCH) bytes past the data; longest_match may read them.
    this.window = new Uint8Array(input.length + MAX_MATCH + MIN_MATCH);
    this.window.set(input);
    this.lookahead = input.length;
    this.dynLtree.freq[END_BLOCK] = 1;
    if (this.lookahead >= MIN_MATCH) {
      this.insH = this.window[0];
      this.insH = ((this.insH << HASH_SHIFT) ^ this.window[1]) & HASH_MASK;
    }
  }

  private insertString(str: number): number {
    this.insH = ((this.insH << HASH_SHIFT) ^ this.window[str + MIN_MATCH - 1]) & HASH_MASK;
    const matchHead = (this.prev[str & W_MASK] = this.head[this.insH]);
    this.head[this.insH] = str;
    return matchHead;
  }

  private longestMatch(curMatch: number): number {
    let chainLength = MAX_CHAIN;
    const win = this.window;
    const scan0 = this.strstart;
    let bestLen = this.prevLength;
    let niceMatch = NICE_LENGTH;
    const limit = this.strstart > MAX_DIST ? this.strstart - MAX_DIST : 0;
    const strend = this.strstart + MAX_MATCH;
    let scanEnd1 = win[scan0 + bestLen - 1];
    let scanEnd = win[scan0 + bestLen];
    if (this.prevLength >= GOOD_LENGTH) chainLength >>>= 2;
    if (niceMatch > this.lookahead) niceMatch = this.lookahead;
    do {
      const match = curMatch;
      if (
        win[match + bestLen] !== scanEnd ||
        win[match + bestLen - 1] !== scanEnd1 ||
        win[match] !== win[scan0] ||
        win[match + 1] !== win[scan0 + 1]
      ) {
        continue;
      }
      // scan[2] and match[2] are equal when the hash keys are; the unrolled C loop compares from index 3 on.
      let scan = scan0 + 2;
      let m = match + 2;
      do {
        scan++;
        m++;
      } while (win[scan] === win[m] && scan < strend);
      if (scan > strend) scan = strend;
      const len = MAX_MATCH - (strend - scan);
      if (len > bestLen) {
        this.matchStart = curMatch;
        bestLen = len;
        if (len >= niceMatch) break;
        scanEnd1 = win[scan0 + bestLen - 1];
        scanEnd = win[scan0 + bestLen];
      }
    } while ((curMatch = this.prev[curMatch & W_MASK]) > limit && --chainLength !== 0);
    return bestLen <= this.lookahead ? bestLen : this.lookahead;
  }

  private tallyLit(c: number): void {
    this.syms.push(0, c);
    this.dynLtree.freq[c]++;
  }

  private tallyDist(distance: number, length: number): void {
    this.syms.push(distance, length);
    this.dynLtree.freq[LENGTH_CODE[length] + LITERALS + 1]++;
    this.dynDtree.freq[dCode(distance - 1)]++;
  }

  run(): Uint8Array {
    const w = this.w;
    w.byte(0x78);
    w.byte(0x9c);
    while (this.lookahead !== 0) {
      let hashHead = 0;
      if (this.lookahead >= MIN_MATCH) hashHead = this.insertString(this.strstart);
      this.prevLength = this.matchLength;
      this.prevMatch = this.matchStart;
      this.matchLength = MIN_MATCH - 1;
      if (hashHead !== 0 && this.prevLength < MAX_LAZY && this.strstart - hashHead <= MAX_DIST) {
        this.matchLength = this.longestMatch(hashHead);
        if (this.matchLength === MIN_MATCH && this.strstart - this.matchStart > TOO_FAR) {
          this.matchLength = MIN_MATCH - 1;
        }
      }
      if (this.prevLength >= MIN_MATCH && this.matchLength <= this.prevLength) {
        const maxInsert = this.strstart + this.lookahead - MIN_MATCH;
        this.tallyDist(this.strstart - 1 - this.prevMatch, this.prevLength - MIN_MATCH);
        this.lookahead -= this.prevLength - 1;
        this.prevLength -= 2;
        do {
          if (++this.strstart <= maxInsert) this.insertString(this.strstart);
        } while (--this.prevLength !== 0);
        this.matchAvailable = false;
        this.matchLength = MIN_MATCH - 1;
        this.strstart++;
      } else if (this.matchAvailable) {
        this.tallyLit(this.window[this.strstart - 1]);
        this.strstart++;
        this.lookahead--;
      } else {
        this.matchAvailable = true;
        this.strstart++;
        this.lookahead--;
      }
    }
    if (this.matchAvailable) {
      this.tallyLit(this.window[this.strstart - 1]);
      this.matchAvailable = false;
    }
    this.flushBlock(this.input, true);
    const adler = adler32(this.input);
    w.byte(adler >>> 24);
    w.byte(adler >>> 16);
    w.byte(adler >>> 8);
    w.byte(adler);
    return Uint8Array.from(w.out);
  }

  // -- trees.c --------------------------------------------------------------------------------------------------

  private smaller(tree: Tree, n: number, m: number): boolean {
    return tree.freq[n] < tree.freq[m] || (tree.freq[n] === tree.freq[m] && this.depth[n] <= this.depth[m]);
  }

  private pqdownheap(tree: Tree, k: number): void {
    const heap = this.heap;
    const v = heap[k];
    let j = k << 1;
    while (j <= this.heapLen) {
      if (j < this.heapLen && this.smaller(tree, heap[j + 1], heap[j])) j++;
      if (this.smaller(tree, v, heap[j])) break;
      heap[k] = heap[j];
      k = j;
      j <<= 1;
    }
    heap[k] = v;
  }

  private genBitlen(desc: TreeDesc): void {
    const { tree, stree, extra, extraBase: base, maxLength, maxCode } = desc;
    const heap = this.heap;
    let overflow = 0;
    for (let bits = 0; bits <= MAX_BITS; bits++) this.blCount[bits] = 0;
    tree.len[heap[this.heapMax]] = 0;
    let h: number;
    for (h = this.heapMax + 1; h < HEAP_SIZE; h++) {
      const n = heap[h];
      let bits = tree.len[tree.dad[n]] + 1;
      if (bits > maxLength) (bits = maxLength), overflow++;
      tree.len[n] = bits;
      if (n > maxCode) continue;
      this.blCount[bits]++;
      let xbits = 0;
      if (n >= base) xbits = extra[n - base];
      const f = tree.freq[n];
      this.optLen += f * (bits + xbits);
      if (stree) this.staticLen += f * (stree.len[n] + xbits);
    }
    if (overflow === 0) return;
    do {
      let bits = maxLength - 1;
      while (this.blCount[bits] === 0) bits--;
      this.blCount[bits]--;
      this.blCount[bits + 1] += 2;
      this.blCount[maxLength]--;
      overflow -= 2;
    } while (overflow > 0);
    for (let bits = maxLength; bits !== 0; bits--) {
      let n = this.blCount[bits];
      while (n !== 0) {
        const m = heap[--h];
        if (m > maxCode) continue;
        if (tree.len[m] !== bits) {
          this.optLen += (bits - tree.len[m]) * tree.freq[m];
          tree.len[m] = bits;
        }
        n--;
      }
    }
  }

  private buildTree(desc: TreeDesc): void {
    const { tree, stree, elems } = desc;
    const heap = this.heap;
    let maxCode = -1;
    this.heapLen = 0;
    this.heapMax = HEAP_SIZE;
    for (let n = 0; n < elems; n++) {
      if (tree.freq[n] !== 0) {
        heap[++this.heapLen] = maxCode = n;
        this.depth[n] = 0;
      } else {
        tree.len[n] = 0;
      }
    }
    while (this.heapLen < 2) {
      const node = (heap[++this.heapLen] = maxCode < 2 ? ++maxCode : 0);
      tree.freq[node] = 1;
      this.depth[node] = 0;
      this.optLen--;
      if (stree) this.staticLen -= stree.len[node];
    }
    desc.maxCode = maxCode;
    for (let n = this.heapLen >> 1; n >= 1; n--) this.pqdownheap(tree, n);
    let node = elems;
    do {
      const n = heap[1];
      heap[1] = heap[this.heapLen--];
      this.pqdownheap(tree, 1);
      const m = heap[1];
      heap[--this.heapMax] = n;
      heap[--this.heapMax] = m;
      tree.freq[node] = tree.freq[n] + tree.freq[m];
      this.depth[node] = (this.depth[n] >= this.depth[m] ? this.depth[n] : this.depth[m]) + 1;
      tree.dad[n] = tree.dad[m] = node;
      heap[1] = node++;
      this.pqdownheap(tree, 1);
    } while (this.heapLen >= 2);
    heap[--this.heapMax] = heap[1];
    this.genBitlen(desc);
    genCodes(tree, maxCode, this.blCount);
  }

  private scanTree(tree: Tree, maxCode: number): void {
    let prevlen = -1;
    let nextlen = tree.len[0];
    let count = 0;
    let maxCount = 7;
    let minCount = 4;
    if (nextlen === 0) (maxCount = 138), (minCount = 3);
    tree.len[maxCode + 1] = 0xffff;
    for (let n = 0; n <= maxCode; n++) {
      const curlen = nextlen;
      nextlen = tree.len[n + 1];
      if (++count < maxCount && curlen === nextlen) continue;
      else if (count < minCount) this.blTree.freq[curlen] += count;
      else if (curlen !== 0) {
        if (curlen !== prevlen) this.blTree.freq[curlen]++;
        this.blTree.freq[REP_3_6]++;
      } else if (count <= 10) this.blTree.freq[REPZ_3_10]++;
      else this.blTree.freq[REPZ_11_138]++;
      count = 0;
      prevlen = curlen;
      if (nextlen === 0) (maxCount = 138), (minCount = 3);
      else if (curlen === nextlen) (maxCount = 6), (minCount = 3);
      else (maxCount = 7), (minCount = 4);
    }
  }

  private sendCode(c: number, tree: Tree): void {
    this.w.bits(tree.code[c], tree.len[c]);
  }

  private sendTree(tree: Tree, maxCode: number): void {
    let prevlen = -1;
    let nextlen = tree.len[0];
    let count = 0;
    let maxCount = 7;
    let minCount = 4;
    if (nextlen === 0) (maxCount = 138), (minCount = 3);
    for (let n = 0; n <= maxCode; n++) {
      const curlen = nextlen;
      nextlen = tree.len[n + 1];
      if (++count < maxCount && curlen === nextlen) continue;
      else if (count < minCount) {
        do this.sendCode(curlen, this.blTree);
        while (--count !== 0);
      } else if (curlen !== 0) {
        if (curlen !== prevlen) {
          this.sendCode(curlen, this.blTree);
          count--;
        }
        this.sendCode(REP_3_6, this.blTree);
        this.w.bits(count - 3, 2);
      } else if (count <= 10) {
        this.sendCode(REPZ_3_10, this.blTree);
        this.w.bits(count - 3, 3);
      } else {
        this.sendCode(REPZ_11_138, this.blTree);
        this.w.bits(count - 11, 7);
      }
      count = 0;
      prevlen = curlen;
      if (nextlen === 0) (maxCount = 138), (minCount = 3);
      else if (curlen === nextlen) (maxCount = 6), (minCount = 3);
      else (maxCount = 7), (minCount = 4);
    }
  }

  private compressBlock(ltree: Tree, dtree: Tree): void {
    for (let i = 0; i < this.syms.length; i += 2) {
      let dist = this.syms[i];
      let lc = this.syms[i + 1];
      if (dist === 0) {
        this.sendCode(lc, ltree);
        continue;
      }
      let code = LENGTH_CODE[lc];
      this.sendCode(code + LITERALS + 1, ltree);
      let extra = EXTRA_LBITS[code];
      if (extra !== 0) {
        lc -= BASE_LENGTH[code];
        this.w.bits(lc, extra);
      }
      dist--;
      code = dCode(dist);
      this.sendCode(code, dtree);
      extra = EXTRA_DBITS[code];
      if (extra !== 0) {
        dist -= BASE_DIST[code];
        this.w.bits(dist, extra);
      }
    }
    this.sendCode(END_BLOCK, ltree);
  }

  private flushBlock(buf: Uint8Array, last: boolean): void {
    const lDesc: TreeDesc = { tree: this.dynLtree, stree: STATIC_LTREE, extra: EXTRA_LBITS, extraBase: LITERALS + 1,
      elems: L_CODES, maxLength: MAX_BITS, maxCode: 0 };
    const dDesc: TreeDesc = { tree: this.dynDtree, stree: STATIC_DTREE, extra: EXTRA_DBITS, extraBase: 0,
      elems: D_CODES, maxLength: MAX_BITS, maxCode: 0 };
    const blDesc: TreeDesc = { tree: this.blTree, stree: null, extra: EXTRA_BLBITS, extraBase: 0,
      elems: BL_CODES, maxLength: MAX_BL_BITS, maxCode: 0 };
    this.buildTree(lDesc);
    this.buildTree(dDesc);
    // build_bl_tree
    this.scanTree(this.dynLtree, lDesc.maxCode);
    this.scanTree(this.dynDtree, dDesc.maxCode);
    this.buildTree(blDesc);
    let maxBlindex: number;
    for (maxBlindex = BL_CODES - 1; maxBlindex >= 3; maxBlindex--) {
      if (this.blTree.len[BL_ORDER[maxBlindex]] !== 0) break;
    }
    this.optLen += 3 * (maxBlindex + 1) + 5 + 5 + 4;

    let optLenb = (this.optLen + 3 + 7) >>> 3;
    const staticLenb = (this.staticLen + 3 + 7) >>> 3;
    if (staticLenb <= optLenb) optLenb = staticLenb;
    const lastBit = last ? 1 : 0;
    const storedLen = buf.length;
    if (storedLen + 4 <= optLenb) {
      this.w.bits((0 << 1) + lastBit, 3);
      this.w.windup();
      this.w.byte(storedLen);
      this.w.byte(storedLen >>> 8);
      this.w.byte(~storedLen);
      this.w.byte(~storedLen >>> 8);
      for (const b of buf) this.w.byte(b);
    } else if (staticLenb === optLenb) {
      this.w.bits((1 << 1) + lastBit, 3);
      this.compressBlock(STATIC_LTREE, STATIC_DTREE);
    } else {
      this.w.bits((2 << 1) + lastBit, 3);
      const lcodes = lDesc.maxCode + 1;
      const dcodes = dDesc.maxCode + 1;
      const blcodes = maxBlindex + 1;
      this.w.bits(lcodes - 257, 5);
      this.w.bits(dcodes - 1, 5);
      this.w.bits(blcodes - 4, 4);
      for (let rank = 0; rank < blcodes; rank++) this.w.bits(this.blTree.len[BL_ORDER[rank]], 3);
      this.sendTree(this.dynLtree, lcodes - 1);
      this.sendTree(this.dynDtree, dcodes - 1);
      this.compressBlock(this.dynLtree, this.dynDtree);
    }
    if (last) this.w.windup();
  }
}

/** `zlib.compress(data)` (level 6), byte for byte, for inputs up to MAX_COMPRESS_SIZE bytes. */
export function compress(data: Uint8Array): Uint8Array {
  if (data.length > MAX_COMPRESS_SIZE) {
    throw new RangeError(`compress() reproduces zlib only up to ${MAX_COMPRESS_SIZE} bytes, got ${data.length}`);
  }
  return new Deflater(data).run();
}

// ---------------------------------------------------------------------------------------------------------------
// Inflate
// ---------------------------------------------------------------------------------------------------------------

const LEN_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163,
  195, 227, 258];
const LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049,
  3073, 4097, 6145, 8193, 12289, 16385, 24577];
const DIST_EXTRA = EXTRA_DBITS;

class Truncated extends Error {}

interface Huffman {
  counts: Int32Array;
  symbols: Int32Array;
}

function huffman(lengths: ArrayLike<number>, n: number): Huffman {
  const counts = new Int32Array(MAX_BITS + 1);
  for (let i = 0; i < n; i++) counts[lengths[i]]++;
  let left = 1;
  for (let len = 1; len <= MAX_BITS; len++) {
    left = (left << 1) - counts[len];
    if (left < 0) throw new ZlibError("invalid code lengths set");
  }
  const offs = new Int32Array(MAX_BITS + 1);
  for (let len = 1; len < MAX_BITS; len++) offs[len + 1] = offs[len] + counts[len];
  const symbols = new Int32Array(n);
  for (let i = 0; i < n; i++) if (lengths[i] !== 0) symbols[offs[lengths[i]]++] = i;
  return { counts, symbols };
}

const FIXED_LIT = huffman(Array.from({ length: 288 }, (_, i) => (i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8)), 288);
const FIXED_DIST = huffman(new Array(30).fill(5), 30);

class Inflater {
  private pos = 0;
  private bitBuf = 0;
  private bitCnt = 0;
  out: number[] = [];
  private readonly data: Uint8Array;
  private readonly maxSize: number;
  constructor(data: Uint8Array, maxSize: number) {
    this.data = data;
    this.maxSize = maxSize;
  }

  private bits(need: number): number {
    let val = this.bitBuf;
    while (this.bitCnt < need) {
      if (this.pos >= this.data.length) throw new Truncated();
      val |= this.data[this.pos++] << this.bitCnt;
      this.bitCnt += 8;
    }
    this.bitBuf = val >>> need;
    this.bitCnt -= need;
    return val & ((1 << need) - 1);
  }

  private emit(byte: number): void {
    if (this.out.length >= this.maxSize) throw new OutputLimitError(`inflated payload exceeds ${this.maxSize} bytes`);
    this.out.push(byte);
  }

  private decode(h: Huffman): number {
    let code = 0;
    let first = 0;
    let index = 0;
    for (let len = 1; len <= MAX_BITS; len++) {
      code |= this.bits(1);
      const count = h.counts[len];
      if (code - count < first) return h.symbols[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    throw new ZlibError("invalid code");
  }

  private stored(): void {
    this.bitBuf = 0;
    this.bitCnt = 0;
    if (this.pos + 4 > this.data.length) throw new Truncated();
    const d = this.data;
    const len = d[this.pos] | (d[this.pos + 1] << 8);
    const nlen = d[this.pos + 2] | (d[this.pos + 3] << 8);
    this.pos += 4;
    if (len !== (~nlen & 0xffff)) throw new ZlibError("invalid stored block lengths");
    for (let i = 0; i < len; i++) {
      if (this.pos >= d.length) throw new Truncated();
      this.emit(d[this.pos++]);
    }
  }

  private codes(lit: Huffman, dist: Huffman): void {
    for (;;) {
      let sym = this.decode(lit);
      if (sym < 256) this.emit(sym);
      else if (sym === 256) return;
      else {
        sym -= 257;
        if (sym >= 29) throw new ZlibError("invalid literal/length code");
        const len = LEN_BASE[sym] + this.bits(LEN_EXTRA[sym]);
        const dsym = this.decode(dist);
        if (dsym >= 30) throw new ZlibError("invalid distance code");
        const d = DIST_BASE[dsym] + this.bits(DIST_EXTRA[dsym]);
        if (d > this.out.length) throw new ZlibError("invalid distance too far back");
        for (let i = 0; i < len; i++) this.emit(this.out[this.out.length - d]);
      }
    }
  }

  private dynamic(): void {
    const nlen = this.bits(5) + 257;
    const ndist = this.bits(5) + 1;
    const ncode = this.bits(4) + 4;
    if (nlen > 286 || ndist > 30) throw new ZlibError("too many length or distance symbols");
    const lengths = new Int32Array(320);
    for (let i = 0; i < ncode; i++) lengths[BL_ORDER[i]] = this.bits(3);
    const lencode = huffman(lengths, 19);
    let index = 0;
    while (index < nlen + ndist) {
      let sym = this.decode(lencode);
      if (sym < 16) lengths[index++] = sym;
      else {
        let len = 0;
        if (sym === 16) {
          if (index === 0) throw new ZlibError("invalid bit length repeat");
          len = lengths[index - 1];
          sym = 3 + this.bits(2);
        } else if (sym === 17) sym = 3 + this.bits(3);
        else sym = 11 + this.bits(7);
        if (index + sym > nlen + ndist) throw new ZlibError("invalid bit length repeat");
        while (sym--) lengths[index++] = len;
      }
    }
    if (lengths[256] === 0) throw new ZlibError("invalid code -- missing end-of-block");
    this.codes(huffman(lengths, nlen), huffman(lengths.subarray(nlen), ndist));
  }

  /** Returns true when the stream ended with a final block, false when the input ran out first. */
  run(): boolean {
    const d = this.data;
    if (d.length < 2) throw new Truncated();
    const cmf = d[0];
    const flg = d[1];
    if ((cmf * 256 + flg) % 31 !== 0) throw new ZlibError("incorrect header check");
    if ((cmf & 0x0f) !== 8) throw new ZlibError("unknown compression method");
    if (cmf >> 4 > 7) throw new ZlibError("invalid window size");
    if (flg & 0x20) throw new ZlibError("need dictionary");
    this.pos = 2;
    let last = 0;
    do {
      last = this.bits(1);
      const type = this.bits(2);
      if (type === 0) this.stored();
      else if (type === 1) this.codes(FIXED_LIT, FIXED_DIST);
      else if (type === 2) this.dynamic();
      else throw new ZlibError("invalid block type");
    } while (!last);
    this.bitBuf = 0;
    this.bitCnt = 0;
    if (this.pos + 4 > d.length) throw new Truncated();
    const want = ((d[this.pos] << 24) | (d[this.pos + 1] << 16) | (d[this.pos + 2] << 8) | d[this.pos + 3]) >>> 0;
    if (want !== adler32(Uint8Array.from(this.out))) throw new ZlibError("incorrect data check");
    return true;
  }
}

/**
 * Without maxSize: `zlib.decompress(data)` (a truncated stream is an error). With maxSize: `sysex.decompress(data,
 * max_size=...)`, which uses a decompressobj: a truncated stream yields what it holds, more output is an error.
 * Bytes after the end of the stream are ignored either way, as in Python.
 */
export function decompress(data: Uint8Array, maxSize?: number): Uint8Array {
  const inflater = new Inflater(data, maxSize ?? Number.MAX_SAFE_INTEGER);
  try {
    inflater.run();
  } catch (e) {
    if (!(e instanceof Truncated)) throw e;
    if (maxSize === undefined) throw new ZlibError("incomplete or truncated stream");
  }
  return Uint8Array.from(inflater.out);
}
