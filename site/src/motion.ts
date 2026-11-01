/**
 * Motion: GSAP for every animation, Lenis as the one smooth-scroll engine, ScrollTrigger for the reveals.
 * Under prefers-reduced-motion nothing animates and nothing smooth-scrolls: the final state is rendered at once.
 * Each page calls `pageMotion(root)` and gets back a cleanup that the router runs before the next page.
 */

import { gsap } from "gsap";
import { ScrollTrigger } from "gsap/ScrollTrigger";
import Lenis from "lenis";

gsap.registerPlugin(ScrollTrigger);

const reduceQuery = window.matchMedia("(prefers-reduced-motion: reduce)");
export const reducedMotion = (): boolean => reduceQuery.matches;

/**
 * Splits a heading into word spans for a staggered reveal. The heading keeps its full text as its accessible name;
 * the split words are hidden from assistive technology. Only plain-text headings are split (never links or markup).
 */
function splitWords(el: HTMLElement): HTMLElement[] {
  if (el.children.length > 0 || el.dataset.split === "1") return [];
  const text = el.textContent ?? "";
  el.setAttribute("aria-label", text.trim());
  el.dataset.split = "1";
  el.textContent = "";
  const words: HTMLElement[] = [];
  for (const [i, word] of text.trim().split(/\s+/).entries()) {
    if (i > 0) el.append(" ");
    const outer = document.createElement("span");
    outer.className = "word";
    outer.setAttribute("aria-hidden", "true");
    const inner = document.createElement("span");
    inner.textContent = word;
    outer.append(inner);
    el.append(outer);
    words.push(inner);
  }
  return words;
}

/** The landing: the image settles, then the Enter key arrives. Usable from the first frame either way. */
export function gateIntro(root: HTMLElement): () => void {
  if (reducedMotion()) return () => {};
  const ctx = gsap.context(() => {
    gsap.timeline({ defaults: { ease: "power3.out" } })
      .from(".gate__img", { scale: 1.06, opacity: 0, duration: 1.6 })
      .from(".gate__enter", { y: 16, opacity: 0, duration: 0.8 }, "-=0.7")
      .from(".gate__meta", { opacity: 0, duration: 0.8, stagger: 0.1 }, "-=0.5");
  }, root);
  return () => ctx.revert();
}

let lenis: Lenis | null = null;

function startLenis(): void {
  if (lenis || reducedMotion()) return;
  lenis = new Lenis({ duration: 1.1, smoothWheel: true });
  lenis.on("scroll", ScrollTrigger.update);
  gsap.ticker.add(tick);
  gsap.ticker.lagSmoothing(0);
}

function tick(time: number): void { lenis?.raf(time * 1000); }

function stopLenis(): void {
  if (!lenis) return;
  gsap.ticker.remove(tick);
  lenis.destroy();
  lenis = null;
}

/** Scrolls to the top on a page change (through Lenis when it runs). */
export function scrollTop(): void {
  if (lenis) lenis.scrollTo(0, { immediate: true });
  else window.scrollTo(0, 0);
}

/**
 * Reveals for a page: `[data-reveal="words"]` headings word by word, `[data-reveal]` blocks fade up, in order, as
 * they scroll in. Returns the cleanup.
 */
export function pageMotion(root: HTMLElement, { smooth = true } = {}): () => void {
  if (reducedMotion()) return () => {};
  if (smooth) startLenis();
  const ctx = gsap.context(() => {
    for (const el of root.querySelectorAll<HTMLElement>("[data-reveal]")) {
      const words = el.dataset.reveal === "words" ? splitWords(el) : [];
      const targets = words.length ? words : el;
      gsap.from(targets, {
        yPercent: words.length ? 110 : 0,
        y: words.length ? 0 : 24,
        opacity: words.length ? 1 : 0,
        duration: words.length ? 0.9 : 0.8,
        ease: "power3.out",
        stagger: words.length ? 0.045 : 0,
        scrollTrigger: { trigger: el, start: "top 88%", once: true },
      });
    }
  }, root);
  // Fonts and images change measurements after the first layout.
  document.fonts?.ready.then(() => ScrollTrigger.refresh());
  return () => {
    ctx.revert();
    stopLenis();
  };
}

reduceQuery.addEventListener("change", () => { if (reducedMotion()) stopLenis(); });
