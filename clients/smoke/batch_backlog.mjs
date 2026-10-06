/** Batch-backlog smoke (#628); run by run-batch-backlog.sh.
 *
 * K writers send batches of BATCH writes to one shared safe POV. A batch is one update with an
 * entry per write, and a POV's memory merge copies its whole unpromoted backlog, so the writer
 * backpressure must cap that backlog in entries, not just updates. Capped only in updates (#586),
 * the backlog of 200-write batches held most of the Update Entry pool: the merges and Tetris ran
 * out of entries (pool misses), and writers were refused at the reserve.
 *
 * With orlyi's memory reserve at RESERVE_PCT, this requires, over SECS seconds:
 *   - no write refused and no write failing, and at least MIN_BATCHES batches through;
 *   - no Update or Update Entry pool miss (the reporting port's Memory Admission line);
 *   - peak Update Entry use, polled once a second, below half the pool. */

import net from "node:net";
import { connect } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL, RPT = +process.env.REPORT_PORT;
const K = +(process.env.K ?? 8);
const BATCH = +(process.env.BATCH ?? 200);
const KEYS = +(process.env.KEYS ?? 50000);
const SECS = +(process.env.SECS ?? 20);
const MIN_BATCHES = +(process.env.MIN_BATCHES ?? 200);
const STALL_S = +(process.env.STALL_S ?? 20);

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

let batches = 0, stop = false, error = null;
const batch = (w, i) => Array.from({ length: BATCH }, (_, j) => ({ n: (w * 7919 + i * BATCH + j) % KEYS, x: i }));
const writers = Array.from({ length: K }, async (_, w) => {
  const c = await connect(URL);
  await c.newSession();
  for (let i = 0; !stop; ++i) {
    try {
      await withTimeout(c.callBatch(pov, "sample", "write_val", batch(w, i)), STALL_S, "a batch");
      ++batches;
    } catch (err) {
      if (!stop) { stop = true; error = err?.message ?? String(err); }
      return;
    }
  }
  c.close();
});

let peak = 0, max = 0, line = "";
const t0 = Date.now();
while (!stop && (Date.now() - t0) / 1000 < SECS) {
  await sleep(1000);
  const body = await report();
  if (!body) fail("the reporting port didn't answer");
  const m = body.match(/Update Entry pool (\d+) \/ (\d+)/);
  if (m) { peak = Math.max(peak, +m[1]); max = +m[2]; }
  line = (body.match(/^Memory Admission = .*$/m) || [""])[0];
}
stop = true;
await Promise.race([Promise.allSettled(writers), sleep(STALL_S * 1000)]);
console.log(`${batches} batches of ${BATCH} in ${SECS}s; peak Update Entry use ${peak} / ${max}`);
console.log(line);
if (error) fail(`a batch failed: ${error}`);
if (batches < MIN_BATCHES) fail(`only ${batches} batches went through`);
const misses = [...line.matchAll(/misses (\d+)/g)].map((m) => +m[1]);
if (misses.length !== 2) fail(`couldn't read the pool misses from: ${line}`);
if (misses.some((n) => n > 0)) fail(`the merges or Tetris ran out of pool (misses ${misses.join(", ")})`);
if (!max || peak * 2 >= max) fail(`the Update Entry pool peaked at ${peak} of ${max}`);
setup.close();
console.log("BATCH BACKLOG OK");
process.exit(0);
