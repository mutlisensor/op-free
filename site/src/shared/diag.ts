/**
 * The site's problem log: kept in this browser only and never sent anywhere. *Copy report* puts it on the clipboard
 * for the visitor to paste wherever they report the problem. Each line is saved as it is written (localStorage is
 * synchronous), so a tab that freezes or crashes still leaves everything up to that moment for the next visit.
 *
 * startDiag() records: page opens and clean closes (a visit with no close ended in a crash, a freeze or a killed tab),
 * uncaught errors and rejections, console.error, the page being frozen (the main thread stalled while visible), and
 * the synths page's events (`opfree:audition`, `opfree:pick`). Modules add their own lines with note() and their own
 * report section with reportPart().
 */

declare const __OPFREE_BUILD__: string;

export interface StorageLike {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem(key: string): void;
}

export const DIAG_KEYS = { log: "op-free.diag.log", open: "op-free.diag.open" };
export const MAX_LINES = 600;
export const MAX_CHARS = 120_000;
/** A visible page whose timers come back this late was frozen. */
export const STALL_MS = 2000;

function stamp(d: Date): string {
  const p = (n: number, w = 2) => String(n).padStart(w, "0");
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
}

/** The log itself, with no browser in it: a store (localStorage, or nothing) and a clock. */
export class DiagLog {
  private all: string[] = [];
  private readonly storage: StorageLike | null;
  private readonly now: () => Date;

  constructor(storage: StorageLike | null, now: () => Date = () => new Date()) {
    this.storage = storage;
    this.now = now;
    try {
      const raw = storage?.getItem(DIAG_KEYS.log);
      const parsed: unknown = raw ? JSON.parse(raw) : [];
      if (Array.isArray(parsed)) this.all = parsed.filter((l): l is string => typeof l === "string");
    } catch {
      this.all = [];
    }
  }

  get lines(): readonly string[] {
    return this.all;
  }

  note(source: string, text: string): void {
    for (const line of String(text).split("\n")) {
      if (line.trim()) this.all.push(`${stamp(this.now())} ${source.padEnd(6)} ${line.slice(0, 2000)}`);
    }
    this.trim();
    this.save();
  }

  /** A divider with the full date, at each page open. */
  mark(text: string): void {
    this.all.push(`── ${this.now().toISOString()} ${text}`);
    this.trim();
    this.save();
  }

  clear(): void {
    this.all = [];
    this.save();
  }

  private trim(): void {
    if (this.all.length > MAX_LINES) this.all.splice(0, this.all.length - MAX_LINES);
    let chars = this.all.reduce((n, l) => n + l.length + 1, 0);
    while (chars > MAX_CHARS && this.all.length > 1) chars -= this.all.shift()!.length + 1;
  }

  private save(): void {
    try {
      this.storage?.setItem(DIAG_KEYS.log, JSON.stringify(this.all));
    } catch {
      // full or blocked: the lines stay in memory for this visit
    }
  }
}

export type ReportPart = () => string | string[] | null;

const parts = new Map<string, ReportPart>();
let log = new DiagLog(null);
let started = false;

function storage(): StorageLike | null {
  try {
    return globalThis.localStorage ?? null;
  } catch {
    return null;
  }
}

export function note(source: string, text: string): void {
  log.note(source, text);
}

export function diagLines(): readonly string[] {
  return log.lines;
}

export function clearDiag(): void {
  log.clear();
}

/** A section of the report (the loader: its picks and connection). Returns the function that removes it. */
export function reportPart(name: string, part: ReportPart): () => void {
  parts.set(name, part);
  return () => {
    if (parts.get(name) === part) parts.delete(name);
  };
}

export function buildId(): string {
  return typeof __OPFREE_BUILD__ === "string" ? __OPFREE_BUILD__ : "dev";
}

