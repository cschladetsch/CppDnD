#!/usr/bin/env node
// Drives the browser client through the tips and the combat tutorial, headless.
// Usage: node web_smoke.cjs <path to dnd_server>     (needs the playwright package; CTest runs it only when it is installed)
"use strict";
const { chromium } = require("playwright");
const { spawn } = require("child_process");
const net = require("net");

const fail = msg => { console.error("FAIL: " + msg); process.exitCode = 1; throw new Error(msg); };
const ok = (cond, msg) => { if (!cond) fail(msg); console.log("ok   " + msg); };
const freePort = () => new Promise(r => { const s = net.createServer().listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => r(p)); }); });

(async () => {
  const port = await freePort(), base = `http://127.0.0.1:${port}`;
  const server = spawn(process.argv[2], ["--port", String(port), "--mock-llm", "--sessions", ""], { stdio: "ignore" });
  let browser, page;
  try {
    for (let i = 0; i < 100; i++) { try { if ((await fetch(base + "/api/health")).ok) break; } catch {} await new Promise(r => setTimeout(r, 100)); }
    const launch = process.env.PW_CHROMIUM ? { executablePath: process.env.PW_CHROMIUM } : {};
    browser = await chromium.launch(launch);
    page = await (await browser.newContext({ viewport: { width: 1280, height: 900 } })).newPage();
    page.on("dialog", d => d.accept());
    const errors = []; page.on("pageerror", e => errors.push(String(e)));
    const card = title => page.locator("#coach .coach", { has: page.locator("h3", { hasText: title }) });

    // ---- an ordinary game: the welcome tip, Close brings it back after a reload, "Do not show again" does not
    await page.goto(base + "/");
    await page.locator("button.primary", { hasText: "Begin" }).click();
    await card("First time?").waitFor();
    ok(true, "the welcome tip shows in a new game");
    await card("First time?").getByRole("button", { name: "Close" }).click();
    ok(await card("First time?").count() === 0, "Close hides it");
    await page.reload(); await page.waitForSelector("#log");
    await card("First time?").waitFor({ timeout: 5000 });
    ok(true, "after a reload a closed tip comes back");
    await card("First time?").getByRole("button", { name: "Do not show again" }).click();
    await page.reload(); await page.waitForSelector("#log"); await page.waitForTimeout(800);
    ok(await card("First time?").count() === 0, "after a reload a tip marked Do not show again stays hidden");

    // ---- hover help: native titles become the styled popover
    await page.locator("#helpBtn").hover();
    ok(await page.locator("#hovertip").isVisible() && /How to play/.test(await page.locator("#hovertip").textContent()), "hovering a button shows its help");

    // ---- the tutorial
    await page.locator("#learnBtn").click();   // from the header, in the middle of another game
    const tut = page.locator("#coach .coach.tut");
    await tut.locator("h3", { hasText: "Welcome to the Training Yard" }).waitFor();
    ok(true, "the tutorial starts on step 1");
    await page.locator("#scene button", { hasText: "practice ring" }).click();
    await tut.locator("h3", { hasText: "Who goes first" }).waitFor();
    await page.locator(".initstrip span").nth(4).waitFor({ timeout: 5000 });
    ok(true, "entering the ring starts the fight and shows the turn order");
    ok(await page.locator(".legend").isVisible(), "the grid has a legend");
    await tut.getByRole("button", { name: "Next" }).click();
    // step 3 waits for Kestrel's turn, then step 4 asks for a move
    await tut.locator("h3", { hasText: /^(Move|Attack)$/ }).waitFor({ timeout: 20000 });
    ok(await page.locator(".econ .pill").count() >= 4, "on your turn the budget shows as pills");
    const title = await tut.locator("h3").textContent();
    if (title === "Move") {
      await page.locator("#grid .cell.reach").first().click();
      await tut.locator("h3", { hasText: "Attack" }).waitFor({ timeout: 5000 });
      ok(true, "moving finishes the Move step");
    }
    // throw the javelin at whatever is in range: the result is explained in words, on the card and on the log line
    await page.locator("#combat button", { hasText: /^javelin/ }).click();
    await page.locator("#grid .cell.inrange").first().click();
    await tut.locator("h3", { hasText: "Reading the result" }).waitFor({ timeout: 5000 });
    const why = await tut.locator("p").first().textContent();
    ok(/rolled \d+ on the d20/.test(why) && /(hits|misses|always)/.test(why), "the attack is explained: " + why.split("\n")[0]);
    ok(/on the d20/.test(await page.locator("#log .attack").last().getAttribute("data-tip") || await page.locator("#log .attack").last().getAttribute("title") || ""), "the log line carries the same explanation");
    await tut.getByRole("button", { name: "Next" }).click();
    await tut.locator("h3", { hasText: "End your turn" }).waitFor({ timeout: 5000 });
    await page.locator("#combat button", { hasText: "End turn" }).click();
    await tut.locator("h3", { hasText: /^(Their turns|Throwing, range and cover)$/ }).waitFor({ timeout: 20000 });
    ok(true, "ending the turn moves the lesson on");
    await tut.getByRole("button", { name: "Skip tutorial" }).click();   // a confirm() pops up
    ok(true, "the tutorial can be skipped");
    ok(errors.length === 0, "no script errors: " + errors.join("; "));
  } catch (e) {
    if (!process.exitCode) { console.error(e); process.exitCode = 1; }
    if (page && process.env.WEB_SMOKE_SHOT) await page.screenshot({ path: process.env.WEB_SMOKE_SHOT, fullPage: true }).catch(() => {});
  } finally {
    if (browser) await browser.close();
    server.kill();
  }
})();
