/** POV review smoke (#746); run by run-pov-review.sh.
 *
 *  1. Diff, for each POV P that is safe or fast, shared or private, under the global POV or under a
 *     paused shared POV, tracking conflicts in report mode.  The parent holds edges 0..5 of a group
 *     and three counters; after the fork the parent overwrites edge 3 and edge 5, adds edge 200 and
 *     adds 10 to counter 0.  Paused, P deletes edge 4, overwrites edge 3, adds edge 100, adds and
 *     deletes edge 50, adds 1 and 2 to counter 0, sets counter 2 and adds two tags.  P's diff must
 *     list exactly its own changes (the parent's never), with the right kinds, before and after
 *     values and deltas, also while another connection keeps writing the parent; restricted to a
 *     key range; and paged.  Promoted, P must report exactly one conflict, edge 3, and the parent
 *     must then show P's writes.
 *  2. Refusing mode: a promotion that would overwrite a key the parent changed is refused and the
 *     POV stays paused; forced, it promotes and reports the conflict.  An unpaused refusing POV is
 *     held back (blocked) by Tetris; discarding it unblocks it and returns it to its parent's
 *     state.  Commutative updates never conflict.
 *  3. Discard: thousands of paused writes are thrown away, the update pools fall back, and the
 *     POV reads as its parent.
 *  4. Errors: discarding a shared POV or another session's POV, `.since` (save points, #745), an
 *     unknown option. */

import net from "node:net";
import { connect, InsufficientMemoryError } from "../ts/dist/index.js";

const PKG = "pov_review";
const REPORT_PORT = +process.env.ORLY_REPORT_PORT;
/* The writes racing each diff, per scenario, and the pause between them. Each scenario's paused
   parent keeps its writes until the run ends, so a server with small update pools (TSan) asks for
   fewer, spread over the same time: the default 500 writes of 3 updates took the whole Update pool
   by the fifth scenario and refused the next one's setup writes. */
