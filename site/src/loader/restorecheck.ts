/**
 * The content-mode restore check (the core's checkContentRestore) on folders the user picks: a backup of the OP-Z
 * disk on the computer, and optionally the OP-Z disk now. Only names and sizes are read; nothing is written.
 * docs/roadmap/restore-space.md.
 */

import type { DiskEntry } from "./core.ts";

/** The part of FileSystemDirectoryHandle the walk uses (a fake in the tests). */
export interface ListDir {
  readonly kind: "directory";
  readonly name: string;
  values(): AsyncIterable<ListDir | ListFile>;
}

export interface ListFile {
  readonly kind: "file";
  readonly name: string;
  getFile(): Promise<{ size: number }>;
}

/** Every file under `dir` as a path relative to it. Picking the disk's samplepacks folder itself keeps that prefix. */
export async function listFolder(dir: ListDir, limit = 5000): Promise<DiskEntry[]> {
  const out: DiskEntry[] = [];
  const prefix = dir.name.toLowerCase() === "samplepacks" ? "samplepacks/" : "";
  const walk = async (d: ListDir, path: string): Promise<void> => {
    for await (const e of d.values()) {
      if (out.length >= limit) throw new Error(`more than ${limit} files: is this the OP-Z backup folder?`);
      if (e.kind === "directory") {
        if (!e.name.startsWith(".")) await walk(e, `${path}${e.name}/`);
      } else out.push({ path: `${path}${e.name}`, size: (await e.getFile()).size });
    }
  };
  await walk(dir, prefix);
  return out;
}