function describe(value: unknown): string {
  if (value instanceof Error) return `${value.name}: ${value.message}${value.stack ? `\n${value.stack.split("\n").slice(1, 6).join("\n")}` : ""}`;
  if (typeof value === "string") return value;
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

/** The text *Copy report* puts on the clipboard. */
export function report(env: { url: string; userAgent: string; extra?: string[] }, lines: readonly string[] = log.lines): string {
  const out = ["OP-FREE problem report", `made ${new Date().toISOString()} · site build ${buildId()}`, `page ${env.url}`, `browser ${env.userAgent}`];
  if (env.extra) out.push(...env.extra);
  for (const [name, part] of parts) {
    let value: string | string[] | null;
    try {
      value = part();
    } catch (e) {
      value = `(failed: ${(e as Error).message})`;
    }
    if (value === null) continue;
    out.push("", `[${name}]`, ...(Array.isArray(value) ? value : [value]));
  }
  out.push("", `[log: ${lines.length} lines, oldest first]`, ...(lines.length ? lines : ["(empty)"]));
  return out.join("\n");
}

function browserReport(): string {
  const nav = navigator as Navigator & { requestMIDIAccess?: unknown; deviceMemory?: number };
  return report({
    url: location.href,
    userAgent: navigator.userAgent,
    extra: [
      `screen ${screen.width}×${screen.height} @${devicePixelRatio} · cores ${navigator.hardwareConcurrency ?? "?"}${nav.deviceMemory ? ` · memory ${nav.deviceMemory} GB` : ""}`,
      `web midi ${typeof nav.requestMIDIAccess === "function" ? "yes" : "no"} · audio worklet ${typeof AudioWorkletNode === "function" ? "yes" : "no"} · secure ${isSecureContext ? "yes" : "no"}`,
    ],
  });
}

/** Copy the report; true when it reached the clipboard (else it was saved as a text file). */
export async function copyReport(): Promise<boolean> {
  const text = browserReport();
  try {
    await navigator.clipboard.writeText(text);
    note("site", "report copied");
    return true;
  } catch {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([text], { type: "text/plain" }));
    a.download = `op-free-report-${new Date().toISOString().replace(/[:.]/g, "")}.txt`;
    document.body.append(a);
    a.click();
    a.remove();
    note("site", "report saved as a file (no clipboard)");
    return false;
  }
}

/** Start the log for this visit. Once per page load; safe to call again. */
export function startDiag(): void {
  if (started) return;
  started = true;
  const store = storage();
  log = new DiagLog(store);
  let last: string | null = null;
  try {
    last = store?.getItem(DIAG_KEYS.open) ?? null;
    store?.setItem(DIAG_KEYS.open, new Date().toISOString());
  } catch {
    // blocked storage
  }
  log.mark(`page opened · build ${buildId()} · ${navigator.userAgent}`);
  if (last) note("site", `the visit opened at ${last} never closed: the tab crashed, froze or was killed (or another tab is open)`);

  addEventListener("pagehide", () => {
    note("site", "page closed");
    try {
      store?.removeItem(DIAG_KEYS.open);
    } catch {
      // blocked storage
    }
  });
  addEventListener("pageshow", (e) => {
    if ((e as PageTransitionEvent).persisted) {
      note("site", "page back from the cache");
      try {
        store?.setItem(DIAG_KEYS.open, new Date().toISOString());
      } catch {
        // blocked storage
      }
    }
  });

  addEventListener("error", (e) => {
    const where = e.filename ? ` (${e.filename.replace(location.origin, "")}:${e.lineno}:${e.colno})` : "";
    note("error", `${e.error ? describe(e.error) : e.message}${where}`);
  });
  addEventListener("unhandledrejection", (e) => note("error", `unhandled: ${describe(e.reason)}`));
  const consoleError = console.error.bind(console);
  console.error = (...args: unknown[]) => {
    note("error", args.map(describe).join(" "));
    consoleError(...args);
  };

  document.addEventListener("opfree:audition", (e) => note("lab", `asked to play ${(e as CustomEvent<{ dir: string }>).detail?.dir}`));
  document.addEventListener("opfree:pick", (e) => {
    const d = (e as CustomEvent<{ dir: string; picked: boolean }>).detail;
    note("lab", `${d?.picked ? "picked" : "unpicked"} ${d?.dir}`);
  });
  addEventListener("hashchange", () => note("site", `went to ${location.hash || "#/"}`));

  // A frozen page: the timer comes back late while the page was in front the whole time (background tabs are slowed
  // on purpose, so those gaps don't count).
  let tick = performance.now();
  let visible = document.visibilityState === "visible";
  document.addEventListener("visibilitychange", () => {
    visible = document.visibilityState === "visible";
    tick = performance.now();
  });
  setInterval(() => {
    const now = performance.now();
    const gap = now - tick;
    tick = now;
    if (visible && document.visibilityState === "visible" && gap - 500 > STALL_MS) {
      note("freeze", `the page did not respond for ${(gap / 1000).toFixed(1)} s`);
    }
  }, 500);
}