const WRITER_MAX = +(process.env.WRITER_MAX ?? 500);
const WRITER_SLEEP_MS = +(process.env.WRITER_SLEEP_MS ?? 2);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
let failures = 0;
const check = (what, got, want) => {
  if (!same(got, want)) {
    ++failures;
    console.log(`POV REVIEW FAIL: ${what}: got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);
  }
  return same(got, want);
};
const expectError = async (what, promise, pattern) => {
  try {
    await promise;
    ++failures;
    console.log(`POV REVIEW FAIL: ${what}: no error`);
  } catch (e) {
    const text = JSON.stringify(e.reply ?? e.message);
    if (!pattern.test(text)) {
      ++failures;
      console.log(`POV REVIEW FAIL: ${what}: wrong error ${text}`);
    }
  }
};

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
const pools = async () => {
  const body = await report();
  const u = body?.match(/^Update Pool = (\d+) \/ (\d+)/m);
  const e = body?.match(/^Update Entry Pool = (\d+) \/ (\d+)/m);
  return u && e ? { updates: +u[1], entries: +e[1] } : null;
};

const c = await connect(process.env.ORLY_URL);
await c.newSession();
await c.install(PKG, 1);
/* One POV that writes into the global POV (a private child: its writes promote), and one that
   reads it (a child with no writes reads its parent).  Each POV holds data layers, so the test
   doesn't make one per read. */
const GW = await c.newPov({ safe: false, shared: false });
const GR = await c.newPov({ safe: false, shared: false });
const globalView = async () => GR;

/* JSON numbers come back as floats, and sets as arrays in no set order. */
const norm = (v) => Array.isArray(v) ? v.map(norm) : typeof v === "number" ? Math.round(v) : v;
const normSet = (v) => Array.isArray(v) ? norm(v).sort() : norm(v);
const normChange = (ch) => {
  /* The tags are a set. */
  const val = ch.key[0] === "tags" ? normSet : norm;
  const out = { key: norm(ch.key), kind: ch.kind, before: val(ch.before), after: val(ch.after) };
  if (ch.kind === "delta") { out.op = ch.op; out.delta = val(ch.delta); }
  return out;
};
const normDiff = (changes) => changes.map(normChange);

/* What a POV shows of group g. */
async function view(pov, g) {
  const get = (e) => c.call(pov, PKG, "get", { g, e });
  const count = (k) => c.call(pov, PKG, "count", { g, k });
  return norm({ e3: await get(3), e4: await get(4), e5: await get(5), e100: await get(100), e200: await get(200),
    c0: await count(0), c2: await count(2) });
}
async function waitFor(what, read, want, ms = 30_000) {
  const deadline = Date.now() + ms;
  for (let got; !same((got = await read()), want);) {
    if (Date.now() > deadline) { check(`${what} (${ms / 1000} s)`, got, want); return false; }
    await sleep(100);
  }
  return true;
}

/* A setup write the server refuses for want of pool room is retried: on a slow server (TSan)
   the merges can lag the earlier scenarios' writes for a while, and the refusal clears once they
   catch up.  Only the scenarios' writers, which test the refusal, stop at the first one. */
async function retryRefused(write) {
  const deadline = Date.now() + 60000;
  for (;;) {
    try {
      return await write();
    } catch (e) {
      if (!(e instanceof InsufficientMemoryError) || Date.now() >= deadline) throw e;
      await sleep(250);
    }
  }
}

/* Writes into `pov`: a fresh private child of the global POV (so the writes promote into it), or a
   paused shared parent (they stay there). */
async function writeBase(pov, g) {
  await retryRefused(() => c.callBatch(pov, PKG, "put", [0, 1, 2, 3, 4, 5].map((e) => ({ g, e, w: e * 10 }))));
  for (const [k, n] of [[0, 15], [1, 15], [2, 14]]) await retryRefused(() => c.call(pov, PKG, "set_count", { g, k, n }));
}
async function parentChanges(pov, g) {
  await retryRefused(() => c.call(pov, PKG, "put", { g, e: 3, w: 3000 }));
  await retryRefused(() => c.call(pov, PKG, "put", { g, e: 5, w: 555 }));
  await retryRefused(() => c.call(pov, PKG, "put", { g, e: 200, w: 7 }));
  await retryRefused(() => c.call(pov, PKG, "bump", { g, k: 0, n: 10 }));
}
async function povChanges(pov, g) {
  await retryRefused(() => c.call(pov, PKG, "remove", { g, e: 4 }));
  await retryRefused(() => c.call(pov, PKG, "put", { g, e: 3, w: 333 }));
  await retryRefused(() => c.call(pov, PKG, "put", { g, e: 100, w: 0 }));
  await retryRefused(() => c.call(pov, PKG, "put", { g, e: 50, w: 5 }));
  await retryRefused(() => c.call(pov, PKG, "remove", { g, e: 50 }));
  await retryRefused(() => c.call(pov, PKG, "bump", { g, k: 0, n: 1 }));
  await retryRefused(() => c.call(pov, PKG, "bump", { g, k: 0, n: 2 }));
  await retryRefused(() => c.call(pov, PKG, "set_count", { g, k: 2, n: 100 }));
  await retryRefused(() => c.call(pov, PKG, "tag", { g, t: "b" }));
  await retryRefused(() => c.call(pov, PKG, "tag", { g, t: "a" }));
}
/* P's diff, given counter 0 in the parent. */
const expectedDiff = (g, c0) => [
  { key: ["count", g, 0], kind: "delta", before: c0, after: c0 + 3, op: "add", delta: 3 },
  { key: ["count", g, 2], kind: "changed", before: 14, after: 100 },
  { key: ["edge", g, 3], kind: "changed", before: 3000, after: 333 },
  { key: ["edge", g, 4], kind: "removed", before: 40, after: null },
  { key: ["edge", g, 100], kind: "added", before: null, after: 0 },
  { key: ["tags", g], kind: "delta", before: null, after: ["a", "b"], op: "union", delta: ["a", "b"] },
];

/* 1. Diff and report mode. */
let g = 0;
for (const parentKind of ["global", "paused shared pov"]) {
  for (const safe of [true, false]) {
    for (const shared of [true, false]) {
      ++g;
      const name = `${safe ? "safe" : "fast"} ${shared ? "shared" : "private"} pov under the ${parentKind}`;
      const before = failures;
      let parent, parentView, parentWriter;
      if (parentKind === "global") {
        await writeBase(GW, g);
        parentView = globalView;
        parentWriter = async () => GW;
        await waitFor(`${name}: the base reaches global`, async () => view(await parentView(), g),
          { e3: 30, e4: 40, e5: 50, e100: -1, e200: -1, c0: 15, c2: 14 });
      } else {
        parent = await c.newPov({ safe: true, shared: true });
        await c.pause(parent);
        await writeBase(parent, g);
        parentView = async () => parent;
        parentWriter = async () => parent;
      }
      const P = await c.newPov({ safe, shared, parent, conflicts: "report" });
      await c.pause(P);
      check(`${name}: diff before any write`, (await c.diff(P)).changes, []);
      await parentChanges(await parentWriter(), g);
      await waitFor(`${name}: the parent's changes land`, async () => view(await parentView(), g),
        { e3: 3000, e4: 40, e5: 555, e100: -1, e200: 7, c0: 25, c2: 14 });
      await povChanges(P, g);

      const full = await c.diff(P);
      check(`${name}: diff`, normDiff(full.changes), expectedDiff(g, 25));
      check(`${name}: diff next`, [full.next, full.next_literal], [null, null]);
      check(`${name}: diff updates`, full.updates, 10);

      /* A key range: edges of group g only. */
      const ranged = await c.diff(P, { start: ["edge", g], stop: ["edge", g + 1] });
      check(`${name}: ranged diff`, normDiff(ranged.changes), expectedDiff(g, 25).filter((ch) => ch.key[0] === "edge"));
      const ranged2 = await c.diff(P, { start: ["count", g, 1], stop: ["edge", g, 4] });
      check(`${name}: ranged diff 2`, normDiff(ranged2.changes), expectedDiff(g, 25).slice(1, 3));

      /* Paged, 2 at a time and 1 at a time. */
      for (const limit of [2, 1]) {
        const pages = [];
        for await (const page of c.diffPages(P, { limit })) {
          if (page.length > limit) check(`${name}: page size ${limit}`, page.length, limit);
          pages.push(...page);
        }
        check(`${name}: paged by ${limit}`, normDiff(pages), expectedDiff(g, 25));
      }
      const first = await c.diff(P, { limit: 2 });
      check(`${name}: first page next`, norm(first.next), ["count", g, 2]);
      const second = await c.diff(P, { limit: 2, after: first.next });
      check(`${name}: second page from next`, normDiff(second.changes), expectedDiff(g, 25).slice(2, 4));

      /* Another connection writes the parent while P is diffed: new edges, edge 3 again, counter
         0.  P's diff keeps its own keys only, and its after values follow the parent's. */
      const w = await connect(process.env.ORLY_URL);
      await w.newSession();
      let writing = true;
      let written = 0;
      const writerPov = parentKind === "global" ? await w.newPov({ safe: false, shared: false }) : parent;
      const writer = (async () => {
        /* At most WRITER_MAX writes: a paused parent keeps them all, and refuses them once its
           backlog is full (a small server, as under TSan). */
        for (let i = 0; writing && i < WRITER_MAX; ++i) {
          try {
            await w.callMany(writerPov, [
            [PKG, "put", { g, e: 1000 + i, w: i }],
            [PKG, "put", { g, e: 3, w: 4000 + i }],
            [PKG, "bump", { g, k: 0, n: 1 }],
            ]);
          } catch (e) {
            if (!(e instanceof InsufficientMemoryError)) throw e;
            break;
          }
          ++written;
          await sleep(WRITER_SLEEP_MS);
        }
      })();
      for (let i = 0; i < 20; ++i) {
        const d = normDiff((await c.diff(P)).changes);
        const keys = d.map((ch) => JSON.stringify(ch.key));
        check(`${name}: diff keys under parent writes`, keys, expectedDiff(g, 0).map((ch) => JSON.stringify(ch.key)));
        const byKey = Object.fromEntries(d.map((ch) => [ch.key.join("/"), ch]));
        const c0 = byKey[`count/${g}/0`];
        if (c0) check(`${name}: counter delta under parent writes`, [c0.kind, c0.delta, c0.after - c0.before], ["delta", 3, 3]);
        const e3 = byKey[`edge/${g}/3`];
        if (e3) check(`${name}: edge 3 under parent writes`, [e3.kind, e3.after], ["changed", 333]);
        const e4 = byKey[`edge/${g}/4`];
        if (e4) check(`${name}: edge 4 under parent writes`, [e4.kind, e4.before, e4.after], ["removed", 40, null]);
        await sleep(50);
      }
      writing = false;
      await writer;
      if (written < 5) check(`${name}: parent writes during the diffs`, written >= 5, true);
      if (parentKind === "global") {
        /* Let the writer's POV finish promoting into the global POV, so P's edge 3 lands last. */
        await waitFor(`${name}: the parent writer drains`, async () => (await w.review(writerPov)).pending, 0);
      }
      w.close();

      /* Promote: exactly one conflict, edge 3, overwritten by P after the parent changed it. */
      const promoted = await c.promote(P);
      check(`${name}: promote`, [promoted.status, promoted.pending, promoted.blocked_on], ["promoted", 0, []]);
      check(`${name}: promote conflicts`, promoted.conflicts.map((x) => [x.number, norm(x.key), x.op, !!x.raced]),
        [[1, ["edge", g, 3], "put", false]]);
      if (parentKind !== "global") {
        /* The parent is paused, so P's writes stop there; it now shows them. */
        await waitFor(`${name}: the parent after promotion`, async () => (await view(parent, g)).e3, 333);
      } else {
        await waitFor(`${name}: global after promotion`, async () => (await view(await parentView(), g)).e3, 333);
      }
      const after = await view(await parentView(), g);
      check(`${name}: the parent after promotion`, [after.e3, after.e4, after.e100, after.c2], [333, -1, 0, 100]);
      check(`${name}: diff after promotion`, (await c.diff(P)).changes, []);
      const review = await c.review(P);
      check(`${name}: review after promotion`, [review.conflict_mode, review.status, review.pending, review.blocked, review.conflict_count],
        ["report", "normal", 0, false, 1]);
      if (failures === before) console.log(`POV REVIEW OK: ${name}`);
    }
  }
}

