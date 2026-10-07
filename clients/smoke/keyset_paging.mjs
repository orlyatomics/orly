/** Keyset-paging smoke, TS driver (#735); run by run-keyset-paging.sh, which starts orlyi with
 *  --read_budget_rows=$ROW_LIMIT. It prints METRIC lines for tools/maint/ab_bench.py before it
 *  checks anything.
 *
 *  Group 1 holds $ROWS edges, e = 0 .. ROWS-1. A page is $PAGE edges. `page` (keys ... after)
 *  and `page_skip` (skip s) are timed at page 1 and $DEPTH rows in. */

import net from "node:net";
import { connect, ReadTooLargeError } from "../ts/dist/index.js";

const PKG = "keyset_paging";
const ROWS = +process.env.ROWS;
const ROW_LIMIT = +process.env.ROW_LIMIT;
const PAGE = +process.env.PAGE;
const DEPTH = +process.env.DEPTH;
const REPS = 15;
const REPORT_PORT = +process.env.ORLY_REPORT_PORT;

const fail = (msg) => {
  console.error(`KEYSET PAGING FAIL (ts): ${msg}`);
  process.exit(1);
};
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const es = (rows) => rows.map((r) => r.e);
const range = (from, to) => Array.from({ length: to - from }, (_, i) => from + i);

/* The reporting port's "Global Layers = disk N; memory M" line (#701). Its headers end in a bare
   \n, which node's http parser rejects, so read it off a socket. */
function globalLayers() {
  return new Promise((resolve) => {
    let body = "", done = false;
    const end = (v) => { if (!done) { done = true; sock.destroy(); resolve(v); } };
    const sock = net.connect(REPORT_PORT, "127.0.0.1", () => sock.write("GET / HTTP/1.0\r\n\r\n"));
    sock.on("data", (d) => {
      body += d;
      const m = /^Global Layers = disk (\d+); memory (\d+)$/m.exec(body);
      if (m) end({ disk: +m[1], memory: +m[2] });
    });
    sock.on("error", () => end(null));
    sock.setTimeout(5000, () => end(null));
  });
}

const c = await connect(process.env.ORLY_URL);
await c.newSession();
await c.install(PKG, 1);

/* Write the edges, then wait until Tetris has promoted them all to the global POV, so the
   timed reads below read settled data through a fresh POV. */
{
  const pov = await c.newPov();
  const t0 = performance.now();
  for (let at = 0; at < ROWS; at += 250) {
    const batch = range(at, Math.min(ROWS, at + 250)).map((e) => ({ g: 1, e, w: e * 10 }));
    await c.callBatch(pov, PKG, "put", batch);
  }
  console.log(`wrote ${ROWS} edges in ${((performance.now() - t0) / 1000).toFixed(1)} s`);
  const deadline = Date.now() + 120_000;
  for (;;) {
    const probe = await c.newPov();
    const got = await c.call(probe, PKG, "page", { g: 1, last: ROWS - 2, n: 1 });
    if (same(es(got.rows), [ROWS - 1])) break;
    if (Date.now() > deadline) fail("the edges never reached the global POV");
    await new Promise((r) => setTimeout(r, 250));
  }
  /* And until the global POV's memory merges have written them to disk, so the pages below walk
     disk files (the disk range walker) rather than only memory layers. */
  for (let layers; !((layers = await globalLayers())?.disk > 0 && layers.memory === 0);) {
    if (Date.now() > deadline) fail(`the edges never reached disk: ${JSON.stringify(layers)}`);
    await new Promise((r) => setTimeout(r, 250));
  }
  console.log(`edges on disk: ${JSON.stringify(await globalLayers())}`);
}
const pov = await c.newPov();

/* Cost: median wall time of REPS calls, each a round trip on an idle server. */
async function median(fn) {
  const ms = [];
  for (let i = 0; i < REPS; ++i) {
    const t0 = performance.now();
    await fn();
    ms.push(performance.now() - t0);
  }
  ms.sort((a, b) => a - b);
  return ms[Math.floor(ms.length / 2)];
}
const keyset = (last) => () => c.call(pov, PKG, "page", { g: 1, last, n: PAGE });
const skip = (s) => () => c.call(pov, PKG, "page_skip", { g: 1, s, n: PAGE });
/* Warm up both paths once. */
await keyset(DEPTH - 1)();
await skip(DEPTH)();
const keysetFirst = await median(keyset(-1));
const keysetDeep = await median(keyset(DEPTH - 1));
const skipFirst = await median(skip(0));
const skipDeep = await median(skip(DEPTH));
console.log(`METRIC keyset_page1_ms ${keysetFirst.toFixed(3)}`);
console.log(`METRIC keyset_deep_ms ${keysetDeep.toFixed(3)}`);
console.log(`METRIC skip_page1_ms ${skipFirst.toFixed(3)}`);
console.log(`METRIC skip_deep_ms ${skipDeep.toFixed(3)}`);
console.log(`METRIC keyset_deep_over_page1 ${(keysetDeep / keysetFirst).toFixed(2)}`);
console.log(`METRIC skip_deep_over_page1 ${(skipDeep / skipFirst).toFixed(2)}`);
console.log(`METRIC skip_deep_over_keyset_deep ${(skipDeep / keysetDeep).toFixed(2)}`);

