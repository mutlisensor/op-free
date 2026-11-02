/**
 * The loader's sheets on <dialog>: the one confirm sheet every write goes through (after the flow has backed up: each
 * slot before → after, the files sent, a restart, the safety net, the core's lines), the bake's confirm, and results
 * that need advice. The parts follow web/app/src/ui/confirm.ts, in the site's look.
 */

import type { CatalogueEngine } from "../shared/catalogue.ts";
import type { LoaderConfirm, OpResult } from "./controller.ts";
import type { BakeConfirm } from "./disk.ts";
import { type Child, h } from "./dom.ts";
import { explain } from "./explain.ts";
import { TRACK_LABELS } from "./model.ts";
import { type Cell, plugLabel } from "./plan.ts";

export interface SheetAction {
  value: string;
  label: string;
  kind?: "primary" | "danger" | "ghost";
}

/** Resolves with the value of the button pressed ("cancel" for Escape). */
export function openSheet(title: string, body: Child, actions: SheetAction[], testId?: string): Promise<string> {
  return new Promise((resolve) => {
    const dialog = h("dialog", { class: "dialog ld-sheet", "aria-label": title, "data-testid": testId });
    // resolve from the button itself: the "close" event is a queued task that a hidden or throttled page may not run
    let done = false;
    const finish = (value: string) => {
      if (done) return;
      done = true;
      if (dialog.open) dialog.close(value);
      dialog.remove();
      resolve(value);
    };
    const buttons = actions.map((a) => h("button", {
      type: "button", class: `btn ${a.kind === "primary" ? "btn--primary" : a.kind === "danger" ? "btn--danger" : "btn--ghost"}`,
      "data-action": a.value, onclick: () => finish(a.value),
    }, a.label));
    dialog.append(h("div", { class: "ld-sheet__in" },
      h("h2", { class: "ld-sheet__title" }, title),
      h("div", { class: "ld-sheet__body" }, body),
      h("div", { class: "ld-sheet__actions" }, buttons)));
    dialog.addEventListener("cancel", (e) => {
      e.preventDefault();
      finish("cancel");
    });
    dialog.addEventListener("close", () => finish(dialog.returnValue || "cancel"));
    document.body.append(dialog);
    dialog.showModal();
    (buttons.find((b) => b.classList.contains("btn--primary")) ?? buttons.at(-1))?.focus();
  });
}

const trackName = (t: number) => TRACK_LABELS[t] ?? (t === 10 ? "tape" : t === 11 ? "master" : String(t));

/** Every changed cell, grouped by track: "1  analoga → swirl". */
function changeList(cells: Cell[], label: (id: number | null) => string): HTMLElement {
  const tracks = [...new Set(cells.map((c) => c.track))].sort((a, b) => a - b);
  return h("div", { class: "ld-changes", "data-testid": "change-list" }, tracks.map((t) => h("div", { class: "ld-changes__track" },
    h("p", { class: "label" }, trackName(t)),
    h("ul", null, cells.filter((c) => c.track === t).sort((a, b) => a.slot - b.slot).map((c) => h("li", null,
      h("span", { class: "ld-key" }, String(c.slot % 10)),
      h("span", { class: "ld-from" }, label(c.from)), h("span", { class: "ld-arrow", "aria-label": "becomes" }, "→"),
      h("span", { class: c.to !== null && c.to >= 61000 && c.to < 62000 ? "ld-to ld-ours" : "ld-to" }, label(c.to))))))));
}

function safety(backupId: string | null, midi: boolean, extra: string): HTMLElement {
  return h("ul", { class: "ld-safety", "aria-label": "Safety" },
    backupId ? h("li", { "data-testid": "confirm-backup" }, "Backed up first: ", h("code", null, backupId)) : null,
    h("li", null, extra),
    midi ? h("li", null, "Keep this tab in front and the OP-Z connected until it is done.") : null);
}

function details(lines: string[]): HTMLElement {
  const plan = lines.filter((l) => !l.startsWith("  "));
  return h("details", { class: "ld-details" }, h("summary", null, "Details from the core"),
    h("pre", { class: "ld-pre" }, plan.join("\n") || "No plan lines."));
}

