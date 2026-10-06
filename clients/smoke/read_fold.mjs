/** Read-heavy smoke (#674); run by run-read-fold.sh.
 *
 * Writes NKEYS `+=` counters ROUNDS times, one batch per round with a pause between, so each
 * counter's history spreads over memory layers and disk files. Then R readers, each on its own
 * connection and shared POV, read for SECS seconds: point reads of random counters, and every
 * tenth call a range read that sums them all. Every read is checked against what was written.
 * Prints METRIC lines for tools/maint/ab_bench.py. */

import { connect } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const NKEYS = +(process.env.NKEYS ?? 200);
const ROUNDS = +(process.env.ROUNDS ?? 40);
const PAUSE_MS = +(process.env.PAUSE_MS ?? 250);
const R = +(process.env.R ?? 4);
const SECS = +(process.env.SECS ?? 20);

const setup = await connect(URL);
await setup.newSession();
await setup.install("read_fold", 1);
const writer = await setup.newPov({ safe: true, shared: true });
const keys = Array.from({ length: NKEYS }, (_, n) => ({ n }));
for (let round = 0; round < ROUNDS; ++round) {
  await setup.callBatch(writer, "read_fold", "bump", keys);
  await new Promise((r) => setTimeout(r, PAUSE_MS));
}
console.log(`wrote ${NKEYS} counters x ${ROUNDS} rounds`);

const failures = [];
let points = 0, ranges = 0, stop = false;
const reader = async (id) => {
  const c = await connect(URL);
  await c.newSession();
  const pov = await c.newPov({ safe: true, shared: true });
  for (let i = 0; !stop && failures.length === 0; ++i) {
    try {
      if (i % 10 === 9) {
        const got = Number(await c.call(pov, "read_fold", "total", { last: NKEYS - 1 }));
        if (got !== NKEYS * ROUNDS) failures.push(`reader ${id}: total read ${got}, want ${NKEYS * ROUNDS}`);
        ++ranges;
      } else {
        const n = Math.floor(Math.random() * NKEYS);
        const got = Number(await c.call(pov, "read_fold", "count", { n }));
        if (got !== ROUNDS) failures.push(`reader ${id}: count ${n} read ${got}, want ${ROUNDS}`);
        ++points;
      }
    } catch (err) {
      failures.push(`reader ${id}: ${err?.message ?? err}`);
    }
  }
  c.close();
};
const t0 = Date.now();
const readers = Array.from({ length: R }, (_, id) => reader(id));
await new Promise((r) => setTimeout(r, SECS * 1000));
stop = true;
await Promise.all(readers);
const elapsed = (Date.now() - t0) / 1000;
setup.close();
console.log(`${points} point reads and ${ranges} range reads of ${NKEYS} keys in ${elapsed.toFixed(1)}s`);
console.log(`METRIC point_reads_per_s ${(points / elapsed).toFixed(1)}`);
console.log(`METRIC range_reads_per_s ${(ranges / elapsed).toFixed(1)}`);
if (failures.length) {
  console.error(`READ FOLD FAIL:\n  ${failures.slice(0, 10).join("\n  ")}`);
  process.exit(1);
}
console.log("read fold smoke: ok");
process.exit(0);
