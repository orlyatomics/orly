# orly-mcp — MCP server for Orly

Exposes a running [`orlyi`](../../README.md) to any [MCP](https://modelcontextprotocol.io)-speaking
agent runtime (Claude Code, Claude Desktop, or anything on the MCP SDKs) as
**conflict-free shared memory**: N agents exchange a POV id and write to it
concurrently — commutative writes (`+=`, set union `|=`) merge in the engine
with no locks and no lost updates, and reads fold the value's history. The
[`agent-swarm`](../../examples/agent-swarm/) and [`grc20-pov`](../../examples/grc20-pov/)
examples show the workload this is built for.

A thin stdio wrapper over the [TypeScript driver](../ts/) — no engine or
protocol changes. Requires Node.js 18+ and a reachable `orlyi` (see the
repo [Quick start](../../README.md#quick-start)).

## Setup

```sh
cd clients/mcp
npm install
npm run build
```

Register with your MCP client, pointing `ORLY_URL` at your server (default
`ws://127.0.0.1:8082/`). For Claude Code:

```sh
claude mcp add orly -e ORLY_URL=ws://127.0.0.1:8082/ -- node /path/to/orly/clients/mcp/dist/index.js
```

or in a JSON MCP config:

```json
{
  "mcpServers": {
    "orly": {
      "command": "node",
      "args": ["/path/to/orly/clients/mcp/dist/index.js"],
      "env": { "ORLY_URL": "ws://127.0.0.1:8082/" }
    }
  }
}
```

## Tools

| Tool | What it does |
|---|---|
| `orly_new_pov` | Create a POV (safe/fast × shared/private, optional parent); returns the pov id agents share. |
| `orly_call` | `try {pov} package method <{args}>` — call a package method (read or write), one transaction. |
| `orly_call_batch` | N calls of one method folded into a single atomic transaction ([#253](https://github.com/orlyatomics/orly/issues/253)). |
| `orly_install_package` | Install a compiled package by name/version (its `.so` must be in orlyi's package dir). |

One Orly session per server process, lazily connected, reconnecting once on a
dropped connection (povs live in the engine and survive the reconnect).

## Argument encoding

Tool args are JSON; the driver encodes them as orlyscript literals — numbers
as ints, strings/booleans/arrays/objects as str/bool/list/record. Three
orlyscript shapes JSON can't express use escape objects (valid anywhere in
`args`, including nested):

| Escape | Orlyscript |
|---|---|
| `{"$set": [1, 2]}` | the set literal `{1, 2}` |
| `{"$real": 5}` | the real literal `5.0` |
| `{"$raw": "now()"}` | the expression text, verbatim |

Results come back as the engine's JSON: ints as numbers, sets as unordered
arrays, variants as `{Tag: payload}`, absent optionals as `null`.

## The multi-agent pattern

1. One agent (or a setup script) installs the schema package and calls
   `orly_new_pov`, then shares the pov id (it's just a string).
2. Every agent calls `orly_call` / `orly_call_batch` against that pov id —
   no coordination, no locking, no merge conflicts for commutative ops.
3. Readers see the merged state; history-shaped schemas (see the
   [examples](../../examples/)) additionally give replay and time-travel.

## Smoke test

With the repo built (`make debug`):

```sh
npm run smoke
```

starts a throwaway mem-sim `orlyi`, spawns the server over stdio via the MCP
SDK client, and exercises every tool end-to-end.
