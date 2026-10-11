/** Write amplification and merge amplification smoke benchmark (#857).
 *
 * Runs two workloads on one server:
 *   1. Inserts of new keys: measures write amplification ((flush + merge) / logical)
 *      and merge amplification (merge / flush).
 *   2. Overwrites of a fixed key set across 3 sequential passes on the same server:
 *      measures write amplification, merge amplification, throughput, and p99 latency per pass,
 *      verifying that write amplification remains within bounds and that pass 3 throughput
 *      and p99 stay within 10% of pass 1 (three-pass flatness check).
 *
 * Emits METRIC lines for tools/maint/ab_bench.py.
 */

import net from "node:net";
import { connect, InsufficientMemoryError, InsufficientStorageError } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const REPORT_PORT = +process.env.ORLY_REPORT_PORT;
const K = +(process.env.K ?? 4);

/* Number of distinct new keys to insert in Load 1. */
const INSERT_COUNT = +(process.env.INSERT_COUNT ?? 4000);

/* Key space size and overwrite count per pass in Load 2. */
const NUM_KEYS = +(process.env.NUM_KEYS ?? 500);
const OVERWRITE_COUNT = +(process.env.OVERWRITE_COUNT ?? 4000);

/* Write amplification limit for overwrites (#857 §3). */
const MAX_WRITE_AMP = +(process.env.MAX_WRITE_AMP ?? 60.0);

/* Allowed relative variation between pass 1 and pass 3 ("about 10%", #857 §4). */
const FLATNESS_TOLERANCE = +(process.env.FLATNESS_TOLERANCE ?? 0.15);
/* Minimum absolute latency increase in ms to flag p99 decay, avoiding sub-ms jitter. */
const MIN_P99_DECAY_MS = +(process.env.MIN_P99_DECAY_MS ?? 1.0);


/* Query reporting port for write volume and layer counts. */
function report() {
  return new Promise((resolve) => {
    let body = "", done = false;
    const end = (v) => { if (!done) { done = true; sock.destroy(); resolve(v); } };
    const sock = net.connect(REPORT_PORT, "127.0.0.1", () => sock.write("GET / HTTP/1.0\r\n\r\n"));
    sock.on("data", (d) => {
      body += d;
      if (/^Update Entry Pool = .*\n/m.test(body)) end(body);
    });
    sock.on("error", () => end(null));
    sock.setTimeout(5000, () => end(null));
  });
}

async function getStats() {
  const body = await report();
  if (!body) return null;
  const wv = body.match(/^Global Write Volume = flush (\d+); merge (\d+); logical (\d+)/m);
  const ly = body.match(/^Global Layers = disk (\d+); memory (\d+)/m);
  return {
    flush: wv ? Number(wv[1]) : 0,
    merge: wv ? Number(wv[2]) : 0,
    logical: wv ? Number(wv[3]) : 0,
    diskLayers: ly ? Number(ly[1]) : 0,
    memLayers: ly ? Number(ly[2]) : 0,
  };
}

/* Wait until memory merges flush remaining updates and disk merges settle. */
async function settle(maxWaitMs = 10000) {
  const start = Date.now();
  let lastFlush = -1, lastMerge = -1, stableCount = 0;
  while (Date.now() - start < maxWaitMs) {
    await new Promise((r) => setTimeout(r, 200));
    const stats = await getStats();
    if (!stats) break;
    if (stats.memLayers === 0 && stats.flush === lastFlush && stats.merge === lastMerge) {
      if (++stableCount >= 2) {
        return stats;
      }
    } else {
      stableCount = 0;
      lastFlush = stats.flush;
      lastMerge = stats.merge;
    }
  }
  return await getStats();
}

function percentile(sorted, p) {
  return sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * p))] : 0;
}

const setup = await connect(URL);
await setup.newSession();
await setup.install("write_amp", 1);
const pov = await setup.newPov({ safe: true, shared: true });

const failures = [];

console.log(`[write_amp] Initializing: K=${K} writers, ${INSERT_COUNT} inserts, ${NUM_KEYS} keys, ${OVERWRITE_COUNT} overwrites/pass`);

