/**
 * Backups in this browser: the core's `BackupStore` (the core saves one before every write) plus what the loader
 * needs: a list, the first backup of a device (what "original" and "Restore original layout" go back to), and the
 * bake's copies of config/userPresets.dat. Ported from web/app/src/backups.ts: same records, same ids
 * (`<UTC time>-<label>`), same files as a CLI backup directory. IndexedDB lives in this browser profile only.
 *
 * Where IndexedDB is missing or blocked (a private window, the tests), `MemoryLoaderStore` keeps them for the page's
 * lifetime and the UI says so.
 */

import { type BackupStore, SLOTS, loads } from "./core.ts";

export const DB_NAME = "op-free";
const STORE = "backups";
/** The file a userPresets.dat backup holds (no settings files: it goes back on the content disk, not over MIDI). */
export const USER_PRESETS_FILE = "userPresets.dat";

export interface BackupRecord {
  id: string;
  label: string;
  /** ISO time it was made. */
  created: string;
  /** "demo", the MIDI port's name, or the disk's. */
  source: string;
  files: Record<string, Uint8Array>;
}

export interface LoaderStore extends BackupStore {
  /** Recorded as `source` on new backups: the loader sets it on connect. */
  source: string;
  /** Called after every save of settings (the controller reads the plan inputs from it). */
  onSave: ((record: BackupRecord) => void) | null;
  /** Kept only for this page (no IndexedDB). */
  readonly volatile: boolean;
  get(id: string): Promise<BackupRecord | undefined>;
  /** Newest first. */
  list(): Promise<BackupRecord[]>;
  /** A bake's copy of config/userPresets.dat (label "userpresets"). */
  saveUserPresets(data: Uint8Array, meta: Uint8Array, source: string): Promise<string>;
}

export function isSettingsBackup(r: BackupRecord): boolean {
  return SLOTS in r.files;
}

export function isUserPresetsBackup(r: BackupRecord): boolean {
  return USER_PRESETS_FILE in r.files && !(SLOTS in r.files);
}

/** The oldest settings backup from `source`: the layout before this browser changed anything on that OP-Z. */
export function firstBackup(list: readonly BackupRecord[], source: string): BackupRecord | null {
  return [...list].reverse().find((r) => isSettingsBackup(r) && r.source === source) ?? null;
}

/** The slot map a settings backup holds. */
export function backupSlots(r: BackupRecord): ReturnType<typeof loads> {
  return loads(r.files[SLOTS]);
}

/** The CLI's directory name for a backup made now: 20261017T213028961000Z (µs padded from ms). */
export function stampId(now: Date): string {
  return now.toISOString().replace(/[-:]/g, "").replace(/\.(\d+)Z$/, "$1000Z");
}

function byNewest(a: BackupRecord, b: BackupRecord): number {
  return a.created < b.created ? 1 : a.created > b.created ? -1 : a.id < b.id ? 1 : -1;
}

/** The parts both stores share: ids, the save hook, annotate. */
abstract class BaseStore implements LoaderStore {
  source = "";
  onSave: ((record: BackupRecord) => void) | null = null;
  abstract readonly volatile: boolean;
  protected readonly now: () => Date;

  constructor(now: () => Date = () => new Date()) {
    this.now = now;
  }

  abstract get(id: string): Promise<BackupRecord | undefined>;
  protected abstract add(record: BackupRecord): Promise<void>;
  protected abstract put(record: BackupRecord): Promise<void>;
  protected abstract all(): Promise<BackupRecord[]>;

  async list(): Promise<BackupRecord[]> {
    return (await this.all()).sort(byNewest);
  }

  /** A new record `<stamp>-<label>`: never overwrites a backup. */
  private async create(label: string, source: string, files: Map<string, Uint8Array>): Promise<BackupRecord> {
    const now = this.now();
    const stamp = stampId(now);
    let id = stamp + (label ? "-" + label : "");
    for (let n = 2; await this.get(id); n++) id = `${stamp}.${n}` + (label ? "-" + label : "");
    const record: BackupRecord = { id, label, created: now.toISOString(), source, files: Object.fromEntries(files) };
    await this.add(record);
    return record;
  }

  async save(label: string, files: Map<string, Uint8Array>): Promise<string> {
    const record = await this.create(label, this.source, files);
    this.onSave?.(record);
    return record.id;
  }

  async saveUserPresets(data: Uint8Array, meta: Uint8Array, source: string): Promise<string> {
    return (await this.create("userpresets", source, new Map([[USER_PRESETS_FILE, data], ["backup.json", meta]]))).id;
  }

  async load(id: string): Promise<Map<string, Uint8Array>> {
    const record = await this.get(id);
    if (record === undefined) throw new Error(`no backup ${id}`);
    return new Map(Object.entries(record.files));
  }

  async annotate(id: string, name: string, data: Uint8Array): Promise<void> {
    const record = await this.get(id);
    if (record === undefined) throw new Error(`no backup ${id}`);
    record.files[name] = data;
    await this.put(record);
  }
}

export class MemoryLoaderStore extends BaseStore {
  readonly volatile = true;
  readonly records = new Map<string, BackupRecord>();

  async get(id: string): Promise<BackupRecord | undefined> {
    return this.records.get(id);
  }

  protected async add(record: BackupRecord): Promise<void> {
    if (this.records.has(record.id)) throw new Error(`backup ${record.id} exists`);
    this.records.set(record.id, record);
  }

  protected async put(record: BackupRecord): Promise<void> {
    this.records.set(record.id, record);
  }

  protected async all(): Promise<BackupRecord[]> {
    return [...this.records.values()];
  }
}

function request<T>(req: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

export class IndexedDbLoaderStore extends BaseStore {
  readonly volatile = false;
  private readonly db: IDBDatabase;

  constructor(db: IDBDatabase, now?: () => Date) {
    super(now);
    this.db = db;
  }

  static open(factory: IDBFactory = indexedDB, name = DB_NAME): Promise<IndexedDbLoaderStore> {
    return new Promise((resolve, reject) => {
      const req = factory.open(name, 1);
      req.onupgradeneeded = () => {
        if (!req.result.objectStoreNames.contains(STORE)) req.result.createObjectStore(STORE, { keyPath: "id" });
      };
      req.onsuccess = () => resolve(new IndexedDbLoaderStore(req.result));
      req.onerror = () => reject(req.error);
      req.onblocked = () => reject(new Error("the backup database is open in another tab with an older version"));
    });
  }

  private store(mode: IDBTransactionMode): IDBObjectStore {
    return this.db.transaction(STORE, mode).objectStore(STORE);
  }

  get(id: string): Promise<BackupRecord | undefined> {
    return request(this.store("readonly").get(id) as IDBRequest<BackupRecord | undefined>);
  }

  protected async add(record: BackupRecord): Promise<void> {
    await request(this.store("readwrite").add(record));
  }

  protected async put(record: BackupRecord): Promise<void> {
    await request(this.store("readwrite").put(record));
  }

  protected all(): Promise<BackupRecord[]> {
    return request(this.store("readonly").getAll() as IDBRequest<BackupRecord[]>);
  }
}

/** IndexedDB when it opens, else the page-lifetime store. */
export async function openStore(): Promise<LoaderStore> {
  try {
    if (typeof indexedDB === "undefined") throw new Error("no IndexedDB");
    return await IndexedDbLoaderStore.open();
  } catch {
    return new MemoryLoaderStore();
  }
}