/* 2. Refusing mode. */
{
  const before = failures;
  /* Refused, then forced. */
  g = 20;
  await writeBase(GW, g);
  await waitFor("refuse: base", async () => (await view(await globalView(), g)).e3, 30);
  const P = await c.newPov({ safe: false, shared: false, conflicts: "refuse" });
  await c.pause(P);
  await c.call(GW, PKG, "put", { g, e: 3, w: 3000 });
  await waitFor("refuse: the parent's change lands", async () => (await view(await globalView(), g)).e3, 3000);
  await c.call(P, PKG, "put", { g, e: 3, w: 333 });
  await c.call(P, PKG, "put", { g, e: 100, w: 1 });
  await c.call(P, PKG, "bump", { g, k: 0, n: 5 });
  const refused = await c.promote(P);
  check("refuse: promotion refused", [refused.status, refused.pending, refused.conflicts.map((x) => [norm(x.key), x.op])],
    ["refused", 3, [[["edge", g, 3], "put"]]]);
  let review = await c.review(P);
  check("refuse: still paused, not blocked", [review.status, review.pending, review.blocked, review.conflict_count], ["paused", 3, false, 0]);
  check("refuse: the parent unchanged", (await view(await globalView(), g)).e3, 3000);
  const forced = await c.promote(P, { force: true });
  check("refuse: forced", [forced.status, forced.conflicts.map((x) => [x.number, norm(x.key), x.op, !!x.raced])],
    ["promoted", [[1, ["edge", g, 3], "put", false]]]);
  await waitFor("refuse: forced writes land", async () => (await view(await globalView(), g)).e3, 333);

  /* Blocked by Tetris, then discarded. */
  g = 21;
  await writeBase(GW, g);
  await waitFor("blocked: base", async () => (await view(await globalView(), g)).e3, 30);
  const B = await c.newPov({ safe: true, shared: false, conflicts: "refuse" });
  await c.call(GW, PKG, "put", { g, e: 3, w: 3000 });
  await waitFor("blocked: the parent's change lands", async () => (await view(await globalView(), g)).e3, 3000);
  await c.call(B, PKG, "put", { g, e: 3, w: 333 });
  await c.call(B, PKG, "bump", { g, k: 0, n: 1 });
  await waitFor("blocked: Tetris holds the POV back", async () => (await c.review(B)).blocked, true, 10_000);
  review = await c.review(B);
  check("blocked: review", [review.status, review.pending, review.blocked_on.map((x) => [norm(x.key), x.op])],
    ["normal", 2, [[["edge", g, 3], "put"]]]);
  await sleep(500);
  check("blocked: the parent unchanged", (await view(await globalView(), g)).e3, 3000);
  const discarded = await c.discard(B);
  check("blocked: discard", [discarded.discarded_updates, discarded.discarded_entries], [2, 2]);
  review = await c.review(B);
  check("blocked: after discard", [review.status, review.pending, review.blocked], ["normal", 0, false]);
  check("blocked: B reads as its parent", await view(B, g), await view(await globalView(), g));
  check("blocked: diff after discard", (await c.diff(B)).changes, []);
  /* A discard re-forks: the parent's change to edge 3 is now B's starting point. */
  await c.call(B, PKG, "put", { g, e: 3, w: 334 });
  await waitFor("blocked: a write after the discard promotes", async () => (await view(await globalView(), g)).e3, 334);
  check("blocked: no new conflict", (await c.review(B)).conflict_count, 0);

  /* Commutative updates never conflict, in either direction of a commutative parent change. */
  g = 22;
  await writeBase(GW, g);
  await waitFor("commutative: base", async () => (await view(await globalView(), g)).c0, 15);
  const C = await c.newPov({ safe: false, shared: true, conflicts: "refuse" });
  await c.call(GW, PKG, "bump", { g, k: 0, n: 100 });
  await c.call(GW, PKG, "set_count", { g, k: 2, n: 50 });
  await waitFor("commutative: the parent's changes land", async () => (await view(await globalView(), g)).c2, 50);
  await c.call(C, PKG, "bump", { g, k: 0, n: 1 });
  await c.call(C, PKG, "bump", { g, k: 2, n: 1 });
  await waitFor("commutative: C's updates promote", async () => [(await view(await globalView(), g)).c0, (await view(await globalView(), g)).c2], [116, 51]);
  review = await c.review(C);
  check("commutative: no conflict", [review.blocked, review.conflict_count], [false, 0]);
  if (failures === before) console.log("POV REVIEW OK: refusing mode");
}

