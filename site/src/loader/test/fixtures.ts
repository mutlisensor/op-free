/**
 * A made-up catalogue for the tests: twelve synths (nine featured, so the default has to stop at eight), five drums
 * (three featured), four effects (none featured; one for fx1 only), and a test plug that must never be offered.
 * Plug ids and memory slots are in our ranges and clear of TE's (docs/sdk/memory-slots.md).
 */

import type { CatalogueEngine, Kind } from "../../shared/catalogue.ts";

const MASK: Record<Kind, number> = { synth: 0xf0, drum: 0x0f, fx: 0x300 };
const FREE_SLOTS = [7, 6, 5, 3, 2, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40];
let next = 0;

export function engine(dir: string, kind: Kind, id: number, options: { featured?: boolean; mask?: number; built?: boolean; version?: number } = {}): CatalogueEngine {
  const values = [128, 128, 0, 64, 255, 64, 0, 0, 255, 0, 128, 200, 0, 128, 100, 255, 0, 0];
  const slots = kind === "synth" ? [0, 1, 2, 3] : [0];
  return {
    dir, label: dir, kind, featured: options.featured ?? false,
    manifest: {
      engine: dir, memoryslot: FREE_SLOTS[next++ % FREE_SLOTS.length], engine_version: options.version ?? 1,
      plug: { id, version: options.version ?? 1, name: dir, trackmask: options.mask ?? MASK[kind], flags: kind === "fx" ? 1 : 0, parameter_names: ["p1", "p2"] },
      presets: "presets.json",
    } as CatalogueEngine["manifest"],
    presets: slots.map((slot) => ({ name: `${dir} ${slot}`, values, slot, index: 0, octave: 0 })),
    wasm: null,
    engine: options.built === false ? null : `engines/${dir}.engine`,
    summary: null, help: null, author: null,
  };
}

export function catalogue(): CatalogueEngine[] {
  next = 0;
  const synths = Array.from({ length: 12 }, (_, i) => engine(`syn${i + 1}`, "synth", 61010 + i, { featured: i < 9 }));
  const drums = Array.from({ length: 5 }, (_, i) => engine(`drm${i + 1}`, "drum", 61030 + i, { featured: i < 3 }));
  const fx = [
    engine("fxa", "fx", 61040), engine("fxb", "fx", 61041), engine("fxonly1", "fx", 61042, { mask: 0x100 }), engine("fxd", "fx", 61043),
  ];
  const test = engine("opz_test_sine", "synth", 60901, { featured: true });
  return [test, ...synths, ...drums, ...fx];
}

/** A placeholder `.engine` (the fake OP-Z stores files and never runs them). */
export function placeholder(): Uint8Array {
  return new Uint8Array(2048).fill(0x5a);
}
