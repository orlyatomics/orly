/** Scan-yield smoke (#1015); run by run-scan-yield.sh against an orlyi with one fast runner.
 *
 * Loads ROWS rows (doubling them, up to MAX_ROWS, until the scan takes MIN_SCAN_MS), times a scan
 * over all of them alone, then TRIALS times starts that scan on one
 * WebSocket session and, a quarter of the way into it, a point read on another. The point read
 * must answer before the scan does every time: a fiber doesn't give way while it computes, so
 * unless the scan yields (--read_yield_us), the point read waits for the whole scan behind it
 * on the only fast runner. Prints the timings, and METRIC lines for tools/maint/ab_bench.py. */

import { connect } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
let rows = +(process.env.ROWS ?? 100000);
const MAX_ROWS = +(process.env.MAX_ROWS ?? 1600000);
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
const load = async (from, to) => {
  for (let i = from; i < to; i += 500) {
    const batch = Array.from({ length: Math.min(500, to - i) }, (_, k) => ({ n: i + k }));
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
};
await load(0, rows);
const reader = await connect(URL);
await reader.newSession();

const scan = async () => {
  const got = Number(await scanner.call(pov, "scan_yield", "count_all", {}));
  if (got !== rows) fail(`the scan counted ${got} rows, want ${rows}`);
};
const point = async () => {
  const n = Math.floor(Math.random() * rows);
  const got = Number(await reader.call(pov, "scan_yield", "get", { n }));
  if (got !== n) fail(`get ${n} read ${got}`);
};

/* Warm up, then time the scan alone: the median of three. A fast host gets more rows, so the
 * scan stays long enough to test. */
const timeScan = async () => {
  await scan();
  await point();
  const alone = [];
  for (let i = 0; i < 3; ++i) {
    const t0 = performance.now();
    await scan();
    alone.push(performance.now() - t0);
  }
  return alone.sort((a, b) => a - b)[1];
};
let scanMs = await timeScan();
while (scanMs < MIN_SCAN_MS && rows * 2 <= MAX_ROWS) {
  console.log(`${rows} rows scan in ${scanMs.toFixed(1)} ms, under ${MIN_SCAN_MS} ms; doubling`);
  await load(rows, rows * 2);
  rows *= 2;
  scanMs = await timeScan();
}
console.log(`loaded ${rows} rows; the scan alone takes ${scanMs.toFixed(1)} ms`);
if (scanMs < MIN_SCAN_MS) fail(`the scan takes only ${scanMs.toFixed(1)} ms with ${rows} rows, too short to test; raise MAX_ROWS`);

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
