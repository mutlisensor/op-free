/**
 * End to end in headless Chrome. Each SDK template and swirl, fm2 and kit load into their slot and the computer
 * keyboard plays them; an effect sounds on a generator with no synth; a synth through FX1 and FX2 follows the send
 * model (send 0 dry, send 255 + dry level 0 only through the effect, a passthrough effect keeps the level, two
 * passthroughs at full send add up); the host checks stay clean; a scenario run in the page matches opzsim's golden
 * hash. Needs `make -C sim wasm`.
 */

import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { join } from "node:path";
import { after, before, test } from "node:test";
import { type Browser, type Page, chromium } from "playwright-core";
import { type ViteDevServer, createServer } from "vite";

const ROOT = join(import.meta.dirname, "..");
const MAC_CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const skip = existsSync(join(ROOT, "../build/wasm")) ? false : "no sim/build/wasm: run make -C sim wasm";

let server: ViteDevServer;
let browser: Browser;
let url: string;

before(async () => {
  if (skip) return;
  server = await createServer({ root: ROOT, configFile: join(ROOT, "vite.config.ts"), server: { port: 0 }, logLevel: "error" });
  await server.listen();
  const address = server.httpServer!.address();
  url = `http://localhost:${typeof address === "object" && address ? address.port : 5174}/`;
  const executablePath = process.env.CHROME_PATH ?? (existsSync(MAC_CHROME) ? MAC_CHROME : undefined);
  browser = await chromium.launch({
    headless: true, args: ["--autoplay-policy=no-user-gesture-required"],
    ...(executablePath ? { executablePath } : { channel: "chrome" }),
  });
});

after(async () => {
  await browser?.close();
  await server?.close();
});

interface Level { peak: number; rms: number; maxPeak: number }

/** The page with these slots, audio started, every slot loaded ("none" for an empty one). */
async function open(synth: string, fx1: string, fx2 = "none"): Promise<Page> {
  const page = await browser.newPage();
  page.on("pageerror", (e) => assert.fail(`page error: ${e.message}`));
  await page.goto(`${url}?synth=${synth}&fx1=${fx1}&fx2=${fx2}`);
  await page.getByTestId("audio").click();
  await page.locator(`body[data-synth-loaded="${synth}"][data-fx1-loaded="${fx1}"][data-fx2-loaded="${fx2}"]`)
    .waitFor({ timeout: 15_000 });
  // the worklet's first stats show the audio is running
  await page.waitForFunction(() => !!(window as unknown as { websim: { state: { lastStats: unknown } } }).websim.state.lastStats);
  return page;
}

const level = (page: Page) => page.evaluate(() => (window as unknown as { websim: { level(): Level } }).websim.level());
const resetLevel = (page: Page) => page.evaluate(() => (window as unknown as { websim: { resetLevel(): void } }).websim.resetLevel());

/** The loudest peak (dBFS) over a while, with keys held if any. */
async function peakWhile(page: Page, keys: string[], ms = 600): Promise<number> {
  await resetLevel(page);
  for (const k of keys) await page.keyboard.down(k);
  await page.waitForTimeout(ms);
  const peak = (await level(page)).maxPeak;
  for (const k of keys) await page.keyboard.up(k);
  return peak;
}

/** Turn a knob fully up or down from the keyboard (End / Home), as a user can. */
async function knobTo(page: Page, param: string, up: boolean): Promise<void> {
  await page.locator(`.knob-dial[data-param="${param}"]`).first().focus();
  await page.keyboard.press(up ? "End" : "Home");
  await page.locator("body").focus();
}

async function checksClean(page: Page): Promise<void> {
  await page.waitForTimeout(200);
  for (const id of ["st-state-synth", "st-state-fx1", "st-state-fx2", "st-add-synth"]) {
    const text = await page.locator(`#${id}`).textContent() ?? "";
    assert.ok(text === "–" || text.startsWith("ok"), `${id}: ${text}`);
  }
}

for (const [id, keys] of [
  ["templates/synth", ["a", "d", "g"]],
  ["templates/drum", ["a"]],
  ["engines/fm2", ["a", "d", "g"]],
  ["engines/kit", ["a", "k"]],
] as const) {
  test(`${id}: keys sound`, { skip }, async () => {
    const page = await open(id, "none");
    await page.waitForTimeout(300);
    const quiet = (await level(page)).peak;
    assert.ok(!(quiet > -90), `silent before a key: ${quiet}`);
    const peak = await peakWhile(page, [...keys]);
    assert.ok(peak > -40, `peak while playing ${peak} dBFS`);
    assert.match(await page.locator("#st-cpu-synth").textContent() ?? "", /not Blackfin cycles/);
    await checksClean(page);
    await page.close();
  });
}

