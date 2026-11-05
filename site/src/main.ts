/**
 * OP-FREE: the router. `#/` is the landing (image + Enter), Enter opens the risk notice, then `#/home`,
 * `#/synths` (lab + loader) and `#/firmware`. Pages past the landing need the notice accepted in this session.
 */

import "./style.css";
import { askConsent, hasAccepted } from "./consent.ts";
import { mountLab } from "./lab/index.ts";
import { mountLoader } from "./loader/index.ts";
import { gateIntro, pageMotion, scrollTop } from "./motion.ts";
import { FIRMWARE, FOOTER, GATE, HEADER, HOME, SYNTHS } from "./pages.ts";
import { copyReport, startDiag } from "./shared/diag.ts";
import type { Mounted } from "./shared/mount.ts";

startDiag();
const app = document.getElementById("app")!;
let cleanups: Array<() => void> = [];

function teardown(): void {
  for (const fn of cleanups.splice(0)) fn();
}

function route(): { page: string; anchor: string | null } {
  const [, page = "", anchor = null] = location.hash.match(/^#\/?([^#]*)(?:#(.*))?$/) ?? [];
  return { page, anchor };
}

async function enter(next = "home"): Promise<void> {
  if (hasAccepted() || (await askConsent())) location.hash = `#/${next}`;
}

function renderGate(pending: string | null): void {
  app.innerHTML = GATE;
  document.title = "OP-FREE";
  const button = app.querySelector<HTMLButtonElement>("[data-enter]")!;
  button.addEventListener("click", () => enter(pending ?? "home"));
  cleanups.push(gateIntro(app));
  button.focus({ preventScroll: true });
  // A deep link that hasn't accepted the notice yet lands here with the notice open.
  if (pending) void enter(pending);
}

function renderPage(page: string, anchor: string | null): void {
  const body = page === "synths" ? SYNTHS : page === "firmware" ? FIRMWARE : HOME;
  const titles: Record<string, string> = { synths: "Synths & effects · OP-FREE", firmware: "Firmware mods · OP-FREE" };
  app.innerHTML = `<a class="skip" href="#main">Skip to content</a>${HEADER(page)}${body}${FOOTER}`;
  document.title = titles[page] ?? "OP-FREE";
  app.querySelector<HTMLAnchorElement>(".skip")!.addEventListener("click", (e) => {
    e.preventDefault();
    app.querySelector<HTMLElement>("#main")?.focus();
  });
  app.querySelector<HTMLElement>("#main")?.setAttribute("tabindex", "-1");
  wireReport(app.querySelector<HTMLButtonElement>("[data-copy-report]"));

  const mounted: Mounted[] = [];
  if (page === "synths") {
    mounted.push(mountLab(app.querySelector("#lab")!), mountLoader(app.querySelector("#loader")!));
  }
  cleanups.push(() => mounted.forEach((m) => m.destroy()));
  // The synths page is a tool: no smooth scrolling there.
  cleanups.push(pageMotion(app, { smooth: page !== "synths" }));

  if (anchor) requestAnimationFrame(() => document.getElementById(anchor)?.scrollIntoView());
  else scrollTop();
}

/** The footer's *Copy report*: the problem log on the clipboard (src/shared/diag.ts). */
function wireReport(button: HTMLButtonElement | null): void {
  button?.addEventListener("click", async () => {
    const copied = await copyReport();
    button.textContent = copied ? "Report copied" : "Report saved as a file";
    setTimeout(() => (button.textContent = "Copy report"), 2500);
  });
}

function render(): void {
  teardown();
  const { page, anchor } = route();
  if (!page) return renderGate(null);
  if (!hasAccepted()) return renderGate(page);
  renderPage(page, anchor);
}

window.addEventListener("hashchange", render);
render();