/* The same page both ways. */
{
  const a = await keyset(DEPTH - 1)();
  const b = await skip(DEPTH)();
  if (!same(es(a.rows), range(DEPTH, DEPTH + PAGE)) || !same(a.rows, b)) {
    fail(`page ${DEPTH} rows in: keyset ${JSON.stringify(es(a.rows))}, skip ${JSON.stringify(es(b))}`);
  }
  if (a.last !== DEPTH + PAGE - 1) fail(`page ${DEPTH} rows in: last ${a.last}`);
}
/* A page $DEPTH rows in costs about what page 1 does by keyset (the bound allows for noise on a
   shared runner); by skip it walks $DEPTH rows first, so it costs many times more. */
if (keysetDeep > 3 * keysetFirst + 2) {
  fail(`a keyset page ${DEPTH} rows in took ${keysetDeep.toFixed(2)} ms, page 1 ${keysetFirst.toFixed(2)} ms`);
}
if (skipDeep < 3 * keysetDeep) {
  fail(`a skip page ${DEPTH} rows in took ${skipDeep.toFixed(2)} ms, the keyset page ${keysetDeep.toFixed(2)} ms: ` +
       "the measurement can't tell them apart");
}
console.log(`cost ok: ${DEPTH} rows in, keyset ${keysetDeep.toFixed(2)} ms (page 1 ${keysetFirst.toFixed(2)}), ` +
            `skip ${skipDeep.toFixed(2)} ms (page 1 ${skipFirst.toFixed(2)})`);

/* The read budget: a skip page past ROW_LIMIT rows walks more rows than a read may, and is
   refused; the keyset page at the same place answers. */
{
  const at = ROW_LIMIT + 5 * PAGE;
  try {
    await skip(at)();
    fail(`a skip page ${at} rows in was answered under a ${ROW_LIMIT}-row budget`);
  } catch (err) {
    if (!(err instanceof ReadTooLargeError)) fail(`skip past the budget failed with ${err?.name}: ${err}`);
  }
  const page = await keyset(at - 1)();
  if (!same(es(page.rows), range(at, at + PAGE))) fail(`the keyset page ${at} rows in: ${JSON.stringify(es(page.rows))}`);
  console.log(`budget ok: skip ${at} rows in refused as read_too_large, keyset answered`);
}

/* Client.pages walks every edge once, in order. */
{
  const seen = [];
  let pages = 0;
  for await (const rows of c.pages(pov, PKG, "page", { g: 1, last: -1, n: PAGE }, { pageSize: PAGE })) {
    ++pages;
    for (const r of rows) seen.push(r.e);
  }
  if (!same(seen, range(0, ROWS))) fail(`pages saw ${seen.length} edges, not 0 .. ${ROWS - 1} in order`);
  console.log(`pages ok: ${ROWS} edges in ${pages} pages`);
}

/* Every edge of group g as a fresh POV under global reads it: what has been promoted. */
async function globalEdges(g) {
  const probe = await c.newPov();
  const out = [];
  for await (const rows of c.pages(probe, PKG, "page", { g, last: -1, n: 1000 })) out.push(...es(rows));
  return out;
}
/* Wait for Tetris to promote group g to global until it holds `want`. A POV doesn't see its own
   delete of a key its parent holds until the delete is promoted (#791), so the
   checks below let every write land before they read. */
async function settle(g, want) {
  const deadline = Date.now() + 60_000;
  for (let got; !same(got = await globalEdges(g), want);) {
    if (Date.now() > deadline) fail(`group ${g} never reached ${JSON.stringify(want)} in global: ${JSON.stringify(got)}`);
    await new Promise((r) => setTimeout(r, 100));
  }
}

