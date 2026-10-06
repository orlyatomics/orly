/** Memory-drain smoke (#607); run by run-memory-full.sh after memory_full.mjs.
 *
 * Reaches a full-pool state on purpose: K writers send BATCH-write batches to one shared safe
 * POV that is paused, so nothing is promoted and nothing drains, until every write is refused.
 * The POV's backlog then fills the update pools up to the reserve. Unpausing it leaves Tetris
 * and the memory merges to move that whole backlog to the global POV and flush it, with only the
 * reserve free to copy into. The pools must drain (below DRAIN_PCT of the Entry pool) within
 * DRAIN_S, and a write must then be accepted and read back. */

import net from "node:net";
import { connect, InsufficientMemoryError } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const REPORT_PORT = +process.env.ORLY_REPORT_PORT;
const K = +(process.env.K ?? 8);
const BATCH = +(process.env.BATCH ?? 200);
const DRAIN_S = +(process.env.DRAIN_S ?? 60);
const DRAIN_PCT = +(process.env.DRAIN_PCT ?? 10);
/* Writers are spread over this many paused POVs, so that unpausing them starts that many
   children's promotions and merges at once. */
const POVS = +(process.env.POVS ?? 8);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const fail = (msg) => { console.error(`MEMORY DRAIN FAIL: ${msg}`); process.exit(1); };

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
const entries = (body) => {
  const m = body?.match(/^Update Entry Pool = (\d+) \/ (\d+)/m);
  return m ? { used: +m[1], size: +m[2] } : null;
};

const setup = await connect(URL);
await setup.newSession();
await setup.install("sample", 1);
const povs = [];
for (let p = 0; p < POVS; ++p) {
  povs.push(await setup.newPov({ safe: true, shared: true }));
  /* Client pause sends a statement the server rejects (#625); send the grammar's form. */
  await setup.send(`pause {${povs[p]}};`);
}
const pov = povs[0];

let writes = 0, refused = 0;
await Promise.all(Array.from({ length: K }, async (_, w) => {
  const c = await connect(URL);
  await c.newSession();
  for (let i = 0, streak = 0; streak < 20; ++i) {
    try {
      await c.callBatch(povs[w % POVS], "sample", "write_val",
        Array.from({ length: BATCH }, (_, j) => ({ n: 100_000_000 + (w * 1_000_000) + i * BATCH + j, x: i })));
      ++writes;
      streak = 0;
    } catch (err) {
      if (!(err instanceof InsufficientMemoryError)) fail(`a write failed other than by refusal: ${err?.message ?? err}`);
      ++refused;
      ++streak;
      await sleep(20);
    }
  }
  c.close();
}));
const full = entries(await report());
console.log(`${POVS} paused POVs filled: ${writes} batches of ${BATCH}, ${refused} refused; Update Entry pool ${full?.used}/${full?.size}`);
if (!refused) fail("no write was refused; the pools never filled");

if (process.env.SEQ_UNPAUSE) { for (const p of povs) await setup.send(`unpause {${p}};`); } else { await Promise.all(povs.map((p) => setup.send(`unpause {${p}};`))); }
const t0 = Date.now();
let last = null;
for (;;) {
  last = entries(await report());
  if (!last) fail("reporting port stopped answering");
  if (last.used * 100 < last.size * DRAIN_PCT) break;
  if (Date.now() - t0 > DRAIN_S * 1000) {
    fail(`the Update Entry pool did not drain within ${DRAIN_S}s of unpausing: ${last.used}/${last.size}`);
  }
  await sleep(500);
}
console.log(`drained to ${last.used}/${last.size} in ${((Date.now() - t0) / 1000).toFixed(1)}s`);

let accepted = false;
for (const deadline = Date.now() + 30_000; !accepted && Date.now() < deadline;) {
  try {
    await setup.call(pov, "sample", "write_val", { n: 7, x: 77 });
    accepted = true;
  } catch (err) {
    if (!(err instanceof InsufficientMemoryError)) fail(`a write after draining failed: ${err?.message ?? err}`);
    await sleep(500);
  }
}
if (!accepted) fail("writes were still refused 30 s after the pools drained");
const x = await setup.call(pov, "sample", "read_val", { n: 7 });
if (x !== 77) fail(`read back ${JSON.stringify(x)}, not 77`);
setup.close();
console.log("MEMORY DRAIN OK");
process.exit(0);