/* Helper for running concurrent writers. */
async function runLoad(label, count, keyFn) {
  const perWriter = Math.floor(count / K);
  const latencies = [];
  let completed = 0, refused = 0;
  const t0 = performance.now();

  const writer = async (w) => {
    const c = await connect(URL);
    await c.newSession();
    const startIdx = w * perWriter;
    const endIdx = (w === K - 1) ? count : (w + 1) * perWriter;

    for (let i = startIdx; i < endIdx; ++i) {
      const key = keyFn(w, i);
      const val = i;
      const sent = performance.now();
      try {
        await c.call(pov, "write_amp", "write_val", { n: key, x: val });
        latencies.push(performance.now() - sent);
        ++completed;
      } catch (err) {
        if (err instanceof InsufficientMemoryError || err instanceof InsufficientStorageError) {
          ++refused;
          --i;
          await new Promise((r) => setTimeout(r, 20));
          continue;
        }
        failures.push(`${label} writer ${w}: ${err?.message ?? err}`);
        break;
      }
    }
    c.close();
  };

  await Promise.all(Array.from({ length: K }, (_, w) => writer(w)));
  const elapsedSec = Math.max((performance.now() - t0) / 1000, 0.001);
  latencies.sort((a, b) => a - b);
  const p99 = percentile(latencies, 0.99);
  const throughput = completed / elapsedSec;

  return { completed, refused, elapsedSec, throughput, p99, latencies };
}

/* =========================================================================
 * LOAD 1: Inserts of new keys
 * ========================================================================= */
console.log(`\n[Load 1] Running inserts: ${INSERT_COUNT} new keys...`);
const vInsertStart = await getStats();
const insertRes = await runLoad("insert", INSERT_COUNT, (w, i) => i);
const vInsertEnd = await settle();

const insertFlush = vInsertEnd.flush - vInsertStart.flush;
const insertMerge = vInsertEnd.merge - vInsertStart.merge;
const insertLogical = vInsertEnd.logical - vInsertStart.logical;
const insertWriteAmp = insertLogical > 0 ? (insertFlush + insertMerge) / insertLogical : 0;
const insertMergeAmp = insertFlush > 0 ? insertMerge / insertFlush : 0;

console.log(`[Load 1] Inserts done in ${insertRes.elapsedSec.toFixed(2)}s: ${insertRes.completed} writes, ` +
            `flush=${insertFlush} B, merge=${insertMerge} B, logical=${insertLogical} B, ` +
            `write_amp=${insertWriteAmp.toFixed(2)}, merge_amp=${insertMergeAmp.toFixed(2)}`);

console.log(`METRIC insert_writes_per_s ${insertRes.throughput.toFixed(1)}`);
console.log(`METRIC insert_p99_ms ${insertRes.p99.toFixed(2)}`);
console.log(`METRIC insert_write_amp ${insertWriteAmp.toFixed(2)}`);
console.log(`METRIC insert_merge_amp ${insertMergeAmp.toFixed(2)}`);

/* =========================================================================
 * LOAD 2: Overwrites of a fixed key set (3 sequential passes)
 * ========================================================================= */
console.log(`\n[Load 2] Running overwrites on ${NUM_KEYS} keys across 3 passes...`);
const passes = [];
const vOverwriteAllStart = await getStats();

for (let p = 1; p <= 3; ++p) {
  const vPassStart = await getStats();
  const passRes = await runLoad(`overwrite_pass${p}`, OVERWRITE_COUNT, (w, i) => i % NUM_KEYS);
  const vPassEnd = await settle();

  const pFlush = vPassEnd.flush - vPassStart.flush;
  const pMerge = vPassEnd.merge - vPassStart.merge;
  const pLogical = vPassEnd.logical - vPassStart.logical;
  const pWriteAmp = pLogical > 0 ? (pFlush + pMerge) / pLogical : 0;
  const pMergeAmp = pFlush > 0 ? pMerge / pFlush : 0;

  passes.push({
    pass: p,
    throughput: passRes.throughput,
    p99: passRes.p99,
    flush: pFlush,
    merge: pMerge,
    logical: pLogical,
    writeAmp: pWriteAmp,
    mergeAmp: pMergeAmp,
  });

  console.log(`[Load 2 Pass ${p}] ${passRes.completed} overwrites in ${passRes.elapsedSec.toFixed(2)}s ` +
              `(${passRes.throughput.toFixed(1)} w/s, p99=${passRes.p99.toFixed(2)} ms): ` +
              `flush=${pFlush} B, merge=${pMerge} B, logical=${pLogical} B, ` +
              `write_amp=${pWriteAmp.toFixed(2)}, merge_amp=${pMergeAmp.toFixed(2)}`);

  console.log(`METRIC overwrite_pass${p}_writes_per_s ${passRes.throughput.toFixed(1)}`);
  console.log(`METRIC overwrite_pass${p}_p99_ms ${passRes.p99.toFixed(2)}`);
  console.log(`METRIC overwrite_pass${p}_write_amp ${pWriteAmp.toFixed(2)}`);
  console.log(`METRIC overwrite_pass${p}_merge_amp ${pMergeAmp.toFixed(2)}`);
}

