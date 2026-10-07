/** WebSocket concurrency smoke (#761); run by run-ws-scale.sh.
 *
 * Loads GROUPS groups of six keys, then, for each session count in LEVELS, opens that many
 * WebSocket sessions (each its own connection, session and shared POV; the POVs are made once
 * and reused at every level, as a new POV per reader per level, ~700 a run, ran the 1 GiB server
 * out of memory) and has each run reads in a
 * closed loop for SECS seconds: point reads of one random key, then small prefix reads that walk
 * one random group. Every read is checked. The sessions are spread over WORKERS worker threads so
 * the client isn't the bottleneck.
 *
 * Prints, per read kind and session count, reads per second and p50/p99 latency as METRIC lines
 * for tools/maint/ab_bench.py: ws_<kind>_rps_s<N>, ws_<kind>_p50_ms_s<N>, ws_<kind>_p99_ms_s<N>.
 *
 * KINDS may also name "spin": a read that only computes (a reduce over [0..SPIN_N]).
 *
 * SCALE_CHECK=<n>:<x> fails the run unless SCALE_KIND (default point) reads/s at n sessions is
 * at least x times the rate at one session. Before #761 a statement held a WebSocket I/O thread
 * while it ran, so with --num_ws_threads=1 sixteen sessions ran spins no faster than one. */

import { Worker, isMainThread, parentPort, workerData } from "node:worker_threads";
import { connect } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const GROUPS = +(process.env.GROUPS ?? 2000);
const PER_GROUP = 6;
const SPIN_N = +(process.env.SPIN_N ?? 20000);
const value = (g, n) => g * 10 + n;

