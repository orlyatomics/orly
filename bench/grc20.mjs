import assert from "node:assert/strict";
import { connect, InsufficientMemoryError } from "../clients/ts/dist/index.js";

const levels = (process.env.GRC_LEVELS ?? "1,4,16").split(",").map(Number);
const entities = +(process.env.GRC_ENTITIES ?? 128);
const revisions = +(process.env.GRC_REVISIONS ?? 4);
const seconds = +(process.env.GRC_SECS ?? 5);
const degree = 4;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const target = (n, edge) => (n + edge + 1) % entities;
const clients = [];
async function client() {
  const c = await connect(process.env.ORLY_URL, { retries: 8 });
  clients.push(c);
  await c.newSession();
  return c;
}
async function write(c, pov, method, args) {
  for (let tries = 0; ; ++tries) {
    try {
      return await c.call(pov, "grc20", method, args);
    } catch (err) {
      if (!(err instanceof InsufficientMemoryError) || tries >= 600) throw err;
      await sleep(50);
    }
  }
}
async function oneHop(c, pov, level, n) {
  const entity = `source-${level}-${n}`;
  const props = await c.call(pov, "grc20", "props_of", { entity });
  assert.deepEqual([...props].sort(), ["edge-0", "edge-1", "edge-2", "edge-3"]);
  for (let edge = 0; edge < degree; ++edge) {
    const op = await c.call(pov, "grc20", "resolve", { entity, property: `edge-${edge}` });
    const expected = target(n, edge);
    assert.deepEqual(op, { Relation: `target-${expected}` });
    const name = await c.call(pov, "grc20", "display", { entity: op.Relation, property: "name" });
    assert.equal(name, `name-${expected}`);
  }
}

try {
  assert(levels.length > 0 && levels.every((n) => Number.isInteger(n) && n > 0));
  assert(Number.isInteger(entities) && entities > degree);
  assert(Number.isInteger(revisions) && revisions > 0);
  assert(seconds > 0 && Number.isFinite(seconds));
  const setup = await client();
  await setup.install("grc20", 1);
  const pov = await setup.newPov({ safe: true, shared: true });
  for (let n = 0; n < entities; ++n) {
    await write(setup, pov, "create_entity",
      { entity: `target-${n}`, ts: 1, editor: "bench", kind: "BenchTarget" });
    await write(setup, pov, "register_prop", { entity: `target-${n}`, property: "name" });
    await write(setup, pov, "set_text",
      { entity: `target-${n}`, property: "name", text: `name-${n}`, ts: 1, editor: "bench" });
  }
  for (const level of levels) {
    for (let n = 0; n < entities; ++n) {
      await write(setup, pov, "create_entity",
        { entity: `source-${level}-${n}`, ts: 1, editor: "bench", kind: "BenchSource" });
      for (let edge = 0; edge < degree; ++edge) {
        await write(setup, pov, "register_prop", { entity: `source-${level}-${n}`, property: `edge-${edge}` });
      }
    }
    const sessions = await Promise.all(Array.from({ length: level }, client));
    let written = 0;
    const started = performance.now();
    await Promise.all(sessions.map(async (c, w) => {
      for (let revision = 1; revision <= revisions; ++revision) {
        for (let n = w; n < entities; n += level) {
          for (let edge = 0; edge < degree; ++edge) {
            await write(c, pov, "set_relation", { entity: `source-${level}-${n}`,
              property: `edge-${edge}`, target: `target-${target(n, edge)}`,
              ts: revision, editor: "bench" });
            ++written;
          }
        }
      }
    }));
    const elapsed = (performance.now() - started) / 1000;
    assert.equal(written, entities * degree * revisions);
    console.log(`METRIC ingest_events_per_s_c${level} ${(written / elapsed).toFixed(3)}`);
    // Verify the whole fixture before timing reads; acknowledgements include POV-local writes.
    for (let n = 0; n < entities; ++n) {
      await oneHop(setup, pov, level, n);
      for (let edge = 0; edge < degree; ++edge) {
        const count = await setup.call(pov, "grc20", "event_count",
          { entity: `source-${level}-${n}`, property: `edge-${edge}` });
        assert.equal(Number(count), revisions);
      }
    }
    let reads = 0;
    const reading = performance.now();
    const stopAt = reading + seconds * 1000;
    await Promise.all(sessions.map(async (c, w) => {
      for (let n = w; performance.now() < stopAt; n = (n + level) % entities) {
        await oneHop(c, pov, level, n);
        ++reads;
      }
    }));
    assert(reads > 0);
    console.log(`METRIC one_hop_per_s_c${level} ${(reads / ((performance.now() - reading) / 1000)).toFixed(3)}`);
    for (const c of sessions) c.close();
  }
  console.log("GRC-20 ingest and one-hop checks: ok");
} finally {
  for (const c of clients) c.close();
}
