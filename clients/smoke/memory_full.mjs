/** Memory-full smoke (#607); run by run-memory-full.sh.
 *
 * K writers send batches of BATCH writes (one update of BATCH entries each) to one shared safe
 * POV, against an orlyi whose update pools are forced small, while a reader reads the same POV
 * every 200 ms. The batches fill the Update Entry pool within a second or two. orlyi must then
 * refuse writes with status "insufficient_memory" instead of failing them with bad_alloc (or
 * crashing), keep serving reads, and accept writes again once the merges have freed the pools.
 * Keys cycle through KEYS, so the data on disk stays small however long the run is.
 *
 * Checks, in order:
 *   - writes are refused with "insufficient_memory", and every other write succeeds or is
 *     refused that way (no other error, and no write that hangs for STALL_S);
 *   - writes keep going through: the run makes at least MIN_WRITES batches;
 *   - every read succeeds, none taking STALL_S;
 *   - after IDLE_SECS with no writes, a write is accepted (retrying refusals for up to 30 s),
 *     and a new connection can open a session and read.
 * run-memory-full.sh also requires orlyi to be alive with no abort in its log. */

import { connect, InsufficientMemoryError } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const K = +(process.env.K ?? 8);
const BATCH = +(process.env.BATCH ?? 200);
const KEYS = +(process.env.KEYS ?? 50000);
const SECS = +(process.env.SECS ?? 30);
const MIN_WRITES = +(process.env.MIN_WRITES ?? 100);
const IDLE_SECS = +(process.env.IDLE_SECS ?? 5);
const STALL_S = +(process.env.STALL_S ?? 20);

const withTimeout = (p, secs, what) =>
  Promise.race([p, new Promise((_, rej) => setTimeout(() => rej(new Error(`${what} took over ${secs}s`)), secs * 1000))]);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const fail = (msg) => { console.error(`MEMORY FULL FAIL: ${msg}`); process.exit(1); };
const isRefusal = (err) => err instanceof InsufficientMemoryError;
const batch = (w, i) => Array.from({ length: BATCH }, (_, j) => ({ n: (w * 7919 + i * BATCH + j) % KEYS, x: i }));

const setup = await connect(URL);
await setup.newSession();
await setup.install("sample", 1);
const pov = await setup.newPov({ safe: true, shared: true });
await setup.call(pov, "sample", "write_val", { n: 0, x: 0 });

let writes = 0, refused = 0, stop = false, error = null, first_refusal = null;
const writers = Array.from({ length: K }, async (_, w) => {
  const c = await connect(URL);
  await c.newSession();
  for (let i = 0; !stop; ++i) {
    try {
      await withTimeout(c.callBatch(pov, "sample", "write_val", batch(w, i)), STALL_S, "a write");
      ++writes;
    } catch (err) {
      if (isRefusal(err)) {
        if (!refused++) {
          first_refusal = err.reply.result;
        }
        await sleep(20);
        continue;
      }
      if (!stop) { stop = true; error = err?.message ?? String(err); }
      return;
    }
  }
  c.close();
});

let reads = 0, read_error = null, slowest_read = 0;
const reader = (async () => {
  const c = await connect(URL);
  await c.newSession();
  while (!stop) {
    const t = Date.now();
    try {
      await withTimeout(c.call(pov, "sample", "read_val", { n: 0 }), STALL_S, "a read");
      ++reads;
      slowest_read = Math.max(slowest_read, Date.now() - t);
    } catch (err) {
      if (!stop) { stop = true; read_error = err?.message ?? String(err); }
      return;
    }
    await sleep(200);
  }
  c.close();
})();

const t0 = Date.now();
while (!stop && (Date.now() - t0) / 1000 < SECS) {
  await sleep(1000);
  const secs = Math.round((Date.now() - t0) / 1000);
  if (secs % 5 === 0) console.log(`  t=${secs}s batches=${writes} refused=${refused} reads=${reads}`);
}
stop = true;
await Promise.race([Promise.allSettled([...writers, reader]), sleep(STALL_S * 1000)]);
console.log(`write phase ended: ${writes} batches of ${BATCH}, ${refused} refused; ${reads} reads, slowest ${slowest_read} ms`);
if (first_refusal) console.log(`first refusal: ${first_refusal}`);

if (error) {
  fail(`a write failed other than by refusal: ${error}`);
}
if (read_error) {
  fail(`a read failed: ${read_error}`);
}
if (!refused) {
  fail(`no write was refused (${writes} batches in ${SECS}s); the smoke checked nothing`);
}
if (writes < MIN_WRITES) {
  fail(`only ${writes} batches got through in ${SECS}s; writes stalled behind the refusals`);
}

console.log(`idle ${IDLE_SECS}s`);
await sleep(IDLE_SECS * 1000);

/* The merges have had time to free the pools, so a write must be accepted again. Refusals are
   allowed while they finish, for up to 30 s. */
let accepted = false;
for (const deadline = Date.now() + 30_000; !accepted && Date.now() < deadline;) {
  try {
    await withTimeout(setup.call(pov, "sample", "write_val", { n: 1, x: -1 }), 30, "the write");
    accepted = true;
  } catch (err) {
    if (!isRefusal(err)) {
      fail(`a write after the idle failed other than by refusal: ${err?.message ?? err}`);
    }
    await sleep(500);
  }
}
if (!accepted) {
  fail("writes were still refused 30 s after the writers stopped");
}
console.log("WRITE AFTER IDLE: accepted");
try {
  const c = await withTimeout(connect(URL), 30, "connect");
  await withTimeout(c.newSession(), 30, "a new session");
  const x = await withTimeout(c.call(pov, "sample", "read_val", { n: 1 }), 30, "the read");
  if (x !== -1) {
    fail(`read back ${JSON.stringify(x)} for the write after the idle, not -1`);
  }
  console.log("NEW SESSION READ: ok");
  c.close();
} catch (err) {
  fail(`new session after the idle: ${err?.message ?? err}`);
}
setup.close();
console.log("MEMORY FULL OK");
process.exit(0);
