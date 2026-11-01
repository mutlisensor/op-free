/**
 * Ported from sim/web/src/wasi.ts (keep the two in step).
 * The few WASI preview1 calls opzsim's wasm build imports, over an in-memory file system: stdout and stderr go to a
 * callback, files (scenarios, sample packs, preset files, the rendered WAV) live in a Map keyed by absolute path.
 * Runs the same in the page, in the AudioWorklet and in node's tests. No dependencies.
 */

const ESUCCESS = 0, EBADF = 8, EINVAL = 28, ENOENT = 44;
const FILETYPE_CHAR = 2, FILETYPE_DIR = 3, FILETYPE_FILE = 4;
const O_CREAT = 1, O_TRUNC = 8;
const ROOT_FD = 3;

/** proc_exit (exit() in C): thrown so the caller gets the exit code. */
export class ProcExit extends Error {
  readonly code: number;

  constructor(code: number) {
    super(`exit ${code}`);
    this.code = code;
  }
}

/** Absolute, normalised path: "a/../b" -> "/b". */
export function normalise(path: string): string {
  const out: string[] = [];
  for (const part of path.split("/")) {
    if (!part || part === ".") continue;
    if (part === "..") out.pop();
    else out.push(part);
  }
  return "/" + out.join("/");
}

interface Open { path: string; pos: number }

/** UTF-8 to text; AudioWorkletGlobalScope has no TextDecoder, and opzsim's output is ASCII there. */
const decode: (b: Uint8Array) => string = typeof TextDecoder !== "undefined"
  ? (b) => new TextDecoder().decode(b)
  : (b) => {
    let s = "";
    for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
    return s;
  };

/** The best clock this global scope has (AudioWorkletGlobalScope may lack `performance`). */
const nowMs: () => number = typeof performance !== "undefined" && performance.now ? () => performance.now() : () => Date.now();

export class Wasi {
  readonly files: Map<string, Uint8Array>;
  private memory: WebAssembly.Memory | null = null;
  private fds = new Map<number, Open>();
  private nextFd = ROOT_FD + 1;
  private readonly output: (text: string, fd: number) => void;

  /** files: absolute path -> contents (shared, so the caller sees files the program writes). */
  constructor(files: Map<string, Uint8Array> = new Map(), output: (text: string, fd: number) => void = () => {}) {
    this.files = files;
    this.output = output;
  }

  /** Call after instantiating, before anything runs. */
  bind(memory: WebAssembly.Memory): void {
    this.memory = memory;
  }

  private view(): DataView {
    return new DataView(this.memory!.buffer);
  }

  private bytes(ptr: number, len: number): Uint8Array {
    return new Uint8Array(this.memory!.buffer, ptr, len);
  }

  private iovs(ptr: number, n: number): Array<[number, number]> {
    const v = this.view();
    const out: Array<[number, number]> = [];
    for (let i = 0; i < n; i++) out.push([v.getUint32(ptr + 8 * i, true), v.getUint32(ptr + 8 * i + 4, true)]);
    return out;
  }