export function confirmBody(r: LoaderConfirm, engines: readonly CatalogueEngine[], midi: boolean): Child[] {
  const label = (id: number | null) => plugLabel(r.device?.plugs ?? null, id, engines);
  const body: Child[] = [h("p", { class: "ld-question" }, r.question)];
  if (r.changes.length) {
    body.push(h("p", { class: "label" }, `Every slot that changes (${r.changes.length}), before → after`), changeList(r.changes, label));
  } else {
    body.push(h("p", { class: "dim" }, "No slot changes."));
  }
  if (r.skipped.length) {
    body.push(h("p", { class: "ld-note", "data-testid": "confirm-skipped" }, h("b", null, "Left alone: "),
      r.skipped.map((c) => `${trackName(c.track)} slot ${c.slot} (${c.why === "user-sample" ? label(c.from) : "plug no longer installed"})`).join(", "), "."));
  }
  const work = r.installs.filter((i) => i.action !== "ready");
  if (work.length) {
    body.push(h("p", { class: "label" }, "Engines sent"), h("ul", { class: "ld-list" }, work.map((i) =>
      h("li", null, `${i.label} · plug ${i.plugId} v${i.version}${i.action === "upgrade" ? " (upgrade)" : ""} · ${i.presets} preset${i.presets === 1 ? "" : "s"}`))));
  }
  if (r.files.length) body.push(h("details", { class: "ld-details" }, h("summary", null, `Files sent (${r.files.length})`), h("ul", { class: "ld-list" }, r.files.map((f) => h("li", null, f)))));
  if (r.effects.length) body.push(h("ul", { class: "ld-effects" }, r.effects.map((e) => h("li", null, e))));
  if (r.restart) {
    body.push(h("p", { class: "ld-callout ld-callout--warn", "data-testid": "confirm-restart" }, h("b", null, "Restart needed. "),
      "An engine here was already registered on the OP-Z, which keeps the code it loaded at start-up. Switch the OP-Z off "
      + "and on after the install, before you play it."));
  }
  if (r.op === "install") {
    body.push(h("p", { class: "ld-tested" }, "Not yet tested on hardware as one batch: installing without a slot, then one "
      + "slot-map write. Each step is the core's flow that the command-line tools use on the OP-Z."));
  }
  body.push(safety(r.backupId, midi, r.op === "install"
    ? "Each engine is installed, read back and checked on its own; the batch stops at the first problem. The slot map is read back and rolled back to the backup if it differs."
    : "Read back afterwards; rolled back to the backup if it differs."));
  body.push(details(r.lines));
  return body;
}

export async function confirmSheet(r: LoaderConfirm, engines: readonly CatalogueEngine[], midi: boolean): Promise<boolean> {
  const title = r.op === "install" ? "Install on your OP-Z" : "Restore a backup";
  const verb = r.op === "install" ? "Install" : "Restore";
  return (await openSheet(title, confirmBody(r, engines, midi), [{ value: "cancel", label: "Cancel" }, { value: "yes", label: verb, kind: "primary" }], "confirm")) === "yes";
}

/** The bake's confirm: per engine what goes in, which hand-stored presets stay or are replaced. */
export async function bakeSheet(c: BakeConfirm): Promise<boolean> {
  const body: Child[] = [];
  if (c.request.kind === "bake" && c.plan) {
    body.push(h("p", { class: "ld-question" }, `Write ${c.plan.changed.length} preset record${c.plan.changed.length === 1 ? "" : "s"} into config/userPresets.dat on ${c.disk}?`));
    body.push(h("ul", { class: "ld-list", "data-testid": "bake-plan" }, [...c.plan.report].map(([name, r]) => h("li", null,
      `${name}: ${r.new} new, ${r.updated} updated, ${r.replaced} replaced, ${r.same} already there`
      + (r.kept.length ? ` · your stored ${r.kept.map(([s, i, n]) => `“${n}” (slot ${s} key ${i + 1})`).join(", ")} stay` : "")))));
    body.push(h("p", { class: "dim" }, "Only this one file is written; everything else on the disk stays as it is."));
  } else if (c.request.kind === "restore") {
    body.push(h("p", { class: "ld-question" }, `Put ${c.request.source} back as config/userPresets.dat on ${c.disk}?`));
  }
  body.push(h("p", { class: "ld-tested" }, "The bake is proven on hardware with the command-line tool (H-9); from the browser it is not yet tested on hardware."));
  body.push(safety(c.backupId, false, "Read back after writing; the original goes back on the disk if the bytes differ. "
    + "The backup is the preset file on your OP-Z now, kept in this browser: step 5 puts it back."));
  body.push(details(c.lines));
  return (await openSheet("Content mode: presets that stay", body, [{ value: "cancel", label: "Cancel" }, { value: "yes", label: c.request.kind === "bake" ? "Write presets" : "Put it back", kind: "primary" }], "bake-confirm")) === "yes";
}

/** A failed operation: what went wrong, what to do. */
export async function failureSheet(result: OpResult, what: string): Promise<void> {
  const body: Child[] = [];
  if (result.error) body.push(h("p", { class: "ld-callout ld-callout--error" }, result.error));
  const advice = result.error ? explain(result.error, result.op) : null;
  if (advice) body.push(h("div", { "data-testid": "advice" }, h("p", null, h("b", null, advice.title)), h("ul", { class: "ld-list" }, advice.steps.map((s) => h("li", null, s)))));
  if (result.highlights.length) body.push(h("pre", { class: "ld-pre" }, result.highlights.join("\n")));
  await openSheet(`${what} failed`, body, [{ value: "ok", label: "OK", kind: "primary" }], "result-failed");
}
