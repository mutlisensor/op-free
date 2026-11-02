/**
 * The demo OP-Z: what the core's FakeOpz starts with in the loader, and a demo content-mode disk for the bake.
 *
 * The factory state is closer to a real unit than the core's three-plug default: every sound slot of the drum and
 * synth tracks filled, some effects, and three of the visitor's own samples (flag 2) on the sample track, one of them
 * in slot 4, so the plan has to step around it. Plug ids and file names are made up; engine names are the ones the
 * core already lists as TE's (TE_ENGINES), nothing of teenage engineering's is in it.
 */

import { type DiskDir, type DiskFile } from "./disk.ts";
import { USER_PRESETS_RECORD, USER_PRESETS_RECORDS, dumps, serialiseUserPresets, utf8 } from "./core.ts";

const SYNTHS = ["analoga", "digital", "fm003b", "saw013a", "organ", "ep", "electric", "cluster", "chord", "ministringshort"];
const FX = ["delay", "reverb", "fdnreverb", "chorus", "chorus2", "dist", "crush"];
/** Memory slots of those engines (docs/sdk/memory-slots.md, "Slots used by TE"). */
const MEMORY: Record<string, number> = {
  analoga: 27, digital: 62, fm003b: 13, saw013a: 16, organ: 18, ep: 4, electric: 77, cluster: 83, chord: 59,
  ministringshort: 82, delay: 61, reverb: 66, fdnreverb: 89, chorus: 60, chorus2: 88, dist: 78, crush: 79,
  sampleplayergain: 10, synthsampler: 68, tapetwo: 9,
};
const DRUMS = ["kick", "snare", "perc", "sample"];
/** The visitor's own samples on the demo unit: [plug id, sample track slot]. */
export const DEMO_USER_SAMPLES: ReadonlyArray<[number, number]> = [[1_513_459_487, 4], [1_513_460_102, 9], [1_513_461_377, 10]];

export function demoFiles(): Map<string, Uint8Array> {
  const engines = Object.entries(MEMORY).map(([name, memoryslot]) => ({ memoryslot, path: `engines/${name}.engine`, version: 2 }));
  const plugs: Array<Record<string, unknown>> = [];
  const tracks: Array<{ index: number; packs: Array<{ id: number; slot: number }> }> = [];
  DRUMS.forEach((name, t) => {
    const packs: Array<{ id: number; slot: number }> = [];
    for (let s = 1; s <= 10; s++) {
      const mine = DEMO_USER_SAMPLES.find(([, slot]) => t === 3 && slot === s);
      if (mine) {
        packs.push({ id: mine[0], slot: s });
        continue;
      }
      const id = 100 + t * 10 + s;
      plugs.push({ engine: "engines/sampleplayergain.engine", flags: 0, id, samples: `samplepacks/${t + 1}-${name}/demo-${s}.aif`, version: 1 });
      packs.push({ id, slot: s });
    }
    tracks.push({ index: t, packs });
  });
  for (const [id] of DEMO_USER_SAMPLES) {
    plugs.push({ engine: "engines/sampleplayergain.engine", flags: 2, id, samples: `samplepacks/user/my-sample-${id % 1000}.aif`, version: 1 });
  }
  SYNTHS.forEach((name, i) => plugs.push({ engine: `engines/${name}.engine`, flags: 0, id: i + 1, samples: "", version: 25 }));
  for (let t = 4; t <= 7; t++) tracks.push({ index: t, packs: SYNTHS.map((_, i) => ({ id: i + 1, slot: i + 1 })) });
  FX.forEach((name, i) => plugs.push({ engine: `engines/${name}.engine`, flags: 1, id: 30 + i, samples: "", version: 3 }));
  for (const t of [8, 9]) tracks.push({ index: t, packs: FX.map((_, i) => ({ id: 30 + i, slot: i + 1 })) });
  plugs.push({ engine: "engines/tapetwo.engine", flags: 1, id: 50, samples: "", version: 1 });
  tracks.push({ index: 10, packs: [{ id: 50, slot: 1 }] });
  plugs.push({ engine: "engines/chorus.engine", flags: 1, id: 39, samples: "", version: 3 });
  tracks.push({ index: 11, packs: [{ id: 39, slot: 1 }] });
  plugs.sort((a, b) => Number(a.id) - Number(b.id));
  return new Map([
    ["settings/plugs.json", utf8(dumps({ engines, plugs }))],
    ["settings/slotConfiguration.json", utf8(dumps({ tracks }))],
  ]);
}

/** An empty user-preset store as the OP-Z writes it (version 1, 200 free records). */
export function emptyUserPresets(): Uint8Array {
  return serialiseUserPresets(Array.from({ length: USER_PRESETS_RECORDS }, () => new Uint8Array(USER_PRESETS_RECORD)));
}

function notFound(): Error {
  return Object.assign(new Error("not found"), { name: "NotFoundError" });
}

/**
 * The demo OP-Z's content-mode disk, in memory: a top folder "OP-Z" with config/userPresets.dat. It stands in for the
 * folder picker, so the bake (backup, confirm, write, read-back) runs exactly as on a real disk.
 */
export class DemoDisk implements DiskDir {
  readonly kind = "directory";
  readonly name: string;
  readonly files: Map<string, Uint8Array>;
  private readonly prefix: string;

  constructor(files: Map<string, Uint8Array> = new Map([["config/userPresets.dat", emptyUserPresets()]]), name = "OP-Z", prefix = "") {
    this.files = files;
    this.name = name;
    this.prefix = prefix;
  }

  async getDirectoryHandle(name: string): Promise<DiskDir> {
    const p = this.prefix + name + "/";
    if (![...this.files.keys()].some((k) => k.startsWith(p))) throw notFound();
    return new DemoDisk(this.files, name, p);
  }

  async getFileHandle(name: string): Promise<DiskFile> {
    const path = this.prefix + name;
    const files = this.files;
    if (!files.has(path)) throw notFound();
    return {
      kind: "file", name,
      getFile: async () => ({ arrayBuffer: async () => files.get(path)!.slice().buffer }),
      createWritable: async () => {
        let buf = new Uint8Array(0);
        return {
          write: async (d: Uint8Array) => void (buf = d.slice()),
          close: async () => void files.set(path, buf),
          abort: async () => {},
        };
      },
    };
  }
}
