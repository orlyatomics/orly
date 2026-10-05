/** Concurrent-writer pool-pressure smoke (#584); run by run-pool-pressure.sh.
 *
 * K writers, each on its own connection and session, write distinct keys via
 * sample.write_val -- first all into ONE shared POV, then each into its own
 * private POV -- while the reporting port is polled for Update / Data Layer
 * pool occupancy. Fails if any write errors, the server stops answering, or
 * the Update pool hasn't drained once the writers stop. */

import net from "node:net";
import { connect } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const REPORT_PORT = +process.env.ORLY_REPORT_PORT;
const K = +(process.env.K ?? 8);
const SECS = +(process.env.SECS ?? 30);
/* Each phase also stops at MAX_WRITES, so a fast runner doesn't write several
   times what a slow one does. Every write lands on the server's 256 MB mem-sim
   disk, and a full disk aborts the server, so uncapped the smoke flaked on the
   fastest runners only. #584 aborted within ~2,000 writes, far below the cap. */
const MAX_WRITES = +(process.env.MAX_WRITES ?? 60000);

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

function pool(body, name) {
  const m = body?.match(new RegExp(`^${name} = (\\d+) / (\\d+)`, "m"));
  return m ? { used: +m[1], size: +m[2] } : null;
}

async function phase(label, povFor) {
  let writes = 0, stop = false, peak = 0;
  const failures = [];
  const writer = async (w) => {
    const c = await connect(URL);
    await c.newSession();
    const pov = await povFor(c);
    for (let i = 0; !stop && writes < MAX_WRITES; ++i) {
      try {
        await c.call(pov, "sample", "write_val", { n: w * 10_000_000 + i, x: i });
        ++writes;
      } catch (err) {
        failures.push(`writer ${w}: ${err?.message ?? err}`);
        break;
      }
    }
    c.close();
  };
  const writers = Array.from({ length: K }, (_, w) => writer(w));
  const t0 = Date.now();
  while (Date.now() - t0 < SECS * 1000 && writes < MAX_WRITES && failures.length === 0) {
    await new Promise((r) => setTimeout(r, 1000));
    const body = await report();
    const upd = pool(body, "Update Pool"), layers = pool(body, "Repo Data Layer Pool");
    if (!upd) {
      failures.push("reporting port stopped answering");
      break;
    }
    peak = Math.max(peak, upd.used);
    console.log(`  ${label} t=${((Date.now() - t0) / 1000).toFixed(0)}s writes=${writes} update_pool=${upd.used}/${upd.size} data_layers=${layers?.used}/${layers?.size}`);
  }
  stop = true;
  await Promise.all(writers);
  const elapsed = (Date.now() - t0) / 1000;
  /* Dead layers must actually be freed: before #584's fix the layer cleaner
     wedged and occupancy kept climbing after the writes stopped. */
  let drained = null;
  for (let i = 0; i < 20 && failures.length === 0; ++i) {
    await new Promise((r) => setTimeout(r, 500));
    const upd = pool(await report(), "Update Pool");
    if (!upd) { failures.push("reporting port stopped answering after the writes"); break; }
    drained = upd;
    if (upd.used < upd.size / 2) break;
  }
  if (failures.length === 0 && !(drained && drained.used < drained.size / 2)) {
    failures.push(`Update pool did not drain after the writes stopped: ${drained?.used}/${drained?.size}`);
  }
  console.log(`${label}: ${writes} writes, peak Update pool ${peak}, after drain ${drained?.used}`);
  /* For tools/maint/ab_bench.py. A phase can stop at MAX_WRITES before SECS,
     so compare writes per second, not the write count. */
  const metric = label.toLowerCase().replace(/[^a-z0-9]+/g, "_");
  console.log(`METRIC ${metric}_writes_per_s ${(writes / elapsed).toFixed(1)}`);
  console.log(`METRIC ${metric}_peak_update_pool ${peak}`);
  if (failures.length) {
    console.error(`POOL PRESSURE FAIL (${label}):\n  ${failures.join("\n  ")}`);
    process.exit(1);
  }
  if (writes < 1000) {
    console.error(`POOL PRESSURE FAIL (${label}): only ${writes} writes; the load never built up`);
    process.exit(1);
  }
}

const setup = await connect(URL);
await setup.newSession();
await setup.install("sample", 1);
const shared = await setup.newPov({ safe: true, shared: true });

await phase("shared POV", async () => shared);
await phase("POV per writer", (c) => c.newPov({ safe: true, shared: false }));
setup.close();
console.log("pool pressure smoke: ok");
process.exit(0);
