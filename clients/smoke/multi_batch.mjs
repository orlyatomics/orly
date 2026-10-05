/** Mixed-batch smoke, TS driver (#255); run by run-multi-batch.sh. */
import { connect, OrlyError } from "../ts/dist/index.js";

const assert = (cond, msg) => { if (!cond) { console.error(`MULTI BATCH FAIL: ${msg}`); process.exit(1); } };
const c = await connect(process.env.ORLY_URL);
await c.newSession();
await c.install("multi", 1);
const pov = await c.newPov();

const results = await c.callMany(pov, [
  ["multi", "write_name", { n: 11, s: "beta" }],
  ["multi", "write_val", { n: 11, x: 110 }],
]);
assert(JSON.stringify(results) === JSON.stringify(["named", true]), `results ${JSON.stringify(results)}`);
assert((await c.call(pov, "multi", "read_val", { n: 11 })) === 110, "write_val landed");
assert((await c.call(pov, "multi", "read_name", { n: 11 })) === "beta", "write_name landed");
console.log("ts: mixed batch landed:", JSON.stringify(results));

let refused = false;
try {
  await c.callMany(pov, [["multi", "write_name", { n: 12, s: "gamma" }], ["no_such_pkg", "write_val", { n: 12, x: 1 }]]);
} catch (err) {
  refused = err instanceof OrlyError;
}
assert(refused, "a batch with a bad package was accepted");
assert((await c.call(pov, "multi", "read_name", { n: 12 })) === null, "a failed batch left a write behind");
console.log("ts: failed batch left nothing behind");
c.close();
