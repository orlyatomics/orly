/** Batch-backlog smoke (#628); run by run-batch-backlog.sh.
 *
 * K writers send batches of BATCH writes to one shared safe POV. A batch is one update with an
 * entry per write, and a POV's memory merge copies its whole unpromoted backlog, so the writer
 * backpressure must cap that backlog in entries, not just updates. Capped only in updates (#586),
 * the backlog of 200-write batches held most of the Update Entry pool, and writers were refused at
 * the reserve (before #629's copy claims, the merges and Tetris also ran out of entries).
 *
 * A refused batch (insufficient_memory) is retried after a short pause, as the client contract
 * says (#719): how much of the pools is in use swings with the runner's load, and a loaded CI
 * runner can briefly take the in-use blocks, the blocks promised to writes in flight and the
 * merges' copy claims past the admission line. The regression this smoke guards against is
 * different in kind: with the backlog capped only in updates, about one batch in two was
 * refused, and with no cap at all, more. Since memory admission waits for room before it
 * refuses (#765), an uncapped backlog's batches mostly wait instead, so the negative control
 * in CI requires the backlog check itself to fail.
 *
 * With orlyi's memory reserve at RESERVE_PCT, this requires, over SECS seconds:
 *   - no write failing other than by refusal, and at least MIN_BATCHES batches through;
 *   - at most MAX_REFUSED_PCT percent of the batches sent refused;
 *   - no Update or Update Entry pool miss (the reporting port's Memory Admission line);
 *   - the POV's backlog never past its cap, in entries or updates (#721; the reporting port's
 *     Writer Backlog line, which records the peak at every commit). The cap used to be checked
 *     after a write committed, and a writer that waited 5 s wrote anyway, so with K=8 the backlog
 *     reached 2-3 times its cap.
 *
 * The peak Update Entry use, polled once a second, is reported as a METRIC and is not a gate: a
 * merge's copies may use the memory reserve by design, and a faster engine legitimately runs the
 * writers up to the admission line (a master run peaked at 64%). The health signal for the pools
 * is the merges' own log line, which run-batch-backlog.sh requires to be absent: no
 * "StepMergeMem out of pool space; merge rolled back".
 *
 * It prints METRIC lines for tools/maint/ab_bench.py before it checks anything. */

import net from "node:net";
import { connect, InsufficientMemoryError } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL, RPT = +process.env.REPORT_PORT;
const K = +(process.env.K ?? 8);
const BATCH = +(process.env.BATCH ?? 200);
const KEYS = +(process.env.KEYS ?? 50000);
const SECS = +(process.env.SECS ?? 20);
const MIN_BATCHES = +(process.env.MIN_BATCHES ?? 200);
const STALL_S = +(process.env.STALL_S ?? 20);
const MAX_REFUSED_PCT = +(process.env.MAX_REFUSED_PCT ?? 1);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const fail = (msg) => { console.error(`BATCH BACKLOG FAIL: ${msg}`); process.exit(1); };
const withTimeout = (p, secs, what) =>
  Promise.race([p, new Promise((_, rej) => setTimeout(() => rej(new Error(`${what} took over ${secs}s`)), secs * 1000))]);

function report() {
  return new Promise((res) => {
    let body = "", done = false;
    const end = (v) => { if (!done) { done = true; sock.destroy(); res(v); } };
    const sock = net.connect(RPT, "127.0.0.1", () => sock.write("GET / HTTP/1.0\r\n\r\n"));
    sock.on("data", (d) => { body += d; if (/^Memory Admission = .*\n/m.test(body)) end(body); });
    sock.on("error", () => end(null));
    sock.setTimeout(3000, () => end(body || null));
  });
}

const setup = await connect(URL);
await setup.newSession();
await setup.install("sample", 1);
const pov = await setup.newPov({ safe: true, shared: true });

let batches = 0, refused = 0, stop = false, error = null, firstRefusal = null, firstAck = 0, lastAck = 0;
/* Each accepted batch's round trip, in ms (#765: a batch may now wait at admission). */
const latencies = [];
const batch = (w, i) => Array.from({ length: BATCH }, (_, j) => ({ n: (w * 7919 + i * BATCH + j) % KEYS, x: i }));
const writers = Array.from({ length: K }, async (_, w) => {
  const c = await connect(URL);
  await c.newSession();
  for (let i = 0; !stop; ++i) {
    try {
      const sent = performance.now();
      await withTimeout(c.callBatch(pov, "sample", "write_val", batch(w, i)), STALL_S, "a batch");
      latencies.push(performance.now() - sent);
      ++batches;
      lastAck = Date.now();
      firstAck ||= lastAck;
    } catch (err) {
      if (err instanceof InsufficientMemoryError) {
        ++refused;
        firstRefusal ??= err.message;
        await sleep(50);
        continue;
      }
      if (!stop) { stop = true; error = err?.message ?? String(err); }
      return;
    }
  }
  c.close();
});

