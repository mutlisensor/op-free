/**
 * End to end in headless Chrome. Each SDK template and swirl, fm2 and kit load into their track and the computer
 * keyboard plays them; MIDI (the OP-Z as a controller) plays each track on its own channel and its CCs move the knobs,
 * MIDI learn binds any CC and survives a reload, the virtual OP-Z sends what the device sends; an effect sounds on a generator with no synth; a synth through FX1 and FX2 follows the send
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
  await page.locator(`body[data-track-loaded="${synth}"][data-fx1-loaded="${fx1}"][data-fx2-loaded="${fx2}"]`)
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
  await page.locator(`.knob-dial[data-param="${param}"]:visible`).first().focus();
  await page.keyboard.press(up ? "End" : "Home");
  await page.locator("body").focus();
}

async function checksClean(page: Page): Promise<void> {
  await page.waitForTimeout(200);
  for (const id of ["st-state-track", "st-state-fx1", "st-state-fx2", "st-add-track"]) {
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
    assert.match(await page.locator("#st-cpu-track").textContent() ?? "", /not Blackfin cycles/);
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

// ---- the OP-Z as a controller (src/midimap.ts) ------------------------------------------------------------------

type Slots = Record<string, { id: string; values: number[] }>;
interface Peaks { [slot: string]: number }
const midi = (page: Page, bytes: number[]) =>
  page.evaluate((b) => (window as unknown as { websim: { midiIn(b: number[]): void } }).websim.midiIn(b), bytes);
const slotsOf = (page: Page) => page.evaluate(() => (window as unknown as { websim: { slots(): Slots } }).websim.slots());
const selected = (page: Page) => page.evaluate(() => (window as unknown as { websim: { selected(): string } }).websim.selected());

/** The page with kit on kick and fm2 on lead (no effects), audio on. */
async function openTwo(track = "kick"): Promise<Page> {
  const page = await browser.newPage();
  page.on("pageerror", (e) => assert.fail(`page error: ${e.message}`));
  await page.goto(`${url}?kick=engines/kit&lead=engines/fm2&fx1=none&fx2=none&track=${track}`);
  await page.getByTestId("audio").click();
  await page.locator('body[data-kick-loaded="engines/kit"][data-lead-loaded="engines/fm2"][data-fx1-loaded="none"]')
    .waitFor({ timeout: 15_000 });
  await page.waitForFunction(() => !!(window as unknown as { websim: { state: { lastStats: unknown } } }).websim.state.lastStats);
  return page;
}

/** Each track's loudest sample over a while (from the worklet's per-track peaks). */
async function trackPeaks(page: Page, ms = 500): Promise<Peaks> {
  const best: Peaks = {};
  const end = Date.now() + ms;
  while (Date.now() < end) {
    const p = await page.evaluate(() => (window as unknown as { websim: { state: { lastStats: { peaks: Peaks } } } }).websim.state.lastStats.peaks);
    for (const [k, v] of Object.entries(p)) best[k] = Math.max(best[k] ?? 0, v);
    await page.waitForTimeout(40);
  }
  return best;
}

test("multitimbral: each MIDI channel plays its own track (ch 1 kick, ch 6 lead)", { skip }, async () => {
  const page = await openTwo();
  await page.waitForTimeout(300);
  await midi(page, [0x90, 53, 110]);                  // kick's first key, channel 1
  const kickOnly = await trackPeaks(page);
  await midi(page, [0x80, 53, 0]);
  assert.ok(kickOnly.kick > 0.01, `kick sounds on channel 1: ${kickOnly.kick}`);
  assert.ok(!(kickOnly.lead > 0), `lead silent: ${kickOnly.lead}`);
  await page.waitForTimeout(600);
  await midi(page, [0x95, 60, 110]);                  // channel 6: lead
  const leadOnly = await trackPeaks(page);
  await midi(page, [0x85, 60, 0]);
  assert.ok(leadOnly.lead > 0.01, `lead sounds on channel 6: ${leadOnly.lead}`);
  assert.match(await page.getByTestId("midi-monitor").textContent() ?? "", /ch 6 note off 60/);
  // nothing on channel 3 (perc is empty): no sound, no error
  await midi(page, [0x92, 60, 110]);
  await midi(page, [0x82, 60, 0]);
  await checksClean(page);
  await page.close();
});

