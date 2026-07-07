#!/usr/bin/env node
/**
 * MCP server for the Orly database (#526).
 *
 * Exposes a running `orlyi` to any MCP-speaking agent runtime as conflict-free
 * shared memory: N agents (each running or sharing an instance of this server)
 * exchange a POV id and write concurrently — commutative writes (`+=`, set
 * union) merge in the engine with no locks and no lost updates, and reads fold
 * the value's history.
 *
 * A thin wrapper over the TypeScript driver (`clients/ts`): one WebSocket +
 * session per server process, lazily connected to `ORLY_URL` (default
 * `ws://127.0.0.1:8082/`), reconnecting once on a dropped connection. All
 * statement building/escaping lives in the driver.
 *
 * Argument encoding (JSON -> orlyscript, see `lit()` in the driver): numbers
 * become ints (`5` -> `5`), strings/booleans/arrays/objects become
 * str/bool/list/record. JSON cannot express three orlyscript shapes, so
 * exactly-one-key escape objects are decoded anywhere in `args`:
 *   {"$set":  [1, 2]}   -> the set literal `{1, 2}`
 *   {"$real": 5}        -> the real literal `5.0`
 *   {"$raw":  "now()"}  -> the expression text, verbatim (escape hatch)
 */

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import { connect, Client, OrlyError, set, raw, DEFAULT_URL, Args } from "orly";

const URL = process.env.ORLY_URL ?? DEFAULT_URL;

// -- connection lifecycle: one lazy client + session per process ----------

let cached: Client | null = null;
let connecting: Promise<Client> | null = null;

async function getClient(): Promise<Client> {
  if (cached) return cached;
  connecting ??= (async () => {
    const c = await connect(URL);
    await c.newSession();
    cached = c;
    return c;
  })().finally(() => {
    connecting = null;
  });
  return connecting;
}

/** Run `fn` against the client; on a connection-shaped failure (anything but
    an `OrlyError`, which means the server answered), reconnect once and
    retry. A reconnect makes a fresh session, so povs made by `orly_new_pov`
    survive (they live in the engine), but session-scoped state does not. */
async function withClient<T>(fn: (c: Client) => Promise<T>): Promise<T> {
  try {
    return await fn(await getClient());
  } catch (e) {
    if (e instanceof OrlyError) throw e;
    cached = null;
    return await fn(await getClient());
  }
}

// -- argument decoding: JSON escape objects -> driver literal types -------

function decodeArg(v: unknown): unknown {
  if (Array.isArray(v)) return v.map(decodeArg);
  if (v !== null && typeof v === "object") {
    const o = v as Record<string, unknown>;
    const keys = Object.keys(o);
    if (keys.length === 1) {
      if (keys[0] === "$set" && Array.isArray(o.$set)) return set(o.$set.map(decodeArg));
      if (keys[0] === "$raw" && typeof o.$raw === "string") return raw(o.$raw);
      if (keys[0] === "$real" && typeof o.$real === "number") {
        return raw(Number.isInteger(o.$real) ? o.$real.toFixed(1) : String(o.$real));
      }
    }
    return Object.fromEntries(keys.map((k) => [k, decodeArg(o[k])]));
  }
  return v;
}

const decodeArgs = (args: Record<string, unknown> | undefined): Args =>
  decodeArg(args ?? {}) as Args;

// -- tool plumbing ---------------------------------------------------------

type ToolResult = { content: Array<{ type: "text"; text: string }>; isError?: boolean };

const ok = (result: unknown): ToolResult => ({
  content: [{ type: "text", text: result === undefined ? "null" : JSON.stringify(result) }],
});

const fail = (e: unknown): ToolResult => ({
  isError: true,
  content: [
    {
      type: "text",
      text:
        e instanceof OrlyError
          ? `orly rejected [${e.statement}]: ${JSON.stringify(e.reply)}`
          : e instanceof Error
            ? `orly-mcp: ${e.message}`
            : `orly-mcp: ${String(e)}`,
    },
  ],
});

