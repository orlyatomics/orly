/** Write-too-large smoke, TS driver (#687); run by run-write-too-large.sh.
 *
 * Against an orlyi whose Update Entry pool is forced small, one batch with more entries than half
 * the pool's merge reserve must be refused with status "write_too_large" (WriteTooLargeError, not
 * InsufficientMemoryError or a generic OrlyError), must leave nothing behind, and must stay refused
 * when resent. The same writes split into batches under the limit must all be accepted. */

import { connect, OrlyError, InsufficientMemoryError, WriteTooLargeError } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const LIMIT = +process.env.LIMIT;  // the most entries a single write may hold
const fail = (msg) => { console.error(`WRITE TOO LARGE FAIL (ts): ${msg}`); process.exit(1); };
if (!(LIMIT > 0)) fail("LIMIT is not set");

const c = await connect(URL);
await c.newSession();
await c.install("sample", 1);
const pov = await c.newPov({ safe: true, shared: true });

const TOTAL = 2 * LIMIT + 2;
const rows = Array.from({ length: TOTAL }, (_, j) => ({ n: 1000 + j, x: 7 }));

for (let attempt = 0; attempt < 2; ++attempt) {
  try {
    await c.callBatch(pov, "sample", "write_val", rows);
    fail(`a ${TOTAL}-entry batch was accepted with a limit of ${LIMIT}`);
  } catch (err) {
    if (!(err instanceof WriteTooLargeError)) {
      fail(`a ${TOTAL}-entry batch failed with ${err?.name}, not WriteTooLargeError: ${JSON.stringify(err?.reply) ?? err}`);
    }
    if (!(err instanceof OrlyError) || err instanceof InsufficientMemoryError) {
      fail("WriteTooLargeError has the wrong class hierarchy");
    }
    const r = err.reply;
    if (r?.status !== "write_too_large" || !String(r?.result).startsWith("write too large")) {
      fail(`unexpected reply: ${JSON.stringify(r)}`);
    }
    if (!attempt) console.log(`refused ${TOTAL} entries: ${r.result}`);
  }
}
{
  const v = await c.call(pov, "sample", "read_val", { n: 1000 });
  if (v === 7) fail("the refused batch left a write behind");
}

// The same rows in batches of LIMIT entries all land. A transient insufficient_memory is
// backpressure, so retry it; anything else fails the smoke.
for (let i = 0; i < TOTAL; i += LIMIT) {
  const part = rows.slice(i, i + LIMIT);
  for (let tries = 0; ; ++tries) {
    try {
      await c.callBatch(pov, "sample", "write_val", part);
      break;
    } catch (err) {
      if (err instanceof InsufficientMemoryError && tries < 300) {
        await new Promise((r) => setTimeout(r, 100));
        continue;
      }
      fail(`a ${part.length}-entry batch under the limit failed: ${err?.message ?? err}`);
    }
  }
}
for (const n of [1000, 1000 + LIMIT, 1000 + TOTAL - 1]) {
  const v = await c.call(pov, "sample", "read_val", { n });
  if (v !== 7) fail(`read_val(${n}) = ${JSON.stringify(v)} after the split batches, not 7`);
}
console.log(`accepted the same ${TOTAL} entries in batches of ${LIMIT}`);
c.close();
console.log("WRITE TOO LARGE OK (ts)");