  readonly imports: Record<string, (...args: never[]) => number | void> = {
    clock_time_get: (_id: number, _precision: bigint, out: number) => {
      this.view().setBigUint64(out, BigInt(Math.round(nowMs() * 1e6)), true);
      return ESUCCESS;
    },
    proc_exit: (code: number) => {
      throw new ProcExit(code);
    },
    fd_prestat_get: (fd: number, buf: number) => {
      if (fd !== ROOT_FD) return EBADF;
      const v = this.view();
      v.setUint8(buf, 0);
      v.setUint32(buf + 4, 1, true);
      return ESUCCESS;
    },
    fd_prestat_dir_name: (fd: number, path: number, len: number) => {
      if (fd !== ROOT_FD || len < 1) return EBADF;
      this.bytes(path, 1)[0] = 0x2f; // "/"
      return ESUCCESS;
    },
    fd_fdstat_get: (fd: number, buf: number) => {
      const type = fd <= 2 ? FILETYPE_CHAR : fd === ROOT_FD ? FILETYPE_DIR : this.fds.has(fd) ? FILETYPE_FILE : -1;
      if (type < 0) return EBADF;
      const v = this.view();
      v.setUint8(buf, type);
      v.setUint16(buf + 2, 0, true);
      v.setBigUint64(buf + 8, 0xffffffffffffffffn, true);
      v.setBigUint64(buf + 16, 0xffffffffffffffffn, true);
      return ESUCCESS;
    },
    fd_fdstat_set_flags: () => ESUCCESS,
    path_open: (dirfd: number, _dirflags: number, pathPtr: number, pathLen: number, oflags: number,
      _rightsBase: bigint, _rightsInheriting: bigint, _fdflags: number, fdOut: number) => {
      if (dirfd !== ROOT_FD) return EBADF;
      const path = normalise(decode(this.bytes(pathPtr, pathLen).slice()));
      if (!this.files.has(path)) {
        if (!(oflags & O_CREAT)) return ENOENT;
        this.files.set(path, new Uint8Array(0));
      } else if (oflags & O_TRUNC) {
        this.files.set(path, new Uint8Array(0));
      }
      const fd = this.nextFd++;
      this.fds.set(fd, { path, pos: 0 });
      this.view().setUint32(fdOut, fd, true);
      return ESUCCESS;
    },
    fd_close: (fd: number) => (this.fds.delete(fd) ? ESUCCESS : EBADF),
    fd_seek: (fd: number, offset: bigint, whence: number, out: number) => {
      const f = this.fds.get(fd);
      if (!f) return EBADF;
      const size = this.files.get(f.path)!.length;
      const pos = Number(offset) + (whence === 0 ? 0 : whence === 1 ? f.pos : size);
      if (pos < 0) return EINVAL;
      f.pos = pos;
      this.view().setBigUint64(out, BigInt(pos), true);
      return ESUCCESS;
    },
    fd_read: (fd: number, iovs: number, n: number, out: number) => {
      const f = this.fds.get(fd);
      if (!f) return EBADF;
      const data = this.files.get(f.path)!;
      let total = 0;
      for (const [ptr, len] of this.iovs(iovs, n)) {
        const chunk = data.subarray(f.pos, Math.min(f.pos + len, data.length));
        this.bytes(ptr, chunk.length).set(chunk);
        f.pos += chunk.length;
        total += chunk.length;
        if (chunk.length < len) break;
      }
      this.view().setUint32(out, total, true);
      return ESUCCESS;
    },
    fd_write: (fd: number, iovs: number, n: number, out: number) => {
      let total = 0;
      if (fd === 1 || fd === 2) {
        for (const [ptr, len] of this.iovs(iovs, n)) {
          this.output(decode(this.bytes(ptr, len).slice()), fd);
          total += len;
        }
      } else {
        const f = this.fds.get(fd);
        if (!f) return EBADF;
        for (const [ptr, len] of this.iovs(iovs, n)) {
          // files grow by doubling a private buffer; the Map holds a view of the written length
          let data = this.files.get(f.path)!;
          const end = f.pos + len;
          if (end > data.length) {
            if (data.byteOffset !== 0 || end > data.buffer.byteLength) {
              let cap = Math.max(data.buffer.byteLength, 4096);
              while (cap < end) cap *= 2;
              const grown = new Uint8Array(new ArrayBuffer(cap), 0, end);
              grown.set(data);
              data = grown;
            } else {
              data = new Uint8Array(data.buffer, 0, end);
            }
          }
          data.set(this.bytes(ptr, len), f.pos);
          this.files.set(f.path, data);
          f.pos += len;
          total += len;
        }
      }
      this.view().setUint32(out, total, true);
      return ESUCCESS;
    },
  };
}