const guard =
  <A>(fn: (args: A) => Promise<ToolResult>) =>
  async (args: A): Promise<ToolResult> => {
    try {
      return await fn(args);
    } catch (e) {
      return fail(e);
    }
  };

// -- the server ------------------------------------------------------------

const server = new McpServer({ name: "orly", version: "0.1.0" });

server.registerTool(
  "orly_new_pov",
  {
    title: "Create an Orly POV",
    description:
      "Create a point of view (POV) on the Orly database and return its id. A POV is the " +
      "handle agents write through; to share state, create ONE pov and pass its id to every " +
      "collaborating agent — concurrent commutative writes to it merge without locks or lost " +
      "updates. `shared` povs promote their updates toward the global database; `private` povs " +
      "stay session-local. Leave `safe` true unless you know you want the fast/unsafe variant.",
    inputSchema: {
      shared: z.boolean().optional().describe("shared (default true) vs private"),
      safe: z.boolean().optional().describe("safe (default true) vs fast"),
      parent: z.string().optional().describe("optional parent pov id"),
    },
  },
  guard(async ({ shared, safe, parent }) => {
    const pov = await withClient((c) => c.newPov({ shared, safe, parent }));
    return ok({ pov });
  }),
);

server.registerTool(
  "orly_call",
  {
    title: "Call an Orly package method",
    description:
      "Invoke `method` from installed package `package` against POV `pov` and return its result " +
      "(one transaction). This is both read and write: methods are orlyscript functions; writes " +
      "are their effects. Args map by name to the method's `given` parameters. JSON numbers are " +
      "ints; use {\"$real\": 5} for reals, {\"$set\": [..]} for sets, {\"$raw\": \"expr\"} for a " +
      "verbatim orlyscript expression. Results: ints as numbers, sets as unordered arrays, " +
      "variants as {Tag: payload}, absent optionals as null.",
    inputSchema: {
      pov: z.string().describe("pov id from orly_new_pov (or shared by another agent)"),
      package: z.string().describe("installed package name"),
      method: z.string().describe("method (function) name in the package"),
      args: z.record(z.unknown()).optional().describe("named arguments for the method"),
    },
  },
  guard(async ({ pov, package: pkg, method, args }) => {
    const result = await withClient((c) => c.call(pov, pkg, method, decodeArgs(args)));
    return ok(result);
  }),
);

server.registerTool(
  "orly_call_batch",
  {
    title: "Call an Orly package method N times in one transaction",
    description:
      "Invoke `method` once per record in `args_list`, folded into a SINGLE atomic transaction " +
      "(all-or-nothing; every call sees the same pre-batch snapshot — no read-your-writes inside " +
      "the batch). Returns the N per-call results in order. Use for commutative fan-in and bulk " +
      "load. Same argument encoding as orly_call.",
    inputSchema: {
      pov: z.string().describe("pov id"),
      package: z.string().describe("installed package name"),
      method: z.string().describe("method (function) name in the package"),
      args_list: z
        .array(z.record(z.unknown()))
        .min(1)
        .describe("one named-argument record per call"),
    },
  },
  guard(async ({ pov, package: pkg, method, args_list }) => {
    const result = await withClient((c) =>
      c.callBatch(pov, pkg, method, args_list.map(decodeArgs)),
    );
    return ok(result);
  }),
);

server.registerTool(
  "orly_install_package",
  {
    title: "Install an Orly package",
    description:
      "Install compiled package `package` at `version` into the running server (the package's " +
      ".so must already be in orlyi's package directory — packages are compiled from .orly " +
      "sources with orlyc ahead of time). Typically a setup step before agents start writing.",
    inputSchema: {
      package: z.string().describe("package name (without version suffix)"),
      version: z.number().int().nonnegative().describe("package version number"),
    },
  },
  guard(async ({ package: pkg, version }) => {
    await withClient((c) => c.install(pkg, version));
    return ok({ installed: `${pkg}.${version}` });
  }),
);

// -- main -------------------------------------------------------------------

const transport = new StdioServerTransport();
await server.connect(transport);
