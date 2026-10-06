/** Read-budget smoke, TS driver (#694); run by run-read-budget.sh after read_budget.py has
 *  written the rows. A read over the budget must reject with ReadTooLargeError (a subclass of
 *  OrlyError, not WriteTooLargeError); one under it must answer. */

import { connect, OrlyError, ReadTooLargeError, WriteTooLargeError } from "../ts/dist/index.js";

const ROWS = +process.env.ROWS;
const BUDGETED = process.env.BUDGETED === "1";
const fail = (msg) => {
  console.error(`READ BUDGET FAIL (ts): ${msg}`);
  process.exit(1);
};

const c = await connect(process.env.ORLY_URL);
await c.newSession();
await c.install("read_budget", 1);
const pov = await c.newPov();
await c.callBatch(pov, "read_budget", "write_val", Array.from({ length: 1000 }, (_, n) => ({ n, x: 1 })));
// ROWS point reads are over the row budget whether or not this POV sees the Python driver's rows.
if (BUDGETED) {
  try {
    await c.call(pov, "read_budget", "sum_points", { last: ROWS - 1 });
    fail("a point-read loop over the budget was answered");
  } catch (err) {
    if (!(err instanceof ReadTooLargeError)) {
      fail(`over the budget failed with ${err?.name}, not ReadTooLargeError: ${JSON.stringify(err?.reply) ?? err}`);
    }
    if (!(err instanceof OrlyError) || err instanceof WriteTooLargeError) {
      fail("ReadTooLargeError has the wrong class hierarchy");
    }
    if (err.reply?.status !== "read_too_large") fail(`status ${err.reply?.status}`);
  }
}
const n = Number(await c.call(pov, "read_budget", "sum_points", { last: 99 }));
if (n !== 100) fail(`sum_points over 100 rows read ${n}, want 100`);
c.close();
console.log(`READ BUDGET OK (ts, budgeted=${+BUDGETED})`);
