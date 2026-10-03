/** Disk-full smoke (#590); run by run-disk-full.sh.
 *
 * K writers overwrite a small key set on one shared safe POV against an orlyi
 * whose mem-sim volumes are tiny, so the disk fills within a minute or two.
 * Overwrites never shrink the data (safe repos keep history), so the disk
 * stays full once it fills. The smoke waits until orlyi's log shows a merge
 * that ran out of space and handed its inputs back, keeps writing a little
 * longer so the merges retry, then stops and checks that the server is still
 * up and still answers a read.
 *
 * Writes are expected to slow down or stall once the disk is full: nothing
 * refuses them yet, so they pile up in memory while the merges wait for space.
 * A write that doesn't come back within STALL_S ends the write phase early;
 * that is not a failure.
 *
 * The durable writer and merger, and the file service's base image, still
 * abort on a full disk (they can't wait for space yet), so orlyi may die
 * here. run-disk-full.sh tells that apart from a merge abort; this script
 * only reports whether the read worked. */

import fs from "node:fs";
import { connect } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const LOG = process.env.ORLYI_LOG;
const K = +(process.env.K ?? 8);
const KEYS = +(process.env.KEYS ?? 20000);
const MAX_SECS = +(process.env.MAX_SECS ?? 300);
const AFTER_FULL_SECS = +(process.env.AFTER_FULL_SECS ?? 15);
const STALL_S = +(process.env.STALL_S ?? 20);
const RETRY_LINE = /out of disk space .* inputs handed back/;

const withTimeout = (p, secs, what) =>
  Promise.race([p, new Promise((_, rej) => setTimeout(() => rej(new Error(`${what} took over ${secs}s`)), secs * 1000))]);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const fail = (msg) => { console.error(`DISK FULL FAIL: ${msg}`); process.exit(1); };

const setup = await connect(URL);
await setup.newSession();
await setup.install("sample", 1);
const pov = await setup.newPov({ safe: true, shared: true });

let writes = 0, stop = false, ended = "";
const writers = Array.from({ length: K }, async (_, w) => {
  const c = await connect(URL);
  await c.newSession();
  for (let i = 0; !stop; ++i) {
    try {
      await withTimeout(c.call(pov, "sample", "write_val", { n: (w * 7919 + i) % KEYS, x: i }), STALL_S, "a write");
      ++writes;
    } catch (err) {
      if (!stop) { stop = true; ended = `writes stalled (${err?.message ?? err})`; }
      return;
    }
  }
  c.close();
});

const t0 = Date.now();
let full_at = null;
while (!stop) {
  await sleep(1000);
  const secs = (Date.now() - t0) / 1000;
  if (!full_at && RETRY_LINE.test(fs.readFileSync(LOG, "utf8"))) {
    full_at = secs;
    console.log(`  t=${secs.toFixed(0)}s writes=${writes}: disk full, merges retrying`);
  }
  if (full_at !== null && secs - full_at >= AFTER_FULL_SECS) { stop = true; ended = "ran past disk full"; }
  if (secs >= MAX_SECS) { stop = true; ended = "time limit"; }
  if (Math.round(secs) % 10 === 0) console.log(`  t=${secs.toFixed(0)}s writes=${writes}`);
}
console.log(`write phase ended: ${ended}; ${writes} writes`);
await Promise.race([Promise.allSettled(writers), sleep(2000)]);

if (full_at === null) {
  fail(`the disk never filled (${writes} writes in ${MAX_SECS}s); the smoke checked nothing`);
}
/* Reuse the setup session for the read: it already exists, so this asks for no new durable
   state on a full disk. */
try {
  await withTimeout(setup.call(pov, "sample", "read_val", { n: 1 }), 30, "the read");
  console.log("READ: ok");
} catch (err) {
  console.log(`READ: failed (${err?.message ?? err})`);
}
process.exit(0);