/* 3. Discard releases memory. */
{
  const before = failures;
  g = 30;
  /* 4,000 paused writes by default; a server with small update pools (TSan) caps a POV's backlog
     at 1/32 of its Update Entry pool, so it asks for fewer. */
  const BATCHES = +(process.env.DISCARD_BATCHES ?? 40), BATCH = +(process.env.DISCARD_BATCH ?? 100);
  await sleep(2000);
  const base = await pools();
  const D = await c.newPov({ safe: false, shared: false });
  await c.pause(D);
  for (let b = 0; b < BATCHES; ++b) {
    await c.callBatch(D, PKG, "put", Array.from({ length: BATCH }, (_, i) => ({ g, e: b * BATCH + i, w: i })));
  }
  const full = await pools();
  check("discard: the writes take pool", full.entries - base.entries >= BATCHES * BATCH, true);
  check("discard: diff covers the writes", (await c.diff(D)).updates, BATCHES);
  const discarded = await c.discard(D);
  check("discard: result", [discarded.discarded_updates, discarded.discarded_entries], [BATCHES, BATCHES * BATCH]);
  const freed = await waitFor("discard: the update pools fall back",
    async () => { const p = await pools(); return p.entries - base.entries < BATCHES * BATCH / 10 && p.updates - base.updates < BATCHES; },
    true, 30_000);
  const end = await pools();
  console.log(`discard: Update Entry pool ${base.entries} before, ${full.entries} with ${BATCHES * BATCH} paused writes, ${end.entries} after the discard`);
  check("discard: D reads as its parent", [await c.call(D, PKG, "get", { g, e: 7 }), (await c.diff(D)).changes, (await c.review(D)).pending], [-1, [], 0]);
  await c.call(D, PKG, "put", { g, e: 7, w: 70 });
  check("discard: D still takes writes", await c.call(D, PKG, "get", { g, e: 7 }), 70);
  if (freed && failures === before) console.log("POV REVIEW OK: discard");
}

/* 4. Errors. */
{
  const before = failures;
  const S = await c.newPov({ safe: true, shared: true });
  await expectError("discard a shared POV", c.discard(S), /only the session that made a private POV/);
  const mine = await c.newPov({ safe: false, shared: false });
  const other = await connect(process.env.ORLY_URL);
  await other.newSession();
  await expectError("discard another session's POV", other.discard(mine), /only the session that made a private POV/);
  other.close();
  await expectError("diff since a save point", c.send(`diff_pov {${mine}} <{.since: "v1"}>;`), /#745/);
  await expectError("an unknown option", c.send(`diff_pov {${mine}} <{.limt: 3}>;`), /unknown option/);
  await expectError("a bad conflict mode", c.send("new fast private pov <{.conflicts: \"maybe\"}>;"), /report/);
  await expectError("a limit too big", c.diff(mine, { limit: 100000 }), /limit/);
  if (failures === before) console.log("POV REVIEW OK: errors");
}

c.close();
if (failures) {
  console.log(`POV REVIEW FAIL: ${failures} checks failed`);
  process.exit(1);
}
console.log("POV REVIEW OK");
