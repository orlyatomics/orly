/** `keys ... changed between (a, b)` smoke (#889), TS driver; run by run-changed-between.sh, which
 *  starts orlyi with --retention_versions so the global POV keeps the history to read.
 *
 *  Edges 1..6 are written with weights e*10 and settled to disk (version A). Then edge 2 is
 *  rewritten, 3 is rewritten to the same weight, 4 is deleted and 7 is added (version B). Between
 *  A and B exactly 2, 4 and 7 changed, in either order of the two versions. */

import { connect } from "../ts/dist/index.js";

const PKG = "changed_between";
const GLOBAL = "C4EF7C46-28C5-4000-8CCD-C8E799E2C3F3";
const fail = (msg) => {
  console.error(`CHANGED BETWEEN FAIL: ${msg}`);
  process.exit(1);
};
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const c = await connect(process.env.ORLY_URL);
await c.newSession();
await c.install(PKG, 1);

const durable = async () => (await c.send(`durable_version {${GLOBAL}};`)).durable_version;

/* Wait until the global POV has the writes on disk and its durable version has stopped moving. */
async function settle(check) {
  const deadline = Date.now() + 120_000;
  let last = null, stable = 0;
  for (;;) {
    const v = await durable();
    stable = v !== null && v === last ? stable + 1 : 0;
    last = v;
    if (stable >= 4 && (await check())) return v;
    if (Date.now() > deadline) fail("the writes never settled on the global POV");
    await sleep(250);
  }
}

/* A call on the global POV has to name a version to read at (`.at`); the changed-between versions are its own. */
let atVersion = 0;
const rows = async (method, args, opts = ` <{.at: ${atVersion}}>`) => {
  const text = `try {${GLOBAL}} ${PKG} ${method} <{${Object.entries(args).map(([k, v]) => `.${k}: ${v}`).join(", ")}}>${opts};`;
  return c.send(text);
};

const pov = await c.newPov();
for (let e = 1; e <= 6; e++) await c.call(pov, PKG, "put", { g: 1, e, w: e * 10 });
const versionA = await settle(async () => true);
const probeA = versionA;

await c.call(pov, PKG, "put", { g: 1, e: 2, w: 21 });
await c.call(pov, PKG, "put", { g: 1, e: 3, w: 30 });
await c.call(pov, PKG, "remove", { g: 1, e: 4 });
await c.call(pov, PKG, "put", { g: 1, e: 7, w: 70 });
const versionB = await settle(async () => true);
if (!(versionB > probeA)) fail(`version did not advance: ${probeA} -> ${versionB}`);


atVersion = versionB;
const fwd = await rows("edges_changed", { g: 1, a: probeA, b: versionB });
console.log("forward", JSON.stringify(fwd));
if (!same(fwd.map((r) => r.e), [2, 4, 7])) fail(`changed ${probeA}..${versionB}: ${JSON.stringify(fwd)}`);
const back = await rows("edges_changed", { g: 1, a: versionB, b: probeA });
if (!same(back.map((r) => r.e), [2, 4, 7])) fail(`reversed order: ${JSON.stringify(back)}`);

const same_v = await rows("edges_changed", { g: 1, a: versionB, b: versionB });
if (!same(same_v, [])) fail(`equal versions: ${JSON.stringify(same_v)}`);

const after = await rows("edges_changed_after", { g: 1, a: probeA, b: versionB, last: 2 });
if (!same(after.map((r) => r.e), [4, 7])) fail(`after 2: ${JSON.stringify(after)}`);

const tooNew = await rows("edges_changed", { g: 1, a: probeA, b: versionB + 100000 }).then(
  () => null,
  (e) => e,
);
if (!tooNew || !/version_unavailable|VersionUnavailable|version/i.test(String(tooNew.status ?? tooNew.message ?? tooNew)))
  fail(`a version past the newest should be version_unavailable, got ${tooNew && (tooNew.message ?? JSON.stringify(tooNew))}`);

/* A private POV reads its parent, not a version of it, so it is refused rather than wrong. */
const priv = await c.newPov({ parent: pov }).catch(() => pov);
const refused = await c.call(priv, PKG, "edges_changed", { g: 1, a: probeA, b: versionB }).then(
  () => null,
  (e) => e,
);
if (!refused) fail("changed between on a private POV should be refused");

console.log("changed between ok");
await c.close?.();
process.exit(0);