test("an OP-Z dial moves the matching knob and engine byte, and the page follows its track", { skip }, async () => {
  const page = await openTwo("kick");
  // lead, page 1 dial 1 (P1) on channel 6: byte 255, the page follows to lead
  await midi(page, [0xb5, 1, 127]);
  assert.equal((await slotsOf(page)).lead.values[0], 255);
  assert.equal(await selected(page), "lead");
  assert.equal(await page.locator('#slot-lead .knob-dial[data-param="0"]').getAttribute("aria-valuenow"), "255");
  // kick, page 4 dial 4 (level) on channel 1, with follow off: the byte moves, the selection stays
  await page.locator("#midi-follow").uncheck();
  await midi(page, [0xb0, 16, 0]);
  assert.equal((await slotsOf(page)).kick.values[11], 0);
  assert.equal(await selected(page), "lead");
  // the lead's dial pages: filter (CC 3), attack (CC 5), LFO depth (CC 9), FX1 send (CC 13)
  for (const [cc, byte] of [[3, 8], [5, 2], [9, 13], [13, 6]]) {
    await midi(page, [0xb5, cc, 64]);
    assert.equal((await slotsOf(page)).lead.values[byte], 128, `CC ${cc} → byte ${byte}`);
  }
  // a CC the OP-Z map does not know is shown, not applied
  await midi(page, [0xb5, 74, 1]);
  assert.match(await page.getByTestId("midi-monitor").textContent() ?? "", /CC 74 = 1 → not an OP-Z parameter CC/);
  await page.close();
});

test("MIDI learn binds a CC to a knob and survives a reload", { skip }, async () => {
  const page = await openTwo("lead");
  await page.evaluate(() => localStorage.clear());
  await page.getByTestId("midi-learn").click();
  const res = page.locator('#slot-lead .knob-dial[data-param="9"]');           // resonance
  await res.click();
  assert.equal(await page.locator("#slot-lead .knob.armed").count(), 1);
  await midi(page, [0xb2, 74, 127]);                                           // a CC on channel 3
  assert.equal((await slotsOf(page)).lead.values[9], 255);
  assert.equal(await res.getAttribute("aria-valuenow"), "255");
  assert.match(await page.locator('#slot-lead .knob:has(.knob-dial[data-param="9"]) .knob-cc').textContent() ?? "", /ch 3 CC 74/);
  await page.getByTestId("midi-learn").click();                                // finish learning
  await midi(page, [0xb2, 74, 0]);
  assert.equal((await slotsOf(page)).lead.values[9], 0);
  // after a reload the binding is still there (browser storage), and the default map still works
  await page.reload();
  await page.getByTestId("audio").click();
  await page.locator('body[data-lead-loaded="engines/fm2"]').waitFor({ timeout: 15_000 });
  await midi(page, [0xb2, 74, 64]);
  assert.equal((await slotsOf(page)).lead.values[9], 128);
  await midi(page, [0xb5, 2, 127]);
  assert.equal((await slotsOf(page)).lead.values[1], 255);
  await page.getByTestId("midi-forget").click();
  await midi(page, [0xb2, 74, 0]);
  assert.equal((await slotsOf(page)).lead.values[9], 128, "forgotten: CC 74 does nothing");
  await page.close();
});

test("the virtual OP-Z sends what the device sends: dial 1 of page 1 on lead = CC 1 on channel 6", { skip }, async () => {
  const page = await openTwo("lead");
  await page.getByTestId("vopz-toggle").click();
  await page.getByTestId("vopz-track").selectOption("5");
  await page.locator('.vopz-pages button[data-page="0"]').click();
  await knobTo(page, "vopz-0", true);
  assert.match(await page.getByTestId("midi-monitor").textContent() ?? "", /ch 6 CC 1 = 127 → lead P1 255/);
  assert.equal((await slotsOf(page)).lead.values[0], 255);
  // page 2 dial 1 on kick = CC 5 on channel 1 = kick's attack
  await page.getByTestId("vopz-track").selectOption("0");
  await page.locator('.vopz-pages button[data-page="1"]').click();
  await knobTo(page, "vopz-0", true);
  assert.equal((await slotsOf(page)).kick.values[2], 255);
  await page.getByTestId("vopz-note").click();
  assert.ok((await trackPeaks(page, 400)).kick > 0.01, "the virtual OP-Z's note plays kick");
  await page.close();
});
