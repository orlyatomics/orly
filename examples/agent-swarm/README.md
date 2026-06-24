# Multi-agent knowledge graph — concurrent extraction demo

The shape multi-agent LLM systems keep reinventing badly: many extractor agents reading disjoint chunks of a corpus, each emitting facts about overlapping entities into one shared knowledge graph. N concurrent WebSocket "agents" all stream tags, mentions, and co-occurrences into the same Orly POV with **zero coordination** — no locks, no per-agent partitioning, no merge-on-read logic in the driver. Every tag carries **provenance** — the agent that asserted it — so a tag three agents independently produce is three records you can read back, not a collapsed string. The demo self-checks that every contribution lands.

## The trick

Three commutative field calls — no read-modify-write anywhere:

```orly
*<['entity', e]>::({<{.tag: str, .agent: str}>})  |= {<{.tag, .agent}>}  # provenance set per entity
*<['mention', e, d]>::(int)                       += 1   # per (entity, doc) counter
*<['cooccur', a, b]>::(int)                       += 1   # per unordered-pair counter
```

`|=` and `+=` are field **calls**, not field changes. Two agents concurrently doing `add_tag("Python", "popular", ...)` each emit a deferred `{Union, ...}` mutation at the storage layer; the read path folds them via `Rt::Mutate` back into the correct set. No lock, no race, no lost updates — the same mechanism powers [`wikipedia-pageviews/`](../wikipedia-pageviews/) (counters) and [`wikipedia-categories/`](../wikipedia-categories/) (set unions).

The entity tag set is a set **of records** — `{<{.tag: str, .agent: str}>}`, storable record-sets being [issue #90](https://github.com/orlyatomics/orly/issues/90). Distinct tags and their contributing agents both fall out of the one set on read; the rollup below annotates any tag more than one agent corroborated (`language×5`).

What an LLM-extraction pipeline would normally need:

| Approach | Problem |
|---|---|
| Per-agent local graph + post-hoc merge | Doubles storage; merge logic has to commute by hand. |
| Read-modify-write per fact (Postgres `UPDATE ... SET tags = tags <> ...`) | Row lock per write; N agents serialize on hot entities. |
| Single-writer service in front of the store | Throughput ceiling = one writer; agents wait on the queue. |
| Orly: `*<['entity', e]>::({<{.tag,.agent}>}) \|= {<{.tag,.agent}>}` | Field call; lock-free; N agents land their provenance records, the engine aggregates. |

## Run it

Python:

```sh
cd examples/agent-swarm
./run.sh
```

Go:

```sh
cd examples/agent-swarm
./run-go.sh
```

Both wrappers require `make debug` to have produced `orlyi` and `orlyc`. Each kills any prior demo instance, starts a fresh `orlyi`, and runs the driver.

`DEMO_SCALE=small` runs 4 agents over the first 20 docs (CI default). The default (`DEMO_SCALE=large` or unset) runs 8 agents over all 40 docs.

Exit code is non-zero if the verifier finds any tag provenance set, mention counter, or cooccurrence counter that disagrees with the independently-derived ground truth.

## What the demo proves

- **No lost extractions under contention.** Hot entities (Python, OpenAI, Anthropic, Claude, GPT-4, Microsoft, Google) appear in many docs; agents are assigned docs round-robin so multiple agents hit the same entity concurrently. Every `add_tag` / `add_mention` / `add_cooccur` call lands.
- **Tag provenance sets converge.** Different docs assign different tags to the same entity (Python picks up `language` from some docs, `popular` from others), and different agents independently corroborate the same tag. The on-disk set of `(tag, agent)` records after ingest equals the union derived from the corpus — every contributing agent is preserved.
- **No driver-side merge logic.** The driver never reads-then-writes. It only ever calls the field-call functions in [`graph.orly`](graph.orly). Aggregation is the engine's job.

## Traversing the graph

A knowledge graph you can only point-query is half a graph database. The co-occurrences the agents stream form an **undirected graph** — entities are nodes, a shared document is an edge. `add_cooccur` writes that edge **both ways** as a symmetric adjacency, so a node's neighbours are one prefix scan:

```orly
*<['adj', a, b]>::(int) += 1   # both directions, still a coordination-free field call
*<['adj', b, a]>::(int) += 1
```

That trailing-`free` shape is exactly what Orly's sorted index seeks straight to — **index-free adjacency** ([issue #219](https://github.com/orlyatomics/orly/issues/219)), the thing graph engines are built on. On top of it, transitive traversal needs **no new storage or engine capability** — just a `reduce` over a depth range, accumulating a visited set (so cycles terminate). `union_map` (map each element of a sequence to a set, union the results) is the terse surface for the per-hop frontier expansion; it desugars to `reduce start (empty {T}) | …`:

```orly
neighbors = (keys (int) @ <['adj', e, free::(str)]>) union_map {that.2}   # one hop
grow      = v | ((**v) union_map neighbors(.e: that))                     # expand a frontier
reach     = [1..k] reduce grow(.v: start ({seed}))                        # k-hop closure
```

The driver verifies `reach(seed, k)` against a plain-Python BFS over the same graph, then showcases a neighbourhood — e.g. from **Claude**, ~92% of the graph is reachable within 3 hops. The point: the graph was assembled by N agents with **zero coordination**, and it's still **traversable transitively** — concurrent construction *and* graph queries in one store, which Neo4j (single-primary writes) and Cayley (a query layer over a store) don't combine.

## Related demos

- [`wikipedia-pageviews/`](../wikipedia-pageviews/) — the same `+= n` field call against integer counters under concurrent writers. The original issue-#49 motivating workload.
- [`wikipedia-categories/`](../wikipedia-categories/) — the same `|= {x}` field call against set unions, but single-writer. This demo combines both shapes and runs them concurrently.

This one demonstrates that the commutative-write story generalises directly to the shape AI builders care about: **many independent agents writing into one shared graph, without any coordination protocol**.
