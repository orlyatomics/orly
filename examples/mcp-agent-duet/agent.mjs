/**
 * One scripted extractor agent (#528). Everything it does goes through MCP:
 * it spawns its OWN `orly-mcp` stdio server (the same topology a real agent
 * session has -- one server per session) and never touches the Orly driver
 * or protocol directly.
 *
 *   node agent.mjs <name> <pov-id> <fromDoc> <toDoc>
 *
 * For each doc in its slice it emits, against the SHARED pov:
 *   - one orly_call_batch of add_mention (all the doc's entities, one atomic
 *     transaction),
 *   - orly_call add_tag per (entity, tag) -- provenance carries this agent's
 *     name,
 *   - orly_call add_cooccur per unordered entity pair.
 *
 * The other agent is doing the same thing to the same pov at the same time;
 * neither knows the other exists. That absence of coordination is the demo.
 */

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { DOCS, docPairs } from "./corpus.mjs";

const [name, pov, fromArg, toArg] = process.argv.slice(2);
const from = Number(fromArg);
const to = Number(toArg);
if (!name || !pov || !Number.isInteger(from) || !Number.isInteger(to)) {
  console.error("usage: node agent.mjs <name> <pov-id> <fromDoc> <toDoc>");
  process.exit(2);
}

const SERVER = new URL("../../clients/mcp/dist/index.js", import.meta.url).pathname;

const transport = new StdioClientTransport({
  command: process.execPath,
  args: [SERVER],
  env: { ...process.env },
});
const mcp = new Client({ name: `duet-${name}`, version: "0.0.0" });
await mcp.connect(transport);

async function call(tool, args) {
  const res = await mcp.callTool({ name: tool, arguments: args });
  if (res.isError) {
    throw new Error(`${name}: ${tool} failed: ${res.content?.[0]?.text}`);
  }
  return res;
}

let calls = 0;
for (const doc of DOCS.slice(from, to + 1)) {
  await call("orly_call_batch", {
    pov,
    package: "graph",
    method: "add_mention",
    args_list: doc.entities.map((e) => ({ e, d: doc.id })),
  });
  calls++;
  for (const [e, t] of doc.tags) {
    await call("orly_call", {
      pov,
      package: "graph",
      method: "add_tag",
      args: { e, t, agent: name },
    });
    calls++;
  }
  for (const [a, b] of docPairs(doc)) {
    await call("orly_call", {
      pov,
      package: "graph",
      method: "add_cooccur",
      args: { a, b },
    });
    calls++;
  }
  console.log(`[${name}] doc ${doc.id}: ${doc.entities.length} entities, ${doc.tags.length} tags`);
}

await mcp.close();
console.log(`[${name}] done: ${calls} MCP tool calls, docs ${from}..${to}`);
