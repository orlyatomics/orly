/** Scan-yield smoke (#1015); run by run-scan-yield.sh against an orlyi with one fast runner.
 *
 * Loads ROWS rows, times a scan over all of them alone, then TRIALS times starts that scan on one
 * WebSocket session and, a quarter of the way into it, a point read on another. The point read
 * must answer before the scan does every time: a fiber doesn't give way while it computes, so
 * unless the scan yields (--read_yield_us), the point read waits for the whole scan behind it
 * on the only fast runner. Prints the timings, and METRIC lines for tools/maint/ab_bench.py. */

import { connect } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const ROWS = +(process.env.ROWS ?? 100000);
const TRIALS = +(process.env.TRIALS ?? 5);
/* Below this a scan is too short to tell a yield from a point read that merely got there first. */
const MIN_SCAN_MS = +(process.env.MIN_SCAN_MS ?? 20);
const fail = (msg) => {
  console.error(`SCAN YIELD FAIL: ${msg}`);
  process.exit(1);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const scanner = await connect(URL);
await scanner.newSession();
await scanner.install("scan_yield", 1);
const pov = await scanner.newPov({ safe: true, shared: true });
for (let i = 0; i < ROWS; i += 500) {
  const batch = Array.from({ length: Math.min(500, ROWS - i) }, (_, k) => ({ n: i + k }));
  for (let tries = 0; ; ++tries) {
    try {
      await scanner.callBatch(pov, "scan_yield", "put", batch);
      break;
    } catch (err) {
      if (err?.name !== "InsufficientMemoryError" || tries >= 600) {
        fail(`loading: ${String(err?.reply?.status ?? err?.message ?? err).slice(0, 300)}`);
      }
      await sleep(100);
    }
  }
}
const reader = await connect(URL);
await reader.newSession();

const scan = async () => {
  const got = Number(await scanner.call(pov, "scan_yield", "count_all", {}));
  if (got !== ROWS) fail(`the scan counted ${got} rows, want ${ROWS}`);
};
const point = async () => {
  const n = Math.floor(Math.random() * ROWS);
  const got = Number(await reader.call(pov, "scan_yield", "get", { n }));
  if (got !== n) fail(`get ${n} read ${got}`);
};

/* Warm up, then time the scan alone. */
await scan();
await point();
const alone = [];
for (let i = 0; i < 3; ++i) {
  const t0 = performance.now();
  await scan();
  alone.push(performance.now() - t0);
}
const scanMs = alone.sort((a, b) => a - b)[1];
console.log(`loaded ${ROWS} rows; the scan alone takes ${scanMs.toFixed(1)} ms`);
if (scanMs < MIN_SCAN_MS) fail(`the scan takes only ${scanMs.toFixed(1)} ms, too short to test; raise ROWS`);

const late = [];
const pointMs = [];
for (let trial = 0; trial < TRIALS; ++trial) {
  const t0 = performance.now();
  let scanDone = 0, pointDone = 0;
  const s = scan().then(() => { scanDone = performance.now() - t0; });
  await sleep(scanMs / 4);
  const p0 = performance.now();
  const p = point().then(() => { pointDone = performance.now() - t0; pointMs.push(performance.now() - p0); });
  await Promise.all([s, p]);
  console.log(`trial ${trial}: point read answered at ${pointDone.toFixed(1)} ms, the scan at ${scanDone.toFixed(1)} ms`);
  if (pointDone >= scanDone) late.push(trial);
}
pointMs.sort((a, b) => a - b);
console.log(`METRIC scan_yield_scan_ms ${scanMs.toFixed(3)}`);
console.log(`METRIC scan_yield_point_max_ms ${pointMs[pointMs.length - 1].toFixed(3)}`);
scanner.close();
reader.close();
if (late.length) fail(`the point read answered after the scan in ${late.length} of ${TRIALS} trials`);
console.log(`SCAN YIELD OK: the point read answered before the scan in all ${TRIALS} trials`);
