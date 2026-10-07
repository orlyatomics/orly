/** Insert-order smoke (#754); run by run-insert-order.sh, once per MODE.
 *
 * Writes BATCHES batches of N new keys through one POV, the keys either increasing across the
 * whole run (MODE=ordered) or drawn at random (MODE=random), and times each batch. A memory layer
 * insert used to walk back from the layer's tail to its place, so a random batch got slower the
 * more the layer held while an ordered one stayed flat. Then reads back SAMPLE of the keys and
 * checks each one. Prints METRIC lines for tools/maint/ab_bench.py: keys written per second over
 * the whole run, and the median batch time over the last quarter of the run, when the layer is
 * largest. */

import { connect } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const MODE = process.env.MODE ?? "random";
const N = +(process.env.N ?? 4096);
const BATCHES = +(process.env.BATCHES ?? 32);
const SAMPLE = +(process.env.SAMPLE ?? 200);

if (MODE !== "ordered" && MODE !== "random") {
  console.error(`MODE must be ordered or random, not ${MODE}`);
  process.exit(2);
}

const c = await connect(URL);
await c.newSession();
await c.install("insert_order", 1);
const pov = await c.newPov({ safe: true, shared: true });

const written = [];
const seen = new Set();
let next = 0;
const batchKeys = () => {
  const keys = [];
  while (keys.length < N) {
    let k;
    if (MODE === "ordered") {
      k = next++;
    } else {
      k = Math.floor(Math.random() * Number.MAX_SAFE_INTEGER);
      if (seen.has(k)) continue;
    }
    seen.add(k);
    keys.push(k);
  }
  return keys;
};

const ms = [];
const t0 = Date.now();
for (let b = 0; b < BATCHES; ++b) {
  const keys = batchKeys();
  const t = performance.now();
  await c.callBatch(pov, "insert_order", "put", keys.map((k) => ({ k })));
  const took = performance.now() - t;
  ms.push(took);
  console.log(`${MODE} batch ${b} after ${b * N} keys: ${took.toFixed(1)} ms`);
  for (let i = 0; i < keys.length; i += Math.max(1, Math.floor(N * BATCHES / SAMPLE))) written.push(keys[i]);
}
const elapsed = (Date.now() - t0) / 1000;

const failures = [];
for (const k of written) {
  const got = Number(await c.call(pov, "insert_order", "get", { k }));
  if (got !== k) failures.push(`key ${k} read back ${got}`);
}
c.close();

const late = ms.slice(Math.floor(ms.length * 3 / 4)).sort((a, b) => a - b);
const median = late[Math.floor(late.length / 2)];
console.log(`${MODE}: ${N * BATCHES} keys in ${elapsed.toFixed(1)}s; ${written.length} read back`);
console.log(`METRIC ${MODE}_keys_per_s ${(N * BATCHES / elapsed).toFixed(1)}`);
console.log(`METRIC ${MODE}_late_batch_ms ${median.toFixed(1)}`);
if (failures.length) {
  console.error(`INSERT ORDER FAIL:\n  ${failures.slice(0, 10).join("\n  ")}`);
  process.exit(1);
}
console.log(`insert order smoke (${MODE}): ok`);
process.exit(0);