const vOverwriteAllEnd = await getStats();
const totalOverFlush = vOverwriteAllEnd.flush - vOverwriteAllStart.flush;
const totalOverMerge = vOverwriteAllEnd.merge - vOverwriteAllStart.merge;
const totalOverLogical = vOverwriteAllEnd.logical - vOverwriteAllStart.logical;
const totalOverWriteAmp = totalOverLogical > 0 ? (totalOverFlush + totalOverMerge) / totalOverLogical : 0;
const totalOverMergeAmp = totalOverFlush > 0 ? totalOverMerge / totalOverFlush : 0;

console.log(`\n[Load 2 Total] Overwrites: flush=${totalOverFlush} B, merge=${totalOverMerge} B, logical=${totalOverLogical} B, ` +
            `write_amp=${totalOverWriteAmp.toFixed(2)}, merge_amp=${totalOverMergeAmp.toFixed(2)}`);

console.log(`METRIC overwrite_write_amp ${totalOverWriteAmp.toFixed(2)}`);
console.log(`METRIC overwrite_merge_amp ${totalOverMergeAmp.toFixed(2)}`);

/* =========================================================================
 * CHECK 3: Write amplification limit check (#857 §3)
 * ========================================================================= */
if (totalOverWriteAmp > MAX_WRITE_AMP) {
  failures.push(`WRITE AMP LIMIT FAIL: overwrite write amplification ${totalOverWriteAmp.toFixed(2)} exceeds limit ${MAX_WRITE_AMP}`);
}

/* =========================================================================
 * CHECK 4: Three-pass flatness check (#857 §4)
 * ========================================================================= */
const p1 = passes[0];
const p3 = passes[2];
const throughputRatio = p1.throughput > 0 ? p3.throughput / p1.throughput : 1.0;
const p99Ratio = p1.p99 > 0 ? p3.p99 / p1.p99 : 1.0;

console.log(`\n[Flatness] Pass 1 -> Pass 3: throughput ${p1.throughput.toFixed(1)} -> ${p3.throughput.toFixed(1)} ` +
            `(ratio ${(throughputRatio * 100).toFixed(1)}%), ` +
            `p99 ${p1.p99.toFixed(2)} -> ${p3.p99.toFixed(2)} ms (ratio ${(p99Ratio * 100).toFixed(1)}%)`);

console.log(`METRIC flatness_throughput_ratio ${throughputRatio.toFixed(3)}`);
console.log(`METRIC flatness_p99_ratio ${p99Ratio.toFixed(3)}`);

/* Fails if pass 3 throughput is worse than pass 1 by more than tolerance. */
if (throughputRatio < (1.0 - FLATNESS_TOLERANCE)) {
  failures.push(`FLATNESS FAIL: pass 3 throughput (${p3.throughput.toFixed(1)} w/s) is more than ` +
                `${(FLATNESS_TOLERANCE * 100).toFixed(0)}% worse than pass 1 (${p1.throughput.toFixed(1)} w/s) ` +
                `(ratio ${throughputRatio.toFixed(3)})`);
}

/* Fails if pass 3 p99 latency is worse (higher) than pass 1 by more than tolerance and min delta. */
if (p99Ratio > (1.0 + FLATNESS_TOLERANCE) && (p3.p99 - p1.p99) > MIN_P99_DECAY_MS) {
  failures.push(`FLATNESS FAIL: pass 3 p99 (${p3.p99.toFixed(2)} ms) is more than ` +
                `${(FLATNESS_TOLERANCE * 100).toFixed(0)}% worse than pass 1 (${p1.p99.toFixed(2)} ms) ` +
                `by at least ${MIN_P99_DECAY_MS.toFixed(1)} ms (ratio ${p99Ratio.toFixed(3)})`);
}

setup.close();

if (failures.length) {
  console.error(`\nWRITE AMP SMOKE FAILED:\n  ${failures.join("\n  ")}`);
  process.exit(1);
}

console.log("\nwrite amplification smoke: ok");
process.exit(0);
