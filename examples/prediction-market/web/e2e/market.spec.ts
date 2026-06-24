import { test, expect } from "@playwright/test";

// The smoke test for the browser UI: two "tabs" (pages) on the SAME market,
// betting concurrently, and the price moving live in both with no coordination.
// This is the headless-browser gate for the UI the demo's pitch rests on.
test("two tabs trade concurrently on one market; prices move live", async ({ browser }) => {
  // Tab A loads the app and creates the market POV (it lands in the URL hash).
  const a = await browser.newPage();
  await a.goto("/");
  await expect(a.locator("#question")).toContainText("Will Orly ship");
  await a.waitForFunction(() => location.hash.includes("pov="), null, { timeout: 15000 });
  await expect(a.locator("#status")).toHaveText("live", { timeout: 15000 });
  await expect(a.locator("#pool")).toHaveText("$0");

  // Tab B opens the SAME shared link (#pov=...) -> same market.
  const b = await browser.newPage();
  await b.goto(a.url());
  await expect(b.locator("#status")).toHaveText("live", { timeout: 15000 });

  // Concurrent bets: A buys YES $10, B buys NO $10.
  await a.getByRole("button", { name: /Buy YES/ }).click();
  await b.getByRole("button", { name: /Buy NO/ }).click();

  // Both bets land (no lost write): pool = $20 in BOTH tabs.
  await expect(a.locator("#pool")).toHaveText("$20", { timeout: 8000 });
  await expect(b.locator("#pool")).toHaveText("$20", { timeout: 8000 });

  // Tab A sees Tab B's NO bet reflected in the live price (50/50), via the poll
  // -- i.e. concurrent writes from another session show up with no coordination.
  await expect(a.locator("#market")).toContainText(/YES.*50\.0%/, { timeout: 8000 });
  await expect(a.locator("#market")).toContainText(/NO.*50\.0%/, { timeout: 8000 });
});
