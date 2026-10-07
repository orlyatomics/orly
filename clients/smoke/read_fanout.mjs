/** Point reads over many disk layers with a small frame pool (#762); run by run-read-fanout.sh.
 *
 * Writers write new keys into one shared safe POV, so into the global POV, until its repo has
 * LAYERS disk layers (the disk merges are off, so nothing folds them back down). Then R readers,
 * each on its own connection, read keys already written for SECS seconds. A single-key read
 * builds a sub-walker per disk layer on fiber frames, and R reads times LAYERS layers is far more
 * frames than --max_parallel_frames, which used to take orlyi down ("pure virtual method
 * called"). Every read must return the value written or, rarely (under 1%), be refused as
 * insufficient_memory when no frame comes free for a second; and orlyi must stay up. */

import net from "node:net";
import { connect, InsufficientMemoryError, InsufficientStorageError } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const REPORT_PORT = +process.env.ORLY_REPORT_PORT;
const LAYERS = +(process.env.LAYERS ?? 40);
const K = +(process.env.K ?? 8);
const BATCH = +(process.env.BATCH ?? 20);
const R = +(process.env.R ?? 128);
const SECS = +(process.env.SECS ?? 15);
const LOAD_SECS = +(process.env.LOAD_SECS ?? 240);

/* The reporter's headers end in bare \n, which node's http parser rejects. */
function report() {
  return new Promise((resolve) => {
    let body = "", done = false;
    const end = (v) => { if (!done) { done = true; sock.destroy(); resolve(v); } };
    const sock = net.connect(REPORT_PORT, "127.0.0.1", () => sock.write("GET / HTTP/1.0\r\n\r\n"));
    sock.on("data", (d) => { body += d; if (/^Update Entry Pool = .*\n/m.test(body)) end(body); });
    sock.on("error", () => end(null));
    sock.setTimeout(5000, () => end(null));
  });
}

async function diskLayers() {
  const body = await report();
  if (!body) return null;
  const m = body.match(/^Global Layers = disk (\d+)/m);
  return m ? +m[1] : null;
}

const failures = [];
const setup = await connect(URL);
await setup.newSession();
await setup.install("sample", 1);
const pov = await setup.newPov({ safe: true, shared: true });

/* Load: writer w writes keys w * 10M + [0, done[w]). */
let stop = false;
const done = new Array(K).fill(0);
const writer = async (w) => {
  const c = await connect(URL);
  await c.newSession();
  for (let i = 0; !stop; i += BATCH) {
    try {
      await c.callMany(pov, Array.from({ length: BATCH }, (_, j) => ["sample", "write_val", { n: w * 10_000_000 + i + j, x: i + j }]));
      done[w] = i + BATCH;
    } catch (err) {
      if (err instanceof InsufficientMemoryError || err instanceof InsufficientStorageError) {
        i -= BATCH;
        await new Promise((r) => setTimeout(r, 20));
        continue;
      }
      failures.push(`writer ${w}: ${err?.message ?? err}`);
      break;
    }
  }
  c.close();
};
const writers = Array.from({ length: K }, (_, w) => writer(w));
const t0 = Date.now();
let layers = 0;
while (failures.length === 0 && Date.now() - t0 < LOAD_SECS * 1000) {
  await new Promise((r) => setTimeout(r, 1000));
  const n = await diskLayers();
  if (n === null) {
    failures.push("reporting port stopped answering during the load");
    break;
  }
  layers = n;
  console.log(`  load t=${((Date.now() - t0) / 1000).toFixed(0)}s keys=${done.reduce((a, b) => a + b, 0)} disk_layers=${n}`);
  if (n >= LAYERS) break;
}
stop = true;
await Promise.all(writers);
if (!failures.length && layers < LAYERS) {
  failures.push(`only ${layers} disk layers after ${LOAD_SECS} s (want ${LAYERS}); the read phase would prove nothing`);
}
if (failures.length) {
  console.error(`READ FANOUT FAIL:\n  ${failures.join("\n  ")}`);
  process.exit(1);
}
console.log(`READ PHASE: ${R} readers over ${layers} disk layers for ${SECS} s`);

/* Read: R readers, each reading known keys back as fast as it can. */
let reads = 0, refused = 0, stop_reading = false;
const errors = new Map();
const reader = async (r) => {
  let c;
  try {
    c = await connect(URL);
    await c.newSession();
  } catch (err) {
    failures.push(`reader ${r} could not connect: ${err?.message ?? err}`);
    return;
  }
  let seed = 12345 + r;
  while (!stop_reading) {
    seed = (seed * 1103515245 + 12345) % 2147483648;
    const w = seed % K;
    const n = w * 10_000_000 + (Math.floor(seed / K) % done[w]);
    try {
      const v = await c.call(pov, "sample", "read_val", { n });
      if (v === null || v === undefined) {
        failures.push(`reader ${r}: read of ${n} found nothing`);
        break;
      }
      ++reads;
    } catch (err) {
      if (err instanceof InsufficientMemoryError) {
        /* A typed, retryable refusal: the statement waited a second for a frame and never
           ran (#762). Allowed, but only rarely (checked below). */
        ++refused;
        continue;
      }
      const msg = String(err?.message ?? err).slice(0, 200);
      errors.set(msg, (errors.get(msg) ?? 0) + 1);
      if (/closed|ECONNRESET|ECONNREFUSED|socket/i.test(msg)) break;
    }
  }
  try { c.close(); } catch { /* the server may be gone */ }
};
const readers = Array.from({ length: R }, (_, r) => reader(r));
const t1 = Date.now();
while (Date.now() - t1 < SECS * 1000) {
  await new Promise((r) => setTimeout(r, 1000));
  console.log(`  read t=${((Date.now() - t1) / 1000).toFixed(0)}s reads=${reads}`);
}
stop_reading = true;
await Promise.race([Promise.all(readers), new Promise((r) => setTimeout(r, 30000))]);
const after = await diskLayers();
if (after === null) failures.push("reporting port stopped answering after the reads");
for (const [msg, count] of errors) failures.push(`${count} reads failed: ${msg}`);
if (reads === 0) failures.push("no read succeeded");
if (refused > reads / 100) failures.push(`${refused} reads refused as insufficient_memory, over 1% of ${reads}`);
try { setup.close(); } catch { /* the server may be gone */ }
console.log(`${reads} reads (${(reads / SECS).toFixed(0)}/s), ${refused} refused as insufficient_memory, over ${layers} disk layers`);
console.log(`METRIC reads_per_s ${(reads / SECS).toFixed(1)}`);
if (failures.length) {
  console.error(`READ FANOUT FAIL:\n  ${failures.join("\n  ")}`);
  process.exit(1);
}
console.log("read fanout: ok");
process.exit(0);
