/** Batch-promotion smoke, TS driver (#751); run by run-batch-promotion.sh. callBatch and
    callMany (sizes 1, 2, 8) with conditional writes, on all four POV flavours, checked after
    promotion through a fresh POV off global; each POV must still take writes afterwards. */
import { connect } from "../ts/dist/index.js";

const PKG = "batch_promotion";
const TIMEOUT_MS = Number(process.env.PROMOTION_TIMEOUT_S ?? 60) * 1000;
const fail = (msg) => { console.error(`BATCH PROMOTION FAIL (ts): ${msg}`); process.exit(1); };
const getter = { put_cond: "get_k", put_ci: "get_c", guard: "get_g", put_plain: "get_p" };
const argsFor = (method, k, n) => {
  if (method === "put_cond") return [{ k, v: `v${n}` }, `v${n}`];
  if (method === "put_ci") return [{ k, x: n }, n > 5 ? 1 : 0];
  if (method === "guard") return [{ k }, true];
  return [{ k, v: `p${n}` }, `p${n}`];
};

const c = await connect(process.env.ORLY_URL);
await c.newSession();
await c.install(PKG, 1);
const reader = await connect(process.env.ORLY_URL);
await reader.newSession();

async function waitPromoted(expected, what) {
  const deadline = Date.now() + TIMEOUT_MS;
  for (;;) {
    const pov = await reader.newPov({ safe: false, shared: false });
    const got = await reader.callMany(pov, expected.map(([g, k]) => [PKG, g, { k }]));
    const missing = expected.filter(([, , want], i) => JSON.stringify(got[i]) !== JSON.stringify(want));
    if (missing.length === 0) { console.log(`ts: ${what}: ${expected.length} writes promoted`); return; }
    if (Date.now() > deadline) {
      fail(`${what}: ${missing.length} of ${expected.length} acknowledged writes never reached global, e.g. ${JSON.stringify(missing.slice(0, 3))}`);
    }
    await new Promise((r) => setTimeout(r, 500));
  }
}

for (const [safe, shared] of [[true, true], [true, false], [false, true], [false, false]]) {
  const flav = `${safe ? "safe" : "fast"}-${shared ? "shared" : "private"}`;
  const pov = await c.newPov({ safe, shared });
  const expected = [];
  let n = 0;
  for (const size of [1, 2, 8]) {
    for (const method of ["put_cond", "put_ci"]) {
      const calls = [];
      for (let i = 0; i < size; ++i) {
        const [args, want] = argsFor(method, `ts-${flav}-${method}-${size}-${i}`, ++n);
        calls.push(args);
        expected.push([getter[method], args.k, want]);
      }
      const res = await c.callBatch(pov, PKG, method, calls);
      if (!Array.isArray(res) || res.length !== size) fail(`${flav}: callBatch ${method} x${size} returned ${JSON.stringify(res)}`);
    }
    const mixed = [];
    for (let i = 0; i < size; ++i) {
      const method = Object.keys(getter)[i % 4];
      const [args, want] = argsFor(method, `ts-${flav}-mixed-${size}-${i}`, ++n);
      mixed.push([PKG, method, args]);
      expected.push([getter[method], args.k, want]);
    }
    const res = await c.callMany(pov, mixed);
    if (res.length !== size) fail(`${flav}: callMany x${size} returned ${JSON.stringify(res)}`);
  }
  await waitPromoted(expected, `${flav} batches`);
  const k = `ts-${flav}-after`;
  try {
    await c.call(pov, PKG, "put_cond", { k, v: "after" });
  } catch (err) {
    fail(`${flav}: the POV refused a write after its batches promoted: ${err}`);
  }
  await waitPromoted([["get_k", k, "after"]], `${flav} later write`);
}
console.log("ts: batch promotion OK on all four POV flavours");
c.close();
reader.close();
