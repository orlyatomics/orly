/**
 * Smoke test for the Orly MCP server (#526): spawns the built server over
 * stdio via the MCP SDK client and exercises every tool against a live
 * orlyi (started by run-smoke.sh, which sets ORLY_URL).
 *
 *   tools/list            -> the four orly_* tools are advertised
 *   orly_install_package  -> sample.1 installs
 *   orly_new_pov          -> returns a pov id
 *   orly_call             -> write_val / read_val round-trip
 *   orly_call_batch       -> 3 writes in one transaction, read back
 *   error surface         -> a bad method fails with isError, not a crash
 */

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

function assert(cond, msg) {
  if (!cond) {
    console.error(`SMOKE FAIL: ${msg}`);
    process.exit(1);
  }
  console.log(`ok: ${msg}`);
}

const text = (res) => res.content?.[0]?.text ?? "";
const parsed = (res) => JSON.parse(text(res));

const transport = new StdioClientTransport({
  command: process.execPath,
  args: [new URL("../dist/index.js", import.meta.url).pathname],
  env: { ...process.env },
});
const client = new Client({ name: "orly-mcp-smoke", version: "0.0.0" });
await client.connect(transport);

const tools = (await client.listTools()).tools.map((t) => t.name).sort();
assert(
  JSON.stringify(tools) ===
    JSON.stringify(["orly_call", "orly_call_batch", "orly_install_package", "orly_new_pov"]),
  `four tools advertised (got: ${tools.join(", ")})`,
);

const install = await client.callTool({
  name: "orly_install_package",
  arguments: { package: "sample", version: 1 },
});
assert(!install.isError, `install sample.1 (${text(install)})`);

const povRes = await client.callTool({ name: "orly_new_pov", arguments: {} });
assert(!povRes.isError, `new pov (${text(povRes)})`);
const pov = parsed(povRes).pov;
assert(typeof pov === "string" && pov.length > 0, `pov id returned (${pov})`);

const write = await client.callTool({
  name: "orly_call",
  arguments: { pov, package: "sample", method: "write_val", args: { n: 1, x: 101 } },
});
assert(!write.isError, `orly_call write_val (${text(write)})`);

const read = await client.callTool({
  name: "orly_call",
  arguments: { pov, package: "sample", method: "read_val", args: { n: 1 } },
});
assert(!read.isError && parsed(read) === 101, `orly_call read_val -> 101 (got ${text(read)})`);

const batch = await client.callTool({
  name: "orly_call_batch",
  arguments: {
    pov,
    package: "sample",
    method: "write_val",
    args_list: [
      { n: 2, x: 20 },
      { n: 3, x: 30 },
      { n: 4, x: 40 },
    ],
  },
});
assert(!batch.isError, `orly_call_batch of 3 writes (${text(batch)})`);
let sum = 0;
for (const n of [2, 3, 4]) {
  const r = await client.callTool({
    name: "orly_call",
    arguments: { pov, package: "sample", method: "read_val", args: { n } },
  });
  sum += parsed(r);
}
assert(sum === 90, `batch writes all landed (sum ${sum})`);

const bad = await client.callTool({
  name: "orly_call",
  arguments: { pov, package: "sample", method: "no_such_method", args: {} },
});
assert(bad.isError === true, `bad method surfaces as tool error (${text(bad).slice(0, 80)})`);

await client.close();
console.log("SMOKE PASS");