let peak = 0, max = 0, line = "", backlogLine = "";
const t0 = Date.now();
while (!stop && (Date.now() - t0) / 1000 < SECS) {
  await sleep(1000);
  const body = await report();
  if (!body) fail("the reporting port didn't answer");
  const m = body.match(/Update Entry pool (\d+) \/ (\d+)/);
  if (m) { peak = Math.max(peak, +m[1]); max = +m[2]; }
  line = (body.match(/^Memory Admission = .*$/m) || [""])[0];
  backlogLine = (body.match(/^Writer Backlog = .*$/m) || [""])[0];
}
stop = true;
await Promise.race([Promise.allSettled(writers), sleep(STALL_S * 1000)]);
console.log(`${batches} batches of ${BATCH} in ${SECS}s, ${refused} refused; peak Update Entry use ${peak} / ${max}`);
console.log(line);
console.log(backlogLine || "no Writer Backlog line on the reporting port");
const bl = backlogLine.match(/peak (\d+) entries \/ cap (\d+); peak (\d+) updates \/ cap (\d+); stalled refusals (\d+)/);
const secs = lastAck > firstAck ? (lastAck - firstAck) / 1000 : SECS;
console.log(`METRIC batches_per_s ${(batches / secs).toFixed(1)}`);
console.log(`METRIC refused_pct ${(100 * refused / Math.max(1, batches + refused)).toFixed(2)}`);
console.log(`METRIC peak_entry_pool_pct ${max ? (100 * peak / max).toFixed(1) : 0}`);
latencies.sort((a, b) => a - b);
const pct = (q) => latencies.length ? latencies[Math.min(latencies.length - 1, Math.floor(latencies.length * q))] : 0;
console.log(`METRIC batch_p50_ms ${pct(0.5).toFixed(1)}`);
console.log(`METRIC batch_p99_ms ${pct(0.99).toFixed(1)}`);
/* Memory admission's wait (#765), when the server has one. */
const wt = line.match(/waited (\d+); wait timeouts (\d+); refused at stop \d+; longest wait (\d+) ms/);
if (wt) {
  console.log(`METRIC admission_waited ${wt[1]}`);
  console.log(`METRIC admission_wait_timeouts ${wt[2]}`);
  console.log(`METRIC admission_longest_wait_ms ${wt[3]}`);
}
if (bl) {
  console.log(`METRIC peak_backlog_entries ${bl[1]}`);
  console.log(`METRIC peak_backlog_updates ${bl[3]}`);
  console.log(`METRIC stalled_refusals ${bl[5]}`);
}
if (firstRefusal) console.log(`first refusal: ${firstRefusal.replace(/write_val \[.*\];/, "write_val [...];")}`);
/* Every check runs and every failure is printed, so one can't hide another: the negative
   control in CI must fail on the backlog cap itself (#765: with memory admission's wait, the
   control's batches wait instead of being refused, so the refused share no longer tells). */
const failures = [];
if (error) failures.push(`a batch failed: ${error}`);
if (batches < MIN_BATCHES) failures.push(`only ${batches} batches went through`);
if (refused * 100 > MAX_REFUSED_PCT * (batches + refused)) {
  failures.push(`${refused} of ${batches + refused} batches were refused, more than ${MAX_REFUSED_PCT}%`);
}
const misses = [...line.matchAll(/misses (\d+)/g)].map((m) => +m[1]);
if (misses.length !== 2) failures.push(`couldn't read the pool misses from: ${line}`);
else if (misses.some((n) => n > 0)) failures.push(`the merges or Tetris ran out of pool (misses ${misses.join(", ")})`);
if (!max) failures.push("couldn't read the Update Entry pool size from the reporting port");
if (!bl) failures.push(`couldn't read the writer backlog from: ${backlogLine}`);
else {
  if (+bl[1] > +bl[2]) failures.push(`the POV's backlog reached ${bl[1]} entries, past its cap of ${bl[2]}`);
  if (+bl[3] > +bl[4]) failures.push(`the POV's backlog reached ${bl[3]} updates, past its cap of ${bl[4]}`);
}
if (failures.length) {
  for (const f of failures) console.error(`BATCH BACKLOG FAIL: ${f}`);
  process.exit(1);
}
setup.close();
console.log("BATCH BACKLOG OK");
process.exit(0);
