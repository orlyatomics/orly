/** Disk-full smoke (#590); run by run-disk-full.sh.
 *
 * K writers overwrite a small key set on one shared safe POV against an orlyi
 * whose mem-sim volumes are tiny. Overwrites never shrink the data (safe repos
 * keep history), so free space only goes down. Once it falls below the write
 * admission reserve, orlyi must refuse writes with status
 * "insufficient_storage" instead of stalling or aborting, and keep serving
 * reads.
 *
 * Checks, in order:
 *   - writes are eventually refused with "insufficient_storage", and every
 *     other write either succeeds or is refused that way (no other error, and
 *     no write that hangs for STALL_S);
 *   - after IDLE_SECS with no writes, a read on an existing session works, a
 *     new connection can open a session and read, and a write gets an answer
 *     (accepted or refused) rather than hanging.
 * run-disk-full.sh also requires orlyi to be alive with no abort in its log. */

import { connect, InsufficientStorageError } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const K = +(process.env.K ?? 8);
const KEYS = +(process.env.KEYS ?? 20000);
const MAX_SECS = +(process.env.MAX_SECS ?? 300);
const AFTER_REFUSED_SECS = +(process.env.AFTER_REFUSED_SECS ?? 10);
const IDLE_SECS = +(process.env.IDLE_SECS ?? 10);
const STALL_S = +(process.env.STALL_S ?? 20);

const withTimeout = (p, secs, what) =>
  Promise.race([p, new Promise((_, rej) => setTimeout(() => rej(new Error(`${what} took over ${secs}s`)), secs * 1000))]);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const fail = (msg) => { console.error(`DISK FULL FAIL: ${msg}`); process.exit(1); };
const isRefusal = (err) => err instanceof InsufficientStorageError;

const setup = await connect(URL);
await setup.newSession();
await setup.install("sample", 1);
const pov = await setup.newPov({ safe: true, shared: true });

let writes = 0, refused = 0, stop = false, error = null, first_refusal = null;
const writers = Array.from({ length: K }, async (_, w) => {
  const c = await connect(URL);
  await c.newSession();
  for (let i = 0; !stop; ++i) {
    try {
      await withTimeout(c.call(pov, "sample", "write_val", { n: (w * 7919 + i) % KEYS, x: i }), STALL_S, "a write");
      ++writes;
    } catch (err) {
      if (isRefusal(err)) {
        if (!refused++) {
          first_refusal = err.reply.result;
        }
        await sleep(50);
        continue;
      }
      if (!stop) { stop = true; error = err?.message ?? String(err); }
      return;
    }
  }
  c.close();
});

const t0 = Date.now();
let refused_at = null;
while (!stop) {
  await sleep(1000);
  const secs = (Date.now() - t0) / 1000;
  if (refused && refused_at === null) {
    refused_at = secs;
    console.log(`  t=${secs.toFixed(0)}s writes=${writes}: first refusal: ${first_refusal}`);
  }
  if (refused_at !== null && secs - refused_at >= AFTER_REFUSED_SECS) { stop = true; }
  if (secs >= MAX_SECS) { stop = true; }
  if (Math.round(secs) % 10 === 0) console.log(`  t=${secs.toFixed(0)}s writes=${writes} refused=${refused}`);
}
console.log(`write phase ended: ${writes} writes, ${refused} refused`);
await Promise.race([Promise.allSettled(writers), sleep(2000)]);

if (error) {
  fail(`a write failed other than by refusal: ${error}`);
}
if (refused_at === null) {
  fail(`no write was refused (${writes} writes in ${MAX_SECS}s); the smoke checked nothing`);
}

console.log(`idle ${IDLE_SECS}s`);
await sleep(IDLE_SECS * 1000);

try {
  await withTimeout(setup.call(pov, "sample", "read_val", { n: 1 }), 30, "the read");
  console.log("READ: ok");
} catch (err) {
  fail(`read on an existing session failed: ${err?.message ?? err}`);
}
try {
  const c = await withTimeout(connect(URL), 30, "connect");
  await withTimeout(c.newSession(), 30, "a new session");
  await withTimeout(c.call(pov, "sample", "read_val", { n: 2 }), 30, "the read");
  console.log("NEW SESSION READ: ok");
  /* Either answer is fine: finished merges release their claim on space, which can lift the
     refusal. A hang or any other error is not. */
  try {
    await withTimeout(c.call(pov, "sample", "write_val", { n: 2, x: -1 }), 30, "the write");
    console.log("WRITE AFTER IDLE: accepted");
  } catch (err) {
    if (!isRefusal(err)) {
      fail(`a write after the idle failed other than by refusal: ${err?.message ?? err}`);
    }
    console.log("WRITE AFTER IDLE: refused");
  }
  c.close();
} catch (err) {
  fail(`new session after the idle: ${err?.message ?? err}`);
}
setup.close();
console.log("DISK FULL OK");
process.exit(0);
