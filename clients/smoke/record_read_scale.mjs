/** Record-read concurrency smoke (#798); run by run-record-read-scale.sh.
 *
 * Loads PREFIXES prefixes of four str values and four record values (six str fields each), then,
 * for each kind in KINDS ("strs", "recs") and each session count in LEVELS, has that many
 * WebSocket sessions (each its own connection, session and shared POV) send batches of BATCH
 * prefix reads back to back for SECS seconds. Every result is checked.
 *
 * Before #798, turning record values into Vars and back copied singleton types (str, and the
 * unknown type every default-constructed TType holds) many times per field, and each copy bumped
 * one reference count shared by every thread, so sessions reading records on several cores
 * bounced those cache lines between them: reading four records cost ~4x the server CPU per call
 * at 8 sessions that it cost at 1 on a 16-core arm64 box, while str values stayed nearly flat.
 *
 * Prints, per kind and session count, calls per second and the server's CPU per call (from
 * /proc/ORLYI_PID/stat; CLK_TCK ticks per second) as METRIC lines for tools/maint/ab_bench.py:
 * rr_<kind>_cps_s<N> and rr_<kind>_cpu_us_s<N>, plus rr_<kind>_cpu_growth_s<N>, the CPU per call
 * at N sessions over the CPU per call at 1.
 *
 * GROWTH_CHECK=<n>:<x> fails the run unless recs' CPU per call at n sessions is at most x times
 * its CPU per call at LEVELS[0] sessions. (Measured against strs' growth instead, the check
 * was noisier: on 4-core CI runners strs' own growth swings 0.7-1.4x between runs.) */

import { readFileSync } from "node:fs";
import { Worker, isMainThread, parentPort, workerData } from "node:worker_threads";
import { connect } from "../ts/dist/index.js";

const URL = process.env.ORLY_URL;
const PREFIXES = +(process.env.PREFIXES ?? 5000);
const PER_PREFIX = 4;
const BATCH = +(process.env.BATCH ?? 8);
const FILLER = "0123456789abcdef0123456789abcdef";
const prefix = (i) => `a${String(i % PREFIXES).padStart(6, "0")}`;

/* Checks one call's result; returns an error string or null. */
const check = (kind, a, got) => {
  if (!Array.isArray(got) || got.length !== PER_PREFIX) {
    return `${kind} ${a} read ${JSON.stringify(got)?.slice(0, 200)}, want ${PER_PREFIX} values`;
  }
  for (const v of got) {
    if (kind === "strs" ? v !== FILLER : (v?.src !== a || v?.ent !== FILLER || v?.s !== FILLER)) {
      return `${kind} ${a} read ${JSON.stringify(v).slice(0, 200)}`;
    }
  }
  return null;
};

