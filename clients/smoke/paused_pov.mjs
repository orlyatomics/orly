/** Paused-POV smoke (#626); run by run-paused-pov.sh.
 *
 * A paused POV's writes are not promoted to its parent until it is unpaused, so its memtable
 * backlog only grows. The writer backpressure (#586) used to wait for that backlog to drain
 * below the cap (the Update pool / 32, 156 here), so every write past the cap hung for good,
 * and a reader on the same POV stalled with them. Now a write to a paused POV whose backlog has
 * reached the cap is refused with "insufficient_memory", and other POVs carry on.
 *
 * K writers write distinct keys to one paused shared safe POV while a reader reads it every
 * 200 ms. Checks, in order:
 *   - no write hangs for STALL_S, and each succeeds or is refused with "insufficient_memory";
 *   - the paused POV takes writes up to the cap, then refuses them, and accepts no more than
 *     the cap plus one write per writer (each can pass the check once before the others
 *     commit);
 *   - every read succeeds, none taking STALL_S;
 *   - meanwhile a write to another POV is accepted and reads back, so the paused POV hasn't
 *     filled the pools that every writer shares.
 * Unpausing isn't exercised: it crashes orlyi on master when it has to start a Tetris player. */

import { connect, InsufficientMemoryError } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const K = +(process.env.K ?? 8);
const SECS = +(process.env.SECS ?? 20);
const CAP = +(process.env.CAP ?? 156);
const STALL_S = +(process.env.STALL_S ?? 10);

const withTimeout = (p, secs, what) =>
  Promise.race([p, new Promise((_, rej) => setTimeout(() => rej(new Error(`${what} took over ${secs}s`)), secs * 1000))]);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const fail = (msg) => { console.error(`PAUSED POV FAIL: ${msg}`); process.exit(1); };
const isRefusal = (err) => err instanceof InsufficientMemoryError;

const setup = await connect(URL);
await setup.newSession();
await setup.install("sample", 1);
const pov = await setup.newPov({ safe: true, shared: true });
/* The statement the server accepts; the clients' own pause helpers are #625. */
const paused = await setup.send(`pause {${pov}};`);
if (paused !== "paused") fail(`pause replied ${JSON.stringify(paused)}`);
await setup.call(pov, "sample", "write_val", { n: -1, x: 42 });

let writes = 0, refused = 0, stop = false, error = null, first_refusal = null;
const writers = Array.from({ length: K }, async (_, w) => {
  const c = await connect(URL);
  await c.newSession();
  for (let i = 0; !stop; ++i) {
    try {
      await withTimeout(c.call(pov, "sample", "write_val", { n: w * 10_000_000 + i, x: i }), STALL_S, "a write");
      ++writes;
    } catch (err) {
      if (isRefusal(err)) {
        if (!refused++) first_refusal = String(err.reply?.result);
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
      const x = await withTimeout(c.call(pov, "sample", "read_val", { n: -1 }), STALL_S, "a read");
      if (x !== 42) throw new Error(`read ${JSON.stringify(x)}, not 42`);
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
  if (secs % 5 === 0) console.log(`  t=${secs}s writes=${writes} refused=${refused} reads=${reads}`);
}
stop = true;
await Promise.race([Promise.allSettled([...writers, reader]), sleep(STALL_S * 1000)]);
console.log(`write phase ended: ${writes} writes, ${refused} refused; ${reads} reads, slowest ${slowest_read} ms`);

if (error) fail(`a write failed other than by refusal: ${error} (after ${writes} writes)`);
if (read_error) fail(`a read failed: ${read_error}`);
if (!refused) fail(`no write to the paused POV was refused (${writes} accepted in ${SECS}s)`);
console.log(`first refusal: ${first_refusal}`);
if (!/paused/.test(first_refusal)) fail("the refusal doesn't say the POV is paused");
/* +1: the write before the writers start */
if (writes + 1 < CAP) fail(`only ${writes} writes went through before the refusals; the cap is ${CAP}`);
if (writes + 1 > CAP + K) fail(`${writes} writes went through; the cap is ${CAP}, plus at most ${K} in flight`);
console.log(`PAUSED POV CAPPED: ${writes + 1} updates held, ${refused} writes refused`);

try {
  const other = await withTimeout(setup.newPov({ safe: true, shared: true }), 30, "a new POV");
  await withTimeout(setup.call(other, "sample", "write_val", { n: 1, x: -1 }), 30, "a write to another POV");
  const x = await withTimeout(setup.call(other, "sample", "read_val", { n: 1 }), 30, "a read of it");
  if (x !== -1) fail(`read back ${JSON.stringify(x)} from the other POV, not -1`);
  console.log("OTHER POV WRITE: accepted");
} catch (err) {
  fail(`a write to another POV, while the paused one is full: ${err?.message ?? err}`);
}
setup.close();
console.log("PAUSED POV OK");
process.exit(0);
