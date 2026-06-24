// Recursive sum-type storage + client marshaling demo (issue #115), in TypeScript.
//
// Equivalent to demo.py / demo.go -- same scenario and self-check. Stores a
// fixed recursive value of each shape, reads it back, and checks the marshaled
// JSON. A variant marshals as { Tag: <payload> }; a tag-only arm as { Tag: {} };
// numbers come back as JS numbers.
//
// Uses the shared `orly` client (clients/ts). Run via the wrapper: ./run-ts.sh

import { connect } from "orly";

interface RecCase {
  label: string;
  put: string;
  get: string;
  expected: unknown;
}

const cases: RecCase[] = [
  {
    label: "tree  (self-reference in a record arm, #103)",
    put: "put_tree",
    get: "get_tree",
    expected: { Branch: { l: { Leaf: 1 }, r: { Branch: { l: { Leaf: 2 }, r: { Leaf: 3 } } } } },
  },
  {
    label: "doc   (self-reference under a list, #120)",
    put: "put_doc",
    get: "get_doc",
    expected: { Arr: [{ Num: 1 }, { Str: "two" }, { Arr: [{ Num: 3 }] }, { Null: {} }] },
  },
  {
    label: "mtree (mutual recursion, #116)",
    put: "put_mtree",
    get: "get_mtree",
    expected: {
      MNode: { FCons: { head: { MLeaf: 1 }, tail: { FCons: { head: { MLeaf: 2 }, tail: { FEmpty: {} } } } } },
    },
  },
  {
    label: "ntree (nested-variant arm, #125)",
    put: "put_ntree",
    get: "get_ntree",
    expected: { NBranch: { Un: { NLeaf: 7 } } },
  },
];

// Structural deep-equality, independent of object key order.
function deepEqual(a: unknown, b: unknown): boolean {
  if (a === b) return true;
  if (typeof a !== "object" || typeof b !== "object" || a === null || b === null) return false;
  if (Array.isArray(a) || Array.isArray(b)) {
    if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) return false;
    return a.every((x, i) => deepEqual(x, b[i]));
  }
  const ao = a as Record<string, unknown>;
  const bo = b as Record<string, unknown>;
  const ak = Object.keys(ao);
  const bk = Object.keys(bo);
  if (ak.length !== bk.length) return false;
  return ak.every((k) => k in bo && deepEqual(ao[k], bo[k]));
}

async function main(): Promise<void> {
  const c = await connect();
  await c.newSession();
  await c.install("recursive", 0);
  const pov = await c.newPov();
  console.log(`pov: ${pov}\n`);

  let failures = 0;
  for (let k = 0; k < cases.length; k++) {
    const rc = cases[k];
    await c.call(pov, "recursive", rc.put, { k });
    const got = await c.call(pov, "recursive", rc.get, { k });
    const ok = deepEqual(got, rc.expected);
    console.log(`  [${ok ? "ok" : "FAIL"}] ${rc.label}`);
    if (!ok) {
      console.log(`        expected: ${JSON.stringify(rc.expected)}`);
      console.log(`        got:      ${JSON.stringify(got)}`);
      failures++;
    }
  }

  await c.exit();

  if (failures > 0) {
    console.error(`\n${failures} case(s) failed.`);
    process.exit(1);
  }
  console.log("\nAll recursive-variant values round-tripped through storage and marshaled correctly.");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
