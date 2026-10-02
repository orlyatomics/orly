/** TS half of the new-POV smoke (#580); run by run-new-pov.sh. */

import { connect } from "../ts/dist/index.js";

const c = await connect(process.env.ORLY_URL);
await c.newSession();
await c.install("sample", 1);

let n = 0;
for (const safe of [true, false]) {
  for (const shared of [true, false]) {
    for (const withParent of [false, true]) {
      n += 1;
      const kind = `safe=${safe} shared=${shared} parent=${withParent}`;
      const parent = withParent ? await c.newPov() : undefined;
      const pov = await c.newPov({ safe, shared, parent });
      await c.call(pov, "sample", "write_val", { n, x: n * 11 });
      const got = await c.call(pov, "sample", "read_val", { n });
      if (got !== n * 11) {
        console.error(`SMOKE FAIL (ts) ${kind}: read ${JSON.stringify(got)}, wrote ${n * 11}`);
        process.exit(1);
      }
      console.log(`ok (ts) ${kind}`);
    }
  }
}
c.close();