for (const id of ["templates/fx", "engines/swirl"]) {
  test(`${id}: sounds on a generator with no synth`, { skip }, async () => {
    const page = await open("none", id);
    assert.equal(await page.getByTestId("fx1-source").inputValue(), "saw");
    assert.ok(await peakWhile(page, [], 500) > -40, "saw through the effect");
    await page.getByTestId("fx1-source").selectOption("silence");
    await page.waitForTimeout(400);
    assert.ok(!(await peakWhile(page, [], 300) > -90), "silence in, silence out");
    await checksClean(page);
    await page.close();
  });
}

test("synth through an effect: the send crossfades, the dry level keeps the dry path", { skip }, async () => {
  const page = await open("engines/fm2", "engines/opz_test_passthrough");
  assert.equal(await page.getByTestId("fx1-source").inputValue(), "send");
  // passthrough returns its input: crossfading dry into it keeps the level
  await knobTo(page, "fx1-dry", false);
  await knobTo(page, "fx1-send", false);
  const dry = await peakWhile(page, ["a", "g"]);
  await page.waitForTimeout(400);
  await knobTo(page, "fx1-send", true);
  // the send ramps over a few blocks (no clicks), then the stats show it
  await page.getByTestId("mix").filter({ hasText: "100% into FX1, 0% dry" }).waitFor({ timeout: 3000 });
  const wet = await peakWhile(page, ["a", "g"]);
  assert.ok(dry > -40 && Math.abs(wet - dry) < 1.5, `dry ${dry} dBFS, through passthrough ${wet} dBFS`);
  // dry level 255: dry path and return together, about 6 dB louder
  await page.waitForTimeout(400);
  await knobTo(page, "fx1-dry", true);
  const both = await peakWhile(page, ["a", "g"]);
  assert.ok(both - dry > 4, `dry ${dry} dBFS, dry + return ${both} dBFS`);
  await checksClean(page);
  await page.close();
});

test("FX1 and FX2 in parallel: each send feeds its effect, two full returns add up", { skip }, async () => {
  const page = await open("engines/fm2", "engines/opz_test_passthrough", "engines/opz_test_passthrough");
  for (const fx of ["fx1", "fx2"]) await knobTo(page, `${fx}-dry`, false);
  await knobTo(page, "fx1-send", false);
  await knobTo(page, "fx2-send", false);
  await page.getByTestId("mix").filter({ hasText: "0% into FX1, 0% into FX2, 100% dry" }).waitFor({ timeout: 3000 });
  const dry = await peakWhile(page, ["a", "g"]);
  await page.waitForTimeout(400);
  await knobTo(page, "fx2-send", true);
  await page.getByTestId("mix").filter({ hasText: "0% into FX1, 100% into FX2, 0% dry" }).waitFor({ timeout: 3000 });
  const viaFx2 = await peakWhile(page, ["a", "g"]);
  await page.waitForTimeout(400);
  await knobTo(page, "fx1-send", true);
  await page.getByTestId("mix").filter({ hasText: "100% into FX1, 100% into FX2, 0% dry" }).waitFor({ timeout: 3000 });
  const both = await peakWhile(page, ["a", "g"]);
  assert.ok(dry > -40 && Math.abs(viaFx2 - dry) < 1.5, `dry ${dry} dBFS, through FX2 ${viaFx2} dBFS`);
  assert.ok(Math.abs(both - dry - 6) < 1.5, `dry ${dry} dBFS, both returns ${both} dBFS (+6 dB expected)`);
  await checksClean(page);
  await page.close();
});

test("synth through swirl: the effect is heard at full send", { skip }, async () => {
  const page = await open("engines/fm2", "engines/swirl");
  await knobTo(page, "fx1-dry", false);
  await knobTo(page, "fx1-send", true);
  assert.ok(await peakWhile(page, ["a", "g"]) > -40, "fm2 through swirl");
  await checksClean(page);
  await page.close();
});

test("a scenario run in the page matches opzsim's golden", { skip }, async () => {
  const page = await open("engines/kit", "none");
  await page.locator('[data-scenario="engines/kit/knobs.scn"]').click();
  await page.getByTestId("scenario-golden").waitFor({ timeout: 20_000 });
  assert.match(await page.getByTestId("scenario-golden").textContent() ?? "", /matches opzsim's golden/);
  assert.match(await page.getByTestId("scenario-exit").textContent() ?? "", /exit 0/);
  await page.close();
});
