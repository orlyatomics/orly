/** POV read-your-own-writes smoke (#791); run by run-pov-own-writes.sh.
 *
 *  A POV must read its own writes, deletes included, before Tetris promotes them to its parent.
 *  It used not to see its delete or overwrite of a key an ancestor held, nor its `+=` on an
 *  ancestor's counter: the read merged the repo chain by sequence number, which each repo
 *  numbers for itself, so an ancestor's older entry could outrank the POV's newer one.
 *
 *  For each writing POV P -- safe or fast, shared or private -- whose parent holds edges
 *  0..5 of a group and three counters (15, 15, 14), either a paused shared POV (so its rows stay
 *  there) or the global POV (rows promoted to it, as in the issue): pause P, then in P delete
 *  edge 4, overwrite edge 3, delete and re-put edge 1, put and delete edge 50, put edge 100,
 *  add 1 to counter 0, delete counter 1 and add 2 to it, and set counter 2 to 100. Through P,
 *  and through a private child of P when P is shared, every point read, `is known` and `keys`
 *  walk must show those writes, while the parent still shows its old rows. After P is unpaused
 *  and its writes are promoted, the parent, P and the child must all agree. */

import { connect } from "../ts/dist/index.js";

const PKG = "pov_own_writes";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
let failures = 0;
const check = (what, got, want) => {
  if (!same(got, want)) {
    ++failures;
    console.log(`POV OWN WRITES FAIL: ${what}: got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);
  }
};

const c = await connect(process.env.ORLY_URL);
await c.newSession();
await c.install(PKG, 1);

/* What a POV shows of group g: its edges, edges 1, 3, 4 and 50, and the three counters. */
async function view(pov, g) {
  const all = (await c.call(pov, PKG, "all", { g })).map((k) => k[2]);
  const get = async (e) => c.call(pov, PKG, "get", { g, e });
  const has4 = await c.call(pov, PKG, "has", { g, e: 4 });
  const counts = [];
  for (let i = 0; i < 3; ++i) counts.push(await c.call(pov, PKG, "count", { k: g * 10 + i }));
  return { all, e1: await get(1), e3: await get(3), e4: await get(4), has4, e50: await get(50), counts };
}
const before = { all: [0, 1, 2, 3, 4, 5], e1: 10, e3: 30, e4: 40, has4: true, e50: -1, counts: [15, 15, 14] };
const after = { all: [0, 1, 2, 3, 5, 100], e1: 111, e3: 333, e4: -1, has4: false, e50: -1, counts: [16, 2, 100] };

async function writeBase(pov, g) {
  await c.callBatch(pov, PKG, "put", [0, 1, 2, 3, 4, 5].map((e) => ({ g, e, w: e * 10 })));
  for (const [i, ns] of [[0, [10, 5]], [1, [10, 5]], [2, [7, 7]]]) {
    for (const n of ns) await c.call(pov, PKG, "bump", { k: g * 10 + i, n });
  }
}

/* Poll until `pov` shows `want` for group g, or fail after 30 s. */
async function waitFor(what, pov, g, want) {
  const deadline = Date.now() + 30_000;
  for (let got; !same((got = await view(await pov(), g)), want);) {
    if (Date.now() > deadline) { check(`${what} (30 s)`, got, want); return; }
    await sleep(200);
  }
}

let g = 0;
for (const parentKind of ["paused shared pov", "global"]) {
  for (const safe of [true, false]) {
    for (const shared of [true, false]) {
      ++g;
      const name = `${safe ? "safe" : "fast"} ${shared ? "shared" : "private"} pov under a ${parentKind}`;
      /* The parent, holding the group's rows. */
      let parent, parentView;
      if (parentKind === "global") {
        const w = await c.newPov();
        await writeBase(w, g);
        parentView = async () => c.newPov();
        await waitFor(`${name}: the rows reach global`, () => w, g, before);
        await waitFor(`${name}: the rows reach global`, () => c.newPov(), g, before);
      } else {
        parent = await c.newPov({ safe: true, shared: true });
        await c.pause(parent);
        await writeBase(parent, g);
        parentView = async () => parent;
      }
      const P = await c.newPov({ safe, shared, parent });
      const C = shared ? await c.newPov({ shared: false, parent: P }) : null;
      check(`${name}: P before its writes`, await view(P, g), before);
      const paused = await c.pause(P);
      if (paused !== "paused") check(`${name}: pause`, paused, "paused");

      await c.call(P, PKG, "remove", { g, e: 4 });
      await c.call(P, PKG, "put", { g, e: 3, w: 333 });
      await c.call(P, PKG, "remove", { g, e: 1 });
      await c.call(P, PKG, "put", { g, e: 1, w: 111 });
      await c.call(P, PKG, "put", { g, e: 50, w: 5 });
      await c.call(P, PKG, "remove", { g, e: 50 });
      await c.call(P, PKG, "put", { g, e: 100, w: 0 });
      await c.call(P, PKG, "bump", { k: g * 10, n: 1 });
      await c.call(P, PKG, "reset", { k: g * 10 + 1 });
      await c.call(P, PKG, "bump", { k: g * 10 + 1, n: 2 });
      await c.call(P, PKG, "set_count", { k: g * 10 + 2, n: 100 });

      /* Unpromoted: P and its child see the writes, the parent doesn't. Twice, a second apart,
         so a promotion in between would show. */
      for (let i = 0; i < 2; ++i) {
        check(`${name}: P, unpromoted`, await view(P, g), after);
        if (C) check(`${name}: P's child, unpromoted`, await view(C, g), after);
        check(`${name}: the parent, unpromoted`, await view(await parentView(), g), before);
        await sleep(1000);
      }

      /* Promoted: everyone agrees. */
      await c.unpause(P);
      await waitFor(`${name}: the parent, promoted`, parentView, g, after);
      check(`${name}: P, promoted`, await view(P, g), after);
      if (C) check(`${name}: P's child, promoted`, await view(C, g), after);
      if (!failures) console.log(`POV OWN WRITES OK: ${name}`);
    }
  }
}
c.close();
if (failures) {
  console.log(`POV OWN WRITES FAIL: ${failures} checks failed`);
  process.exit(1);
}
console.log("POV OWN WRITES OK");