/* Writes between pages (group 2, edges 0, 10, .., 90; pages of 3). Each page reads the data as it
   is when it runs: rows written or deleted before the boundary don't change later pages, rows
   after it do. */
{
  const w = await c.newPov();
  await c.callBatch(w, PKG, "put", range(0, 10).map((i) => ({ g: 2, e: i * 10, w: i })));
  await settle(2, range(0, 10).map((i) => i * 10));
  const p1 = await c.call(w, PKG, "page", { g: 2, last: -1, n: 3 });
  if (!same(es(p1.rows), [0, 10, 20])) fail(`writes: page 1 ${JSON.stringify(es(p1.rows))}`);
  await c.call(w, PKG, "put", { g: 2, e: 5, w: 0 });   // before the boundary (20)
  await c.call(w, PKG, "remove", { g: 2, e: 10 });     // before it
  await c.call(w, PKG, "put", { g: 2, e: 25, w: 0 });  // after it
  await c.call(w, PKG, "remove", { g: 2, e: 30 });     // after it
  await settle(2, [0, 5, 20, 25, 40, 50, 60, 70, 80, 90]);
  const p2 = await c.call(w, PKG, "page", { g: 2, last: p1.last, n: 3 });
  if (!same(es(p2.rows), [25, 40, 50])) fail(`writes: page 2 ${JSON.stringify(es(p2.rows))}, want [25, 40, 50]`);
  await c.call(w, PKG, "put", { g: 2, e: 45, w: 0 });  // before the new boundary (50)
  await c.call(w, PKG, "put", { g: 2, e: 95, w: 0 });  // after it
  const rest = [];
  for await (const rows of c.pages(w, PKG, "page", { g: 2, last: p2.last, n: 3 })) rest.push(...es(rows));
  if (!same(rest, [60, 70, 80, 90, 95])) fail(`writes: the rest ${JSON.stringify(rest)}, want [60, 70, 80, 90, 95]`);
  console.log("writes ok: rows after the boundary appear on later pages, rows before it don't");
}

/* What a private POV with no writes of its own sees of its parent between pages (group 3). P is a
   shared POV under global; C is a private POV under P that only reads. P is paused while it
   writes, so its writes stay in P instead of being promoted to global. */
{
  const P = await c.newPov();
  await c.callBatch(P, PKG, "put", range(0, 6).map((e) => ({ g: 3, e, w: e })));
  await settle(3, range(0, 6));
  const C = await c.newPov({ shared: false, parent: P });
  const p1 = await c.call(C, PKG, "page", { g: 3, last: -1, n: 3 });
  if (!same(es(p1.rows), [0, 1, 2])) fail(`private POV: page 1 ${JSON.stringify(es(p1.rows))}`);
  /* Written to P after C's first page, and not yet promoted: C sees P's writes as P does. */
  await c.pause(P);
  await c.call(P, PKG, "put", { g: 3, e: 100, w: 0 });
  await c.call(P, PKG, "put", { g: 3, e: 101, w: 0 });
  await c.call(P, PKG, "remove", { g: 3, e: 101 });
  const c2 = await c.call(C, PKG, "page", { g: 3, last: p1.last, n: 10 });
  const pp2 = await c.call(P, PKG, "page", { g: 3, last: p1.last, n: 10 });
  console.log(`POV: P paused, wrote 100 and 101, deleted 101: C's page 2 is ${JSON.stringify(es(c2.rows))}, ` +
              `P's ${JSON.stringify(es(pp2.rows))}, global's ${JSON.stringify(await globalEdges(3))}`);
  if (!same(es(c2.rows), [3, 4, 5, 100]) || !same(c2, pp2)) {
    fail(`private POV: page 2 ${JSON.stringify(es(c2.rows))}, want [3, 4, 5, 100] as P reads it (C reads P as it is now)`);
  }
  /* Promoted to global once P is unpaused. */
  await c.unpause(P);
  await c.call(P, PKG, "remove", { g: 3, e: 4 });
  await settle(3, [0, 1, 2, 3, 5, 100]);
  const c3 = await c.call(C, PKG, "page", { g: 3, last: p1.last, n: 10 });
  if (!same(es(c3.rows), [3, 5, 100])) fail(`private POV: page 2 again ${JSON.stringify(es(c3.rows))}, want [3, 5, 100]`);
  /* Written to a sibling shared POV Q, which C sees once Tetris promotes it to global. */
  const Q = await c.newPov();
  await c.call(Q, PKG, "put", { g: 3, e: 200, w: 0 });
  await settle(3, [0, 1, 2, 3, 5, 100, 200]);
  const c4 = await c.call(C, PKG, "page", { g: 3, last: 100, n: 10 });
  console.log(`POV: once Q's write of 200 reached global, C's next page is ${JSON.stringify(es(c4.rows))}`);
  if (!same(es(c4.rows), [200])) {
    fail(`private POV: page 3 ${JSON.stringify(es(c4.rows))}, want [200] (C reads global as it is now)`);
  }
  console.log("private POV ok: a reading child is not a snapshot; each page reads its parents as they are then");
}

c.close();
console.log("KEYSET PAGING OK (ts)");
