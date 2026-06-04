# Multi-agent knowledge graph — concurrent extraction demo

The shape multi-agent LLM systems keep reinventing badly: many extractor agents reading disjoint chunks of a corpus, each emitting facts about overlapping entities into one shared knowledge graph. N concurrent WebSocket "agents" all stream tags, mentions, and co-occurrences into the same Orly POV with **zero coordination** — no locks, no per-agent partitioning, no merge-on-read logic in the driver. The demo self-checks that every contribution lands.

## The trick

Three commutative field calls — no read-modify-write anywhere:

```orly
*<['entity', e]>::({str})    |= {tag}    # set union per entity
*<['mention', e, d]>::(int)  += 1        # per (entity, doc) counter
*<['cooccur', a, b]>::(int)  += 1        # per unordered-pair counter
```

`|=` and `+=` are field **calls**, not field changes. Two agents concurrently doing `add_tag("Python", "popular")` each emit a deferred `{Union, {"popular"}}` mutation at the storage layer; the read path folds them via `Rt::Mutate` back into the correct set. No lock, no race, no lost updates — the same mechanism powers [`wikipedia-pageviews/`](../wikipedia-pageviews/) (counters) and [`wikipedia-categories/`](../wikipedia-categories/) (set unions).

What an LLM-extraction pipeline would normally need:

| Approach | Problem |
|---|---|
| Per-agent local graph + post-hoc merge | Doubles storage; merge logic has to commute by hand. |
| Read-modify-write per fact (Postgres `UPDATE ... SET tags = tags <> ...`) | Row lock per write; N agents serialize on hot entities. |
| Single-writer service in front of the store | Throughput ceiling = one writer; agents wait on the queue. |
| Orly: `*<['entity', e]>::({str}) \|= {tag}` | Field call; lock-free; N agents land their unions, the engine aggregates. |

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

Exit code is non-zero if the verifier finds any tag set, mention counter, or cooccurrence counter that disagrees with the independently-derived ground truth.

## What the demo proves

- **No lost extractions under contention.** Hot entities (Python, OpenAI, Anthropic, Claude, GPT-4, Microsoft, Google) appear in many docs; agents are assigned docs round-robin so multiple agents hit the same entity concurrently. Every `add_tag` / `add_mention` / `add_cooccur` call lands.
- **Tag sets converge.** Different docs assign different tags to the same entity (Python picks up `language` from some docs, `popular` from others). The on-disk tag set after ingest equals the union derived from the corpus.
- **No driver-side merge logic.** The driver never reads-then-writes. It only ever calls the three field-call functions in [`graph.orly`](graph.orly). Aggregation is the engine's job.

## Related demos

- [`wikipedia-pageviews/`](../wikipedia-pageviews/) — the same `+= n` field call against integer counters under concurrent writers. The original issue-#49 motivating workload.
- [`wikipedia-categories/`](../wikipedia-categories/) — the same `|= {x}` field call against set unions, but single-writer. This demo combines both shapes and runs them concurrently.

This one demonstrates that the commutative-write story generalises directly to the shape AI builders care about: **many independent agents writing into one shared graph, without any coordination protocol**.
