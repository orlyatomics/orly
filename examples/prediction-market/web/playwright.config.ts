import { defineConfig } from "@playwright/test";

// The market UI + orlyi are started by run-e2e.sh; this just points the test
// at the served page. Chromium runs with --no-sandbox for CI/container envs.
export default defineConfig({
  testDir: "./e2e",
  timeout: 45_000,
  fullyParallel: false,
  retries: 0,
  reporter: [["list"]],
  use: {
    baseURL: process.env.E2E_BASE_URL || "http://localhost:8000",
    headless: true,
    launchOptions: { args: ["--no-sandbox", "--disable-setuid-sandbox"] },
  },
});