if (!isMainThread) {
  /* A worker: open its sessions, wait for "go", read until the deadline, report. */
  const { povs, kind, seed } = workerData;
  const clients = [];
  for (const pov of povs) {
    const c = await connect(URL);
    await c.newSession();
    clients.push({ c, pov });
  }
  parentPort.postMessage({ ready: true });
  const { stopAt } = await new Promise((r) => parentPort.once("message", r));
  let calls = 0;
  const failures = [];
  const loop = async ({ c, pov }, j) => {
    let i = (seed + j) * 7919;
    while (Date.now() < stopAt && failures.length < 10) {
      const args = Array.from({ length: BATCH }, () => ({ a: prefix(i++) }));
      try {
        const got = await c.callBatch(pov, "record_read_scale", kind, args);
        args.forEach(({ a }, k) => {
          const err = check(kind, a, got?.[k]);
          if (err) failures.push(err);
        });
        calls += BATCH;
      } catch (err) {
        failures.push(`${kind}: ${err?.message ?? err}`);
      }
    }
  };
  await Promise.all(clients.map(loop));
  for (const { c } of clients) c.close();
  parentPort.postMessage({ calls, failures });
} else {

const LEVELS = (process.env.LEVELS ?? "1,2,4,8").split(",").map(Number);
const KINDS = (process.env.KINDS ?? "strs,recs").split(",");
const SECS = +(process.env.SECS ?? 5);
const WORKERS = +(process.env.WORKERS ?? 4);
const ORLYI_PID = process.env.ORLYI_PID;
const CLK_TCK = +(process.env.CLK_TCK ?? 100);
const GROWTH_CHECK = process.env.GROWTH_CHECK ?? "";

/* orlyi's user + system CPU so far, in seconds. */
const cpuSecs = () => {
  const stat = readFileSync(`/proc/${ORLYI_PID}/stat`, "utf8");
  const fields = stat.slice(stat.lastIndexOf(")") + 2).split(" ");
  return (Number(fields[11]) + Number(fields[12])) / CLK_TCK;
};

const setup = await connect(URL);
await setup.newSession();
await setup.install("record_read_scale", 1);
const writer = await setup.newPov({ safe: true, shared: true });
const rows = [];
for (let i = 0; i < PREFIXES * PER_PREFIX; ++i) rows.push({ a: prefix(i), b: `b${i}` });
/* Batches of 250; a batch refused at the memory reserve (insufficient_memory, #607) is retried
   once the merges have caught up. */
for (let i = 0; i < rows.length; i += 250) {
  for (let tries = 0; ; ++tries) {
    try {
      await setup.callBatch(writer, "record_read_scale", "put", rows.slice(i, i + 250));
      break;
    } catch (err) {
      if (err?.name !== "InsufficientMemoryError" || tries >= 600) {
        console.error(`RECORD READ SCALE FAIL: loading: ${String(err?.reply?.status ?? err?.message ?? err).slice(0, 300)}`);
        process.exit(1);
      }
      await new Promise((r) => setTimeout(r, 100));
    }
  }
}
/* Wait until the last prefix is visible from a fresh POV, i.e. promoted to global. */
const probe = await setup.newPov({ safe: true, shared: true });
const deadline = Date.now() + 120000;
for (const kind of KINDS) {
  while (check(kind, prefix(PREFIXES - 1), await setup.call(probe, "record_read_scale", kind, { a: prefix(PREFIXES - 1) }))) {
    if (Date.now() > deadline) {
      console.error("RECORD READ SCALE FAIL: the loaded keys never became visible");
      process.exit(1);
    }
    await new Promise((r) => setTimeout(r, 100));
  }
}
console.log(`loaded ${PREFIXES} prefixes x ${PER_PREFIX} keys of each kind`);
const readerPovs = [];
for (let i = 0; i < Math.max(...LEVELS); ++i) readerPovs.push(await setup.newPov({ safe: true, shared: true }));

const cpuPerCall = {};
const failures = [];
for (const kind of KINDS) {
  for (const level of LEVELS) {
    const nw = Math.min(level, WORKERS);
    const shares = Array.from({ length: nw }, (_, i) => Math.floor(level / nw) + (i < level % nw ? 1 : 0));
    const starts = shares.map((_, i) => shares.slice(0, i).reduce((a, b) => a + b, 0));
    const workers = shares.map((sessions, i) => new Worker(new globalThis.URL(import.meta.url),
        { workerData: { povs: readerPovs.slice(starts[i], starts[i] + sessions), kind, seed: starts[i] } }));
    const results = workers.map((w) => new Promise((resolve, reject) => {
      w.on("error", reject);
      w.on("message", (m) => { if (!m.ready) resolve(m); });
    }));
    await Promise.all(workers.map((w) => new Promise((resolve, reject) => {
      w.on("error", reject);
      w.once("message", resolve);
    })));
    const cpu0 = cpuSecs();
    const t0 = Date.now();
    const stopAt = t0 + SECS * 1000;
    for (const w of workers) w.postMessage({ stopAt });
    const done = await Promise.all(results);
    const secs = (Date.now() - t0) / 1000;
    const cpu = cpuSecs() - cpu0;
    let calls = 0;
    for (const r of done) {
      calls += r.calls;
      failures.push(...r.failures.map((f) => `${kind} s${level}: ${f}`));
    }
    const cps = calls / secs;
    const us = calls ? cpu / calls * 1e6 : NaN;
    cpuPerCall[`${kind}:${level}`] = us;
    const growth = us / cpuPerCall[`${kind}:${LEVELS[0]}`];
    console.log(`${kind}, ${level} session(s): ${calls} calls in ${secs.toFixed(1)}s = ${cps.toFixed(0)}/s, ` +
                `orlyi CPU ${(cpu / secs * 100).toFixed(0)}% = ${us.toFixed(1)} us/call ` +
                `(${growth.toFixed(2)}x the ${LEVELS[0]}-session cost)`);
    console.log(`METRIC rr_${kind}_cps_s${level} ${cps.toFixed(1)}`);
    console.log(`METRIC rr_${kind}_cpu_us_s${level} ${us.toFixed(2)}`);
    if (level !== LEVELS[0]) console.log(`METRIC rr_${kind}_cpu_growth_s${level} ${growth.toFixed(3)}`);
    if (failures.length) break;
  }
  if (failures.length) break;
}
setup.close();

if (failures.length) {
  console.error(`RECORD READ SCALE FAIL:\n  ${failures.slice(0, 10).join("\n  ")}`);
  process.exit(1);
}
if (GROWTH_CHECK) {
  const [n, x] = GROWTH_CHECK.split(":").map(Number);
  const one = cpuPerCall[`recs:${LEVELS[0]}`], many = cpuPerCall[`recs:${n}`];
  if (!(one > 0) || !(many > 0)) {
    console.error(`RECORD READ SCALE FAIL: GROWTH_CHECK needs recs at ${LEVELS[0]} and ${n} sessions`);
    process.exit(1);
  }
  const growth = many / one;
  console.log(`recs CPU per call at ${n} sessions / at ${LEVELS[0]} = ${growth.toFixed(2)} (want <= ${x})`);
  if (growth > x) {
    console.error(`RECORD READ SCALE FAIL: recs cost ${growth.toFixed(2)}x the CPU per call at ${n} sessions`);
    process.exit(1);
  }
}
console.log("record read scale smoke: ok");
process.exit(0);
}
