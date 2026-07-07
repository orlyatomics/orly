# Two MCP agents, one knowledge graph

Two independent agent processes — each speaking **only
[MCP](https://modelcontextprotocol.io)**, through its own
[`orly-mcp`](../../clients/mcp/) server instance — build one knowledge graph
in one shared Orly POV, **concurrently, with zero coordination**. Neither
agent knows the other exists; they share exactly one string (the pov id).
The engine is the merge.

This is [`agent-swarm`](../agent-swarm/) lifted to the layer real agent
runtimes use: same schema ([`graph.orly`](../agent-swarm/graph.orly), reused
verbatim — provenance tag sets via `|=`, mention/co-occurrence counters via
`+=`, index-free adjacency + BFS `reach`), but the writers are MCP tool calls
(`orly_call`, `orly_call_batch`), not driver code.

## Run it

With the repo built (`make debug`) and Node.js 18+:

```sh
cd examples/mcp-agent-duet
./run.sh
```

`run.sh` builds the TS driver + MCP server, compiles the shared schema,
starts a throwaway mem-sim `orlyi`, and runs [`duet.mjs`](duet.mjs): the
orchestrator installs `graph.1`, mints **one shared pov**, spawns agents
**ada** (docs 0–5) and **byte** (docs 6–11) as concurrent child processes,
and then verifies the merged graph through MCP reads. Exit code is non-zero
if anything disagrees with the corpus-derived ground truth.

## What it proves

- **Concurrent MCP writers lose nothing.** Both agents hammer the same hub
  entities (JWST, Hubble, Cassini, Vega) through separate sessions; every
  mention/co-occurrence counter and every tag lands.
- **Provenance survives corroboration.** Both agents independently assert
  `JWST:telescope`, `JWST:infrared`, and `Cassini:probe` — the tag set holds
  one `(tag, agent)` record *per agent*, not a collapsed string.
- **The graph bridges the agents.** `reach("Kepler-442b", k=2)` — an entity
  only ada wrote — arrives at TRAPPIST-1e and Proxima Centauri b, which only
  byte wrote, through hubs both touched. One traversal, spanning both agents'
  contributions, computed in the engine.

## Run it with real agents

The scripted agents are the deterministic CI stand-in. The same topology
works with live agent sessions — e.g. two Claude Code sessions:

1. Start `orlyi` and install `graph.1` (steps 1–3 of `run.sh`, or keep it
   running), and register the MCP server in each session:

   ```sh
   claude mcp add orly -e ORLY_URL=ws://127.0.0.1:8082/ -- node /path/to/orly/clients/mcp/dist/index.js
   ```

2. In session one: *"Create a shared pov with `orly_new_pov` and print its
   id. Then read these six documents and, for every entity, tag, and
   co-occurrence you find, record it with `orly_call` (`graph` package:
   `add_tag(e, t, agent: "ada")`, `add_mention(e, d)`, `add_cooccur(a, b)`)."*

3. In session two, paste the pov id: *"Using pov `<id>`, do the same for
   these six documents as agent `byte`."* Run both at once.

4. In either session (or a third): *"Query `tags_for_entity`, `neighbors`,
   and `reach` and summarize the graph — including which facts each agent
   contributed."*

No locks, no queue, no merge prompt — the corroborated tags and the
cross-agent traversal come back exactly as in the scripted run.
