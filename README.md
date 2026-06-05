<p align="center">
  <img src="docs/branding/orly.png?raw=true" height="120" alt="Orly" />
</p>

<p align="center">
  <a href="https://github.com/orlyatomics/orly/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/orlyatomics/orly/actions/workflows/ci.yml/badge.svg?branch=master" /></a>
  &nbsp;
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-23-00599C.svg" />
  &nbsp;
  <img alt="License" src="https://img.shields.io/badge/license-Apache--2.0-blue.svg" />
</p>

<p align="center">
  <b>Orly</b> — a non-relational database built around <i>Points of View</i>,<br />
  causally-ordered merge, and a compiled query language (<i>Orlyscript</i>).
</p>

---

> **Status — 2026 revival.** Dormant from 2019 until early 2026; a modernization pass brought the codebase back to building and testing cleanly on a current Linux toolchain. `make debug`, `make test`, `make release`, and the Orlyscript `lang_test.py` harness all pass on Ubuntu 24.04 + gcc 13. See [#10](https://github.com/orlyatomics/orly/issues/10) for the full status and remaining gaps, and [`CHANGELOG.md`](CHANGELOG.md) for what's landed since. No active feature development; experiments and clean-up are welcome.

---

## Features

- **Points of View.** Optimistic concurrency without locking. Each client makes changes in its own private POV — a small sandbox — which eventually propagates into shared POVs and then into the global POV (the whole database). Field calls (`x += 1`) are preferred over field changes (`x = x + 1`) because they merge commutatively.

- **Causal ordering (the _Flux Capacitor_).** Merge-conflict resolution defines its "time line" by causality rather than clock time. Internal mechanism; the original 2014 pitch implied user-facing "time travel" queries which the engine never grew, but the capability is achievable in user-space — see the [bitcoin time-travel example](examples/bitcoin-time-travel/).

- **Orlyscript.** A high-level, compiled, type-safe, functional query and programming language. Sources are compiled to `.so` packages that `orlyi` loads at runtime. Supports inline tests, native compilation, and most of the niceties of a real language rather than just a query DSL.

- **Single-node first.** Fail-over / replication / hundreds of thousands of transactions per second on one node. Sharding was designed for but never built; that part of the original pitch is currently aspirational.

## Quick start

System dependencies (Ubuntu 24.04):

```sh
sudo apt-get install -y \
  build-essential gcc g++ \
  uuid-dev libgmp-dev libaio-dev libsnappy-dev \
  libreadline-dev libboost-system-dev zlib1g-dev \
  bison flex valgrind
```

Build and test:

```sh
make debug       # 1707 jobs, ~2-5 min on a recent laptop
make test        # runs 188 test binaries
make release     # LTO-built production binaries
```

The four production binaries land in `../out_orly/debug/` (or `../out_orly/release/`):

| Binary | Purpose |
| --- | --- |
| `orly/orlyc` | Orlyscript compiler |
| `orly/server/orlyi` | Database server |
| `orly/spa/spa` | Single-process app server |
| `orly/client/orly_client` | Interactive client shell |

Exercise the Orlyscript test suite against compiled `.orly` programs:

```sh
python3 tools/lang_test.py -d orly/data tests/lang_tests
```

## Examples

### [`examples/bitcoin-time-travel/`](examples/bitcoin-time-travel/) — time travel + multiverse via key-encoded version

A Bitcoin-flavoured ledger that does **historical queries** ("balance at block 5") and **branched-history multiverse queries** ("balance on the fork vs the mainnet at block 7"). The trick: encode the version axis (block height) AND the branch into the key tuple, fold over the height axis on read.

### [`examples/wikipedia-categories/`](examples/wikipedia-categories/) — same trick, different monoid

A Wikipedia-flavoured demo that does the same fold-over-keyed-versions trick, but with **set union** as the operator instead of integer addition. Watch the "Programming languages" set grow from `{FORTRAN}` in 1957 through `{...35 entries...}` in 2024 — `members_at(cat, year)` is one four-line Orlyscript function.

Together the first two examples prove the polymorphic-monoid claim: pick the operator (`+`, `|`, `++`, `min`, …) and the identity, get historical queries for that domain.

### [`examples/wikipedia-pageviews/`](examples/wikipedia-pageviews/) — concurrent `+=` that actually composes

Wikimedia-style hourly pageview counters under contention. 8 concurrent WebSocket sessions all do `*<['views', lang, page, hour]>::(int) += n` against the same hot keys; the self-check confirms every increment lands (5,938 / 5,938 events across 96 keys, zero lost updates).

Most databases force a tradeoff on this workload:

| Database | Statement | Behavior under N concurrent writers |
|---|---|---|
| Postgres | `UPDATE views SET n = n + 1 WHERE page = ?` | Row lock; writers serialize on hot pages |
| Redis | `INCR views:<page>` | Atomic, but the server is single-threaded |
| Cassandra | `UPDATE views SET n = n + 1 WHERE …` | Counter columns with consistency caveats |
| **Orly** | `*<['views', …, page, hour]>::(int) += n` | Field call; lock-free; all N writers land their increments and the read folds them |

The mechanism: each `+=` emits an `{Add, n}` mutation at the storage layer instead of resolving to a value at write time, and the read path folds same-mutator runs back into the resolved value. CI runs this as the workload-level integration test for the property; pass `DEMO_SCALE=small` for the CI-friendly 4-writer / 800-event variant.

### [`examples/agent-swarm/`](examples/agent-swarm/) — multi-agent knowledge-graph extraction without coordination

The shape multi-agent LLM pipelines keep reinventing badly. N independent extractor "agents" each process a disjoint slice of a corpus and stream tags, mentions, and cooccurrences into one shared knowledge graph — **with zero coordination**. No locks, no per-agent partitioning, no driver-side merge logic. Every tag carries **provenance** — the agent that asserted it — so a tag three agents independently produce is three records you can read back, not a collapsed string. Combines both commutative-write shapes from the demos above and runs them concurrently:

- `*<['entity', e]>::({<{.tag: str, .agent: str}>}) |= {<{.tag, .agent}>}` — set-union of provenance **records** per entity (the `wikipedia-categories` operator, but concurrent and structured — storable record-sets are [#90](https://github.com/orlyatomics/orly/issues/90))
- `*<['mention', e, d]>::(int) += 1` — per-(entity, doc) counter
- `*<['cooccur', a, b]>::(int) += 1` — per-unordered-pair counter

8 agents extract from 40 docs concurrently, with hot entities (`Python`, `OpenAI`, `Claude`, `GPT-4`, …) deliberately round-robined across agents so they collide on the same keys; the self-check confirms every tag provenance record, mention counter, and cooccurrence counter matches the independently-derived ground truth (147 `(tag, agent)` records across 25 entities + 105 mention counters + 76 cooccurrence pairs, zero lost extractions). The per-entity rollup annotates each tag with how many distinct agents corroborated it (`language×5`).

What this would normally require:

| Approach | Problem under N concurrent agents |
|---|---|
| Per-agent local graph + post-hoc merge | Doubles storage; merge logic has to commute by hand |
| Read-modify-write per fact (`UPDATE … SET tags = tags ∪ {…}`) | Row lock per write; agents serialize on hot entities; provenance needs a separate join table |
| Single-writer service in front of the store | Throughput ceiling = one writer; agents queue |
| **Orly** | `\|=` and `+=` are field calls; lock-free; all N agents land their contributions and the engine aggregates |

Why it matters: this is exactly the topology AI extraction pipelines have — many agents reading disjoint chunks, emitting overlapping facts into one graph — and every conventional database forces you to invent a coordination protocol on top. Orly gives it to you for free, as a direct consequence of how field calls work. The driver never reads-then-writes; it only ever calls the three commutative functions in [`graph.orly`](examples/agent-swarm/graph.orly).

### [`examples/grc20-pov/`](examples/grc20-pov/) — GRC-20 knowledge graph: event-sourced + concurrent editors + time-travel

A reference impl of Geo / The Graph's [GRC-20](https://github.com/geobrowser/grc-20) knowledge-graph standard. GRC-20 models knowledge as append-only ops (`CreateEntity`, `UpdateEntity`, `CreateRelation`, `DeleteEntity`) — exactly the shape Orly stores natively. Every op becomes one `|=` of an event **record** (`<{.ts: int, .editor: str, .kind: str, .val: str}>`) into a per-`(entity, property)` history set — a set of records, storable since [#90](https://github.com/orlyatomics/orly/issues/90), so the four typed fields travel end to end with no string packing. Concurrent editors stream into the same shared POV with **no coordination**; reads replay the timestamp-sorted log to reconstruct entity state, or any historical state if you pass a cutoff.

The demo runs three phases over a corpus of six Greek philosophers:

| Phase | Editor | What |
|---|---|---|
| 1 | `wiki` | biographical facts (`name`, `born`, `died`) |
| 2 | `stanford` | school of thought + `student_of` relations |
| 3 | both | concurrent race on `pythagoras.born` — both events survive in history |

After each phase the demo prints a snapshot reconstructed from the event log; the final editorial diff shows per-editor event counts and overlapping entities. This example is the synthesis of the other four: time-travel from [`bitcoin-time-travel`](examples/bitcoin-time-travel/) and [`wikipedia-categories`](examples/wikipedia-categories/), concurrent multi-writer from [`wikipedia-pageviews`](examples/wikipedia-pageviews/) and [`agent-swarm`](examples/agent-swarm/), applied to a published graph standard from another project rather than a synthetic workload. The entire engine side is three commutative orlyscript functions; GRC-20's op vocabulary lives entirely in the driver.

All five examples ship two equivalent drivers — Python (`./run.sh`) and Go (`./run-go.sh`) — and are smoke-tested in CI on every push.

## Walkthrough

[`docs/walkthrough.md`](docs/walkthrough.md) — compile an Orlyscript package with `orlyc`, load it into a running `orlyi`, and invoke a method on it via `orly_client`. The full pipeline end to end.

## Supported platforms

Linux only, x86-64. Verified on Ubuntu 24.04. Earlier releases probably work; not re-tested in the revival pass.

## Toolchain

| Component | Version |
| --- | --- |
| gcc | 13.3.0 |
| C++ standard | `-std=c++23` |
| boost (system) | 1.83 — includes `boost::beast` for the WebSocket surface |
| Python (for `lang_test.py`) | 3.12 |
| Go (for the optional Go driver in `examples/`) | 1.22+ |

Build flags live in [`root.jhm`](root.jhm); per-target overrides in `debug.jhm` / `release.jhm` / `bootstrap.jhm`.

## Contributing

The build system (`jhm`) lives in [`jhm/`](jhm/); the bootstrap path is documented in [`bootstrap.sh`](bootstrap.sh). There's no formal style guide; match the surrounding code.

### IDE / clangd

Every successful `make debug` (or any `jhm` invocation that compiles C/C++) writes a [JSON Compilation Database](https://clang.llvm.org/docs/JSONCompilationDatabase.html) to `compile_commands.json` at the repo root. clangd, clang-tidy, IWYU, and most C++-aware editors pick it up automatically. The file is regenerated on every build, so it's `.gitignore`d.

---

<sub>README.md © 2010–2026 Atomic Kismet Company. Licensed under [Creative Commons Attribution-ShareAlike 4.0 International](http://creativecommons.org/licenses/by-sa/4.0/).</sub>
