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

<p align="center">
  <b>Write from many clients at once — no locks, no conflict-resolution code — and query any past state.</b>
</p>

Concurrent commutative writes (`x += 1`, set-union `|=`) merge without losing
updates, and every value is a fold over its own history, so a point-in-time read
is just a query at an earlier version. The clearest showcase is a small
**[parimutuel prediction market](examples/prediction-market/)** ("Polymarket
clone"): N traders bet on one market *concurrently* — zero coordination, not a
single bet lost — the implied prices are a read-time fold of the trade log, and
the price history is time-travel. Build on it from **[Python](clients/python)**,
**[Go](clients/go)**, or **[TypeScript](clients/ts)** (browser + Node), all
speaking the same [WebSocket + JSON protocol](docs/PROTOCOL.md).

---

> **Status — 2026.** Dormant from 2019 until early 2026. A modernization pass brought the codebase back to building and testing cleanly on a current toolchain — `make debug`, `make test`, `make release`, and the Orlyscript `lang_test.py` harness all pass on Ubuntu 24.04 + gcc 13 — and a substantial language arc followed: sum types / tagged unions, recursive and mutually-recursive variants (storable and client-transmissible), variant widening, and recursive-return type verification. See [`CHANGELOG.md`](CHANGELOG.md) for what's landed and [#10](https://github.com/orlyatomics/orly/issues/10) for the original revival status; the open issues track an engine-integrity and test-hardening backlog (latent revival defects plus sanitizer / coverage gaps). Contributions welcome.

---

## Features

> The concurrency model behind the first two features — POVs, the Tetris merge, the Flux Capacitor's causal ordering, and commutative field-call folding — is documented in depth in [`docs/architecture.md`](docs/architecture.md).

- **Points of View.** Optimistic concurrency without locking. Each client makes changes in its own private POV — a small sandbox — which eventually propagates into shared POVs and then into the global POV (the whole database). Field calls (`x += 1`) are preferred over field changes (`x = x + 1`) because they merge commutatively.

- **Causal ordering (the _Flux Capacitor_).** Merge-conflict resolution defines its "time line" by causality rather than clock time. Internal mechanism; the original 2014 pitch implied a built-in user-facing "time travel" query operator which the engine never grew — but the *capability* falls out in user-space by encoding a version axis in the key and folding on read, with no engine changes. See the [bitcoin](examples/bitcoin-time-travel/), [GRC-20](examples/grc20-pov/), and [prediction-market](examples/prediction-market/) examples.

- **Orlyscript.** A high-level, compiled, type-safe, functional query and programming language. Sources are compiled to `.so` packages that `orlyi` loads at runtime. Supports inline tests, native compilation, and most of the niceties of a real language rather than just a query DSL.

- **Sum types (tagged unions).** Typed heterogeneous values — `<| Text(str) | Number(int) | Deleted |>` — that construct, store (including *sets* of differently-tagged variants), and read back like any other value, with exhaustive compile-time-checked `when` matching and payload binders (`Number(n): n + 1`). This lets a heterogeneous event log be folded into its current state *in the engine* — latest-write-wins, tombstones, formatting — rather than in the driver; see the [GRC-20 example](examples/grc20-pov/). Variants may also be **recursive** ([#103](https://github.com/orlyatomics/orly/issues/103)): `tree is <| Leaf(int) | Branch(<{.l: tree, .r: tree}>) |>;` declares a binary tree, constructed through the type's name (`tree.Leaf(1)`, `tree.Branch(<{...}>)`) and folded with ordinary recursive functions plus `when`. Recursive values are fully first-class: they compute, compare, live in sets, **store and read back** (`<-` / `*`), and travel to clients over the wire — including mutually-recursive variant groups ([#115](https://github.com/orlyatomics/orly/issues/115), [#116](https://github.com/orlyatomics/orly/issues/116)) — and can be **widened** to a superset variant with `as` ([#104](https://github.com/orlyatomics/orly/issues/104)).

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

### ThreadSanitizer build

A `tsan` config builds with ThreadSanitizer (`-fsanitize=thread`) to check the
lock-free / commutative-merge machinery for data races. Build a target and run
it under TSan:

```sh
tools/jhm -c tsan orly/indy/context_fold.test          # output: ../out_orly/tsan/...
setarch "$(uname -m)" -R ../out_orly/tsan/orly/indy/context_fold.test
```

The `setarch -R` (disable ASLR) is required on modern kernels — without it
libtsan aborts at startup with *"unexpected memory mapping"*. CI runs a curated
set of concurrency tests this way in a **gating** job — the set is clean under
TSan, so the job fails on any un-suppressed race; provably-benign reports are
documented and suppressed in [`orly/tsan.supp`](orly/tsan.supp). See
[`.github/workflows/ci.yml`](.github/workflows/ci.yml) and issues
[#177](https://github.com/orlyatomics/orly/issues/177) /
[#184](https://github.com/orlyatomics/orly/issues/184).

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

A reference impl of Geo / The Graph's [GRC-20](https://github.com/geobrowser/grc-20) knowledge-graph standard. GRC-20 models knowledge as append-only ops (`CreateEntity`, `SetProperty`, `CreateRelation`, `DeleteEntity`) — exactly the shape Orly stores natively. The op is a **typed tagged union** (`<| Text(str) | Number(int) | Relation(str) | Deleted |>`), and every op becomes one `|=` of an event record (`<{.ts: int, .editor: str, .op: ...}>`) into a per-`(entity, property)` history set — a set of variant-bearing records, storable since [#90](https://github.com/orlyatomics/orly/issues/90) / [#96](https://github.com/orlyatomics/orly/issues/96), so the typed op travels end to end with no string packing. Concurrent editors stream into the same shared POV with **no coordination**; reads replay the timestamp-sorted log — latest-write-wins, tombstones, value formatting, time-travel — *in the engine*, via a `reduce` + an exhaustive `when` match, not in the driver.

The demo runs three phases over a corpus of six Greek philosophers:

| Phase | Editor | What |
|---|---|---|
| 1 | `wiki` | biographical facts (`name`, `born`, `died`) |
| 2 | `stanford` | school of thought + `student_of` relations |
| 3 | both | concurrent race on `pythagoras.born` — both events survive in history |

After each phase the demo prints a snapshot reconstructed from the event log; the final editorial diff shows per-editor event counts and overlapping entities. This example is the synthesis of the others: time-travel from [`bitcoin-time-travel`](examples/bitcoin-time-travel/) and [`wikipedia-categories`](examples/wikipedia-categories/), concurrent multi-writer from [`wikipedia-pageviews`](examples/wikipedia-pageviews/) and [`agent-swarm`](examples/agent-swarm/), applied to a published graph standard from another project rather than a synthetic workload — and it owns the GRC-20 op vocabulary and the entire replay *in orlyscript*, using the [sum-type](#features) `reduce` + `when` fold, with the driver reduced to streaming typed events and reading resolved values.

### [`examples/crdt-text/`](examples/crdt-text/) — collaborative text editing: the database *is* the CRDT merge

A Google-Docs-style collaborative editor where Orly's commutative storage **is** the conflict-free merge — no operational transform, no central reconcile, no lock. It's a [Logoot](https://hal.inria.fr/inria-00432368/document) sequence CRDT: every character gets a dense, totally-ordered position id, so the document is an unordered *set* of `(position, char)` entries and the visible text is the engine's read-time fold — sort by position, drop tombstoned, concatenate (the same `sorted_by` + `reduce` machinery as `grc20-pov`, over a *sequence* instead of an event log). Two editors stream inserts and deletes into one shared POV with **no coordination** and converge; concurrent inserts at the same spot both survive (distinct dense positions, ties broken on `(site, clock)`), so nothing is lost. The one piece of real CRDT logic — `between(p, q)`, the dense-order position allocation — is ~12 lines in the driver; the merge, convergence, tombstones, and time-travel (version history) are all the engine. The self-check asserts three independent readers render byte-identical text.

All six examples ship two equivalent drivers — Python (`./run.sh`) and Go (`./run-go.sh`) — and are smoke-tested in CI on every push.

## Walkthrough

[`docs/walkthrough.md`](docs/walkthrough.md) — compile an Orlyscript package with `orlyc`, load it into a running `orlyi`, and invoke a method on it via `orly_client`. The full pipeline end to end.

[`docs/PROTOCOL.md`](docs/PROTOCOL.md) — the WebSocket + JSON client protocol an application uses to talk to a running `orlyi` (the path the `examples/` drivers use): connection, statements, and JSON marshaling.

**Client libraries** implement that protocol so apps don't hand-roll it: [`clients/python`](clients/python) (`orly`), [`clients/go`](clients/go) (`orly`), and [`clients/ts`](clients/ts) (`orly` — typed, browser + Node). The `examples/` drivers run on them.

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