if (!isMainThread) {
  /* A worker: open its sessions, wait for "go", read until the deadline, report. */
  const { povs, kind } = workerData;
  const clients = [];
  for (const pov of povs) {
    const c = await connect(URL);
    await c.newSession();
    clients.push({ c, pov });
  }
  parentPort.postMessage({ ready: true });
  const { stopAt } = await new Promise((r) => parentPort.once("message", r));
  const lat = [];
  const failures = [];
  const loop = async ({ c, pov }) => {
    while (Date.now() < stopAt && failures.length < 10) {
      const g = Math.floor(Math.random() * GROUPS);
      const t0 = performance.now();
      try {
        if (kind === "point") {
          const n = Math.floor(Math.random() * PER_GROUP);
          const got = Number(await c.call(pov, "ws_scale", "get", { g, n }));
          lat.push(performance.now() - t0);
          if (got !== value(g, n)) failures.push(`get ${g},${n} read ${got}, want ${value(g, n)}`);
        } else if (kind === "spin") {
          const got = Number(await c.call(pov, "ws_scale", "spin", { n: SPIN_N }));
          lat.push(performance.now() - t0);
          if (got !== SPIN_N * (SPIN_N + 1) / 2) failures.push(`spin ${SPIN_N} read ${got}`);
        } else {
          const got = Number(await c.call(pov, "ws_scale", "group", { g }));
          lat.push(performance.now() - t0);
          if (got !== PER_GROUP) failures.push(`group ${g} read ${got} keys, want ${PER_GROUP}`);
        }
      } catch (err) {
        failures.push(`${kind}: ${err?.message ?? err}`);
      }
    }
  };
  await Promise.all(clients.map(loop));
  for (const { c } of clients) c.close();
  parentPort.postMessage({ lat: Float64Array.from(lat), failures });
} else {

const LEVELS = (process.env.LEVELS ?? "1,3,16,64,256").split(",").map(Number);
const KINDS = (process.env.KINDS ?? "point,prefix").split(",");
const SECS = +(process.env.SECS ?? 5);
const WORKERS = +(process.env.WORKERS ?? 4);
const SCALE_CHECK = process.env.SCALE_CHECK ?? "";
const SCALE_KIND = process.env.SCALE_KIND ?? "point";

const setup = await connect(URL);
await setup.newSession();
await setup.install("ws_scale", 1);
const writer = await setup.newPov({ safe: true, shared: true });
const rows = [];
for (let g = 0; g < GROUPS; ++g) {
  for (let n = 0; n < PER_GROUP; ++n) rows.push({ g, n, v: value(g, n) });
}
/* Batches of 200; a batch refused at the memory reserve (insufficient_memory, #607) is retried
   once the merges have caught up. */
for (let i = 0; i < rows.length; i += 200) {
  for (let tries = 0; ; ++tries) {
    try {
      await setup.callBatch(writer, "ws_scale", "put", rows.slice(i, i + 200));
      break;
    } catch (err) {
      if (err?.name !== "InsufficientMemoryError" || tries >= 600) {
        console.error(`WS SCALE FAIL: loading: ${String(err?.reply?.status ?? err?.message ?? err).slice(0, 300)}`);
        process.exit(1);
      }
      await new Promise((r) => setTimeout(r, 100));
    }
  }
}
/* Wait until the last group is visible from a fresh POV, i.e. promoted to global. */
const probe = await setup.newPov({ safe: true, shared: true });
const deadline = Date.now() + 60000;
while (Number(await setup.call(probe, "ws_scale", "group", { g: GROUPS - 1 })) !== PER_GROUP) {
  if (Date.now() > deadline) {
    console.error("WS SCALE FAIL: the loaded keys never became visible");
    process.exit(1);
  }
  await new Promise((r) => setTimeout(r, 100));
}
console.log(`loaded ${rows.length} keys in ${GROUPS} groups`);
const readerPovs = [];
for (let i = 0; i < Math.max(...LEVELS); ++i) readerPovs.push(await setup.newPov({ safe: true, shared: true }));

const pct = (sorted, p) => sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))] : NaN;
const rps = {};
const failures = [];
for (const kind of KINDS) {
  for (const level of LEVELS) {
    const nw = Math.min(level, WORKERS);
    const shares = Array.from({ length: nw }, (_, i) => Math.floor(level / nw) + (i < level % nw ? 1 : 0));
    const starts = shares.map((_, i) => shares.slice(0, i).reduce((a, b) => a + b, 0));
    const workers = shares.map((sessions, i) => new Worker(new globalThis.URL(import.meta.url),
        { workerData: { povs: readerPovs.slice(starts[i], starts[i] + sessions), kind } }));
    const results = workers.map((w) => new Promise((resolve, reject) => {
      w.on("error", reject);
      w.on("message", (m) => { if (!m.ready) resolve(m); });
    }));
    await Promise.all(workers.map((w) => new Promise((resolve, reject) => {
      w.on("error", reject);
      w.once("message", resolve);
    })));
    const stopAt = Date.now() + SECS * 1000;
    for (const w of workers) w.postMessage({ stopAt });
    const done = await Promise.all(results);
    const all = [];
    for (const r of done) {
      for (const x of r.lat) all.push(x);
      failures.push(...r.failures.map((f) => `${kind} s${level}: ${f}`));
    }
    all.sort((a, b) => a - b);
    const rate = all.length / SECS;
    rps[`${kind}:${level}`] = rate;
    console.log(`${kind} reads, ${level} session(s): ${all.length} in ${SECS}s = ${rate.toFixed(0)}/s, ` +
                `p50 ${pct(all, 0.5).toFixed(2)} ms, p99 ${pct(all, 0.99).toFixed(2)} ms`);
    console.log(`METRIC ws_${kind}_rps_s${level} ${rate.toFixed(1)}`);
    console.log(`METRIC ws_${kind}_p50_ms_s${level} ${pct(all, 0.5).toFixed(3)}`);
    console.log(`METRIC ws_${kind}_p99_ms_s${level} ${pct(all, 0.99).toFixed(3)}`);
    if (failures.length) break;
  }
  if (failures.length) break;
}
setup.close();

if (failures.length) {
  console.error(`WS SCALE FAIL:\n  ${failures.slice(0, 10).join("\n  ")}`);
  process.exit(1);
}
if (SCALE_CHECK) {
  const [n, x] = SCALE_CHECK.split(":").map(Number);
  const one = rps[`${SCALE_KIND}:1`], many = rps[`${SCALE_KIND}:${n}`];
  if (!(one > 0) || !(many >= 0)) {
    console.error(`WS SCALE FAIL: SCALE_CHECK needs ${SCALE_KIND} reads at 1 and ${n} sessions`);
    process.exit(1);
  }
  const ratio = many / one;
  console.log(`${SCALE_KIND} reads/s at ${n} sessions / at 1 session = ${ratio.toFixed(2)} (want >= ${x})`);
  if (ratio < x) {
    console.error(`WS SCALE FAIL: ${n} sessions read only ${ratio.toFixed(2)}x as fast as one`);
    process.exit(1);
  }
}
console.log("ws scale smoke: ok");
process.exit(0);
}
