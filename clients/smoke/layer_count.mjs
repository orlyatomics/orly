/** Disk file count under sustained writes, and what it does to reads (#701); run by
 * run-layer-count.sh.
 *
 * K writers write new keys (BATCH per transaction) into one shared POV, so into the global POV,
 * for SECS seconds or MAX_WRITES keys. Meanwhile one reader reads keys already written, one at a
 * time, and the reporting port is polled every SAMPLE_MS for the global repo's layers ("Global
 * Layers = disk N; memory M", absent before #701, when those metrics are left out). After the
 * writers stop, the reader goes on alone for QUIET_SECS against the files they left. Every key
 * is new, so the data, and the files the memory merges flush, keep growing, as in a bulk load.
 * Fails if a write errors or the server stops answering. */

import net from "node:net";
import { connect, InsufficientMemoryError, InsufficientStorageError } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const REPORT_PORT = +process.env.ORLY_REPORT_PORT;
const K = +(process.env.K ?? 8);
const SECS = +(process.env.SECS ?? 60);
const MAX_WRITES = +(process.env.MAX_WRITES ?? 400000);
/* Keys per write: a write of BATCH > 1 is one batched transaction of that many write_val calls. */
const BATCH = +(process.env.BATCH ?? 1);
/* How often to poll the reporting port. */
const SAMPLE_MS = +(process.env.SAMPLE_MS ?? 1000);
/* Seconds the reader keeps going once the writers stop, with nothing else running. */
const QUIET_SECS = +(process.env.QUIET_SECS ?? 5);

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

function percentile(sorted, p) {
  return sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * p))] : NaN;
}

const setup = await connect(URL);
await setup.newSession();
await setup.install("sample", 1);
/* A shared safe POV: its writes are promoted into the global POV, whose memory merges flush them
   to disk files. */
const pov = await setup.newPov({ safe: true, shared: true });

let writes = 0, refused = 0, stop = false, first_write = 0, last_write = 0;
const failures = [];
/* Keys written so far, by writer: writer w has written w * 10M + [0, done[w]). */
const done = new Array(K).fill(0);
const writer = async (w) => {
  const c = await connect(URL);
  await c.newSession();
  for (let i = 0; !stop && writes < MAX_WRITES; i += BATCH) {
    try {
      if (BATCH === 1) {
        await c.call(pov, "sample", "write_val", { n: w * 10_000_000 + i, x: i });
      } else {
        await c.callMany(pov, Array.from({ length: BATCH }, (_, j) => ["sample", "write_val", { n: w * 10_000_000 + i + j, x: i + j }]));
      }
      writes += BATCH;
      done[w] = i + BATCH;
      last_write = Date.now();
      if (!first_write) first_write = last_write;
    } catch (err) {
      if (err instanceof InsufficientMemoryError || err instanceof InsufficientStorageError) {
        ++refused;
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

/* Read latencies in ms: all of them while the writers run, those of the last third of that, and
   those after the writers stop. */
const reads = [], late_reads = [], quiet_reads = [];
let late_from = Infinity, quiet = false, stop_reading = false;
const reader = async () => {
  const c = await connect(URL);
  await c.newSession();
  let seed = 12345;
  while (!stop_reading) {
    seed = (seed * 1103515245 + 12345) % 2147483648;
    const w = seed % K;
    if (!done[w]) {
      await new Promise((r) => setTimeout(r, 10));
      continue;
    }
    const n = w * 10_000_000 + (Math.floor(seed / K) % done[w]);
    const t = process.hrtime.bigint();
    try {
      const v = await c.call(pov, "sample", "read_val", { n });
      const ms = Number(process.hrtime.bigint() - t) / 1e6;
      if (quiet) {
        quiet_reads.push(ms);
      } else {
        reads.push(ms);
        if (Date.now() >= late_from) late_reads.push(ms);
      }
      if (v === null || v === undefined) {
        failures.push(`read of ${n} found nothing`);
        break;
      }
    } catch (err) {
      failures.push(`reader: ${err?.message ?? err}`);
      break;
    }
  }
  c.close();
};

const writers = Array.from({ length: K }, (_, w) => writer(w));
const reading = reader();
const t0 = Date.now();
late_from = t0 + (SECS * 1000 * 2) / 3;
let peak = null, last = null;
while (Date.now() - t0 < SECS * 1000 && writes < MAX_WRITES && failures.length === 0) {
  await new Promise((r) => setTimeout(r, SAMPLE_MS));
  const body = await report();
  if (!body) {
    failures.push("reporting port stopped answering");
    break;
  }
  const m = body.match(/^Global Layers = disk (\d+); memory (\d+)/m);
  if (m) {
    last = { disk: +m[1], mem: +m[2] };
    peak = Math.max(peak ?? 0, last.disk);
  }
  console.log(`  t=${((Date.now() - t0) / 1000).toFixed(1)}s writes=${writes} reads=${reads.length}` +
              (last ? ` disk_layers=${last.disk} mem_layers=${last.mem}` : ""));
}
stop = true;
await Promise.all(writers);
/* Reads alone, against the files the writes left. */
quiet = true;
await new Promise((r) => setTimeout(r, QUIET_SECS * 1000));
const quiet_body = await report();
const quiet_m = quiet_body?.match(/^Global Layers = disk (\d+)/m);
stop_reading = true;
await reading;
setup.close();
const elapsed = Math.max(last_write - first_write, 1) / 1000;
const sorted = [...reads].sort((a, b) => a - b), late = [...late_reads].sort((a, b) => a - b);
const quiet_sorted = [...quiet_reads].sort((a, b) => a - b);
console.log(`${writes} writes (${refused} refused), ${reads.length} reads`);
console.log(`METRIC writes_per_s ${(writes / elapsed).toFixed(1)}`);
console.log(`METRIC reads_per_s ${(reads.length / elapsed).toFixed(1)}`);
console.log(`METRIC read_p50_ms ${percentile(sorted, 0.5).toFixed(3)}`);
console.log(`METRIC read_p99_ms ${percentile(sorted, 0.99).toFixed(3)}`);
console.log(`METRIC late_read_p50_ms ${percentile(late, 0.5).toFixed(3)}`);
console.log(`METRIC late_read_p99_ms ${percentile(late, 0.99).toFixed(3)}`);
console.log(`METRIC quiet_read_p50_ms ${percentile(quiet_sorted, 0.5).toFixed(3)}`);
console.log(`METRIC quiet_read_p99_ms ${percentile(quiet_sorted, 0.99).toFixed(3)}`);
console.log(`METRIC quiet_reads_per_s ${(quiet_reads.length / QUIET_SECS).toFixed(1)}`);
if (quiet_m) {
  console.log(`METRIC quiet_disk_layers ${quiet_m[1]}`);
}
if (peak !== null) {
  console.log(`METRIC peak_disk_layers ${peak}`);
  console.log(`METRIC final_disk_layers ${last.disk}`);
}
if (failures.length) {
  console.error(`LAYER COUNT FAIL:\n  ${failures.join("\n  ")}`);
  process.exit(1);
}
console.log("layer count: ok");
process.exit(0);
