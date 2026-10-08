/** TS half of the package-upgrade smoke (#797); run by run-package-upgrade.sh.
 *
 * Once records of a type were read through one package, loading a second
 * package that declares the same record type (another package, or the next
 * version of the same one) used to corrupt the first package's record
 * descriptor, and the next read of those records crashed orlyi. Every step
 * below must read back exactly what was written, and a version whose record
 * type genuinely changed must get a clean answer, never a crash. */

import { connect } from "../ts/dist/index.js";

const c = await connect(process.env.ORLY_URL);
await c.newSession();

function fail(msg) {
  console.error(`PACKAGE UPGRADE FAIL: ${msg}`);
  process.exit(1);
}

// Records come back as {oid, p} objects; sort so order can't matter.
function norm(rows) {
  if (!Array.isArray(rows)) fail(`expected a list, got ${JSON.stringify(rows)}`);
  return JSON.stringify([...rows].sort((l, r) => (l.oid < r.oid ? -1 : l.oid > r.oid ? 1 : 0)));
}

async function read(pkg, a) {
  const pov = await c.newPov({ safe: true, shared: true });
  return c.call(pov, pkg, "rng", { a });
}

async function expectRead(step, pkg, a, want) {
  let got;
  try {
    got = norm(await read(pkg, a));
  } catch (e) {
    fail(`${step}: ${pkg}.rng(${a}) threw ${e}`);
  }
  if (got !== want) fail(`${step}: ${pkg}.rng(${a}) read ${got}, want ${want}`);
  console.log(`ok ${step}: ${pkg}.rng(${a})`);
}

async function settle() {
  // Writes go to a shared POV and must be promoted to global before a fresh
  // POV sees them.
  await new Promise((r) => setTimeout(r, 3000));
}

// Step 1: xa.1 stores records and reads them back.
await c.install("xa", 1);
const w = await c.newPov({ safe: true, shared: true });
const rows = Array.from({ length: 12 }, (_, j) => ({ a: `a${j % 3}`, b: `b${j}`, p: `p${j}` }));
await c.callBatch(w, "xa", "put", rows);
await settle();
const want = {};
for (const a of ["a0", "a1", "a2"]) {
  want[a] = norm(rows.filter((r) => r.a === a).map((r) => ({ oid: r.b, p: r.p })));
}
for (const a of ["a0", "a1", "a2"]) await expectRead("before any second install", "xa", a, want[a]);

// Step 2: another package declaring the same record type. Each package has
// its own key space, so xb writes and reads records of its own; xa must still
// read exactly its own.
await c.install("xb", 1);
{
  const pov = await c.newPov({ safe: true, shared: true });
  await c.callBatch(pov, "xb", "put", [
    { a: "a0", b: "x1", p: "q1" },
    { a: "a0", b: "x2", p: "q2" },
  ]);
  await settle();
}
const wantXb = norm([{ oid: "x1", p: "q1" }, { oid: "x2", p: "q2" }]);
for (const a of ["a0", "a1"]) await expectRead("after installing xb.1", "xa", a, want[a]);
await expectRead("after installing xb.1", "xb", "a0", wantXb);

// Step 3: the next version of the same package. Installing it replaces xa.1,
// which the server then unloads.
await c.install("xa", 2);
for (const a of ["a0", "a2"]) {
  await expectRead("after installing xa.2", "xa", a, want[a]);
}
{
  // v2 writes, v1-era data and v2 data read back together.
  const pov = await c.newPov({ safe: true, shared: true });
  await c.call(pov, "xa", "put", { a: "a0", b: "b99", p: "p99" });
  await settle();
  want.a0 = norm([...JSON.parse(want.a0), { oid: "b99", p: "p99" }]);
  await expectRead("after a write through xa.2", "xa", "a0", want.a0);
  await expectRead("after a write through xa.2", "xb", "a0", wantXb);
}

// Step 4: a version whose record type genuinely changed (an extra field). The
// stored records are not of its type, so it must read none of them, cleanly.
await c.install("xa", 3);
{
  let got;
  try {
    got = await read("xa", "a0");
  } catch (e) {
    fail(`changed record type: xa.rng threw ${e}`);
  }
  if (!Array.isArray(got) || got.length !== 0) {
    fail(`changed record type: xa.3 read ${JSON.stringify(got)}, want [] (none of the stored records are its type)`);
  }
  console.log("ok changed record type: xa.3 reads none of the old records");
}
await expectRead("after installing xa.3", "xb", "a0", wantXb);

console.log("PACKAGE UPGRADE OK");
c.close();
