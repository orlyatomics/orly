/** Idle point-read latency smoke (#764); run by run-point-read.sh.
 *
 * Writes one `+=` counter, waits until a fresh POV reads it back, then one session reads that key
 * READS times, one call at a time, on an otherwise idle server. Each read hops between fiber
 * runners, so this is what an idle runner's wakeup costs: when idle runners polled with 10 us
 * sleeps, every hop paid the host's timer granularity (~6 ms per read in Docker on macOS).
 * Every read is checked; with MAX_P50_US set, a slower median fails the run. Prints METRIC lines
 * for tools/maint/ab_bench.py. */

import { connect } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const READS = +(process.env.READS ?? 400);
const WARMUP = +(process.env.WARMUP ?? 20);
/* Optional ceiling on the median, in microseconds. A runner wakeup that got lost would leave the
   read waiting out the park's 100 ms safety net. */
const MAX_P50_US = process.env.MAX_P50_US ? +process.env.MAX_P50_US : null;

const c = await connect(URL);
await c.newSession();
await c.install("read_fold", 1);
const writer = await c.newPov({ safe: true, shared: true });
await c.call(writer, "read_fold", "bump", { n: 0 });

/* Wait for the write to promote to the global POV, which a new POV reads from. */
const deadline = Date.now() + 30000;
for (;;) {
  const reader = await c.newPov({ safe: true, shared: true });
  if (Number(await c.call(reader, "read_fold", "count", { n: 0 })) === 1) break;
  if (Date.now() > deadline) throw new Error("the write never became visible to a new POV");
  await new Promise((r) => setTimeout(r, 100));
}
const fresh = await c.newPov({ safe: true, shared: true });

const lat = [];
for (let i = 0; i < WARMUP + READS; ++i) {
  const t0 = process.hrtime.bigint();
  const got = Number(await c.call(fresh, "read_fold", "count", { n: 0 }));
  const us = Number(process.hrtime.bigint() - t0) / 1000;
  if (got !== 1) throw new Error(`read ${i} returned ${got}, want 1`);
  if (i >= WARMUP) lat.push(us);
}
c.close();

lat.sort((a, b) => a - b);
const pct = (p) => lat[Math.min(lat.length - 1, Math.floor(p * lat.length))];
console.log(`${READS} one-key reads, one at a time, on an idle server`);
console.log(`METRIC point_read_p10_us ${pct(0.1).toFixed(1)}`);
console.log(`METRIC point_read_p50_us ${pct(0.5).toFixed(1)}`);
console.log(`METRIC point_read_p90_us ${pct(0.9).toFixed(1)}`);
console.log(`METRIC point_read_p99_us ${pct(0.99).toFixed(1)}`);
if (MAX_P50_US !== null && pct(0.5) > MAX_P50_US) {
  console.error(`median read took ${pct(0.5).toFixed(0)} us, over MAX_P50_US=${MAX_P50_US}`);
  process.exit(1);
}
