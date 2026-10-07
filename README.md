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
  <b>Orly</b> — a database where <i>concurrent writers never conflict</i>.<br />
  Many writers, one shared graph, no locks and no merge code.
</p>

Every multi-agent system reinvents shared memory: a mutex, a queue, or a
single-writer service everyone funnels through. Orly removes the problem rather
than coordinating around it — commutative writes (`+=`, set-union `|=`) merge by
construction, so N writers can hit the same key at once and every contribution
lands.

## Try it in 30 seconds

```sh
docker run -it --rm ghcr.io/orlyatomics/orly repl
```

```
(true) effecting { *<['hits']>::(int) += 1; };
(true) effecting { *<['hits']>::(int) += 1; };
*<['hits']>::(int);                              --> 2

(true) effecting { *<['tags']>::({str}) |= {'blue'}; };
(true) effecting { *<['tags']>::({str}) |= {'green'}; };
*<['tags']>::({str});                            --> ["blue", "green"]
```

No `new`, no read-modify-write, no lock — `+=` on an absent key folds from the
monoid identity (`0` / the empty set), so there is nothing to create first.

Those two writes happen to come from one REPL, which only shows you the
*operator*. The claim that matters is that they need not be ordered,
coordinated, or from the same client:
**[`examples/agent-swarm/`](examples/agent-swarm/)** runs 8 agents extracting
from 40 documents concurrently into one graph, deliberately colliding on hot
keys, and CI checks every contribution against independently-derived ground
truth — 147 provenance records, 105 counters, 76 pairs, zero lost writes.

## Point your agents at it

`orly-mcp` is an [MCP](https://modelcontextprotocol.io) server, so an agent
runtime can use Orly as shared memory without touching the query language:

```jsonc
// claude_desktop_config.json / .mcp.json / Cursor
{
  "mcpServers": {
    "orly": {
      "command": "npx",
      "args": ["-y", "orly-mcp"],
      "env": { "ORLY_URL": "ws://127.0.0.1:8082/" }
    }
  }
}
```

Two agents given the same pov id share one graph and merge concurrently —
that topology is **[`examples/mcp-agent-duet/`](examples/mcp-agent-duet/)**,
gated in CI, with a recipe for running it as two live Claude Code sessions.

## What it is underneath

A non-relational database built around *Points of View*, causally-ordered
merge, and a compiled query language (*Orlyscript*).

Every value is a fold over its own history, so a point-in-time read is just a
query at an earlier version. The clearest showcase is a small
**[parimutuel prediction market](examples/prediction-market/)** ("Polymarket
clone"): N traders bet on one market *concurrently* — zero coordination, not a
single bet lost — the implied prices are a read-time fold of the trade log, and
the price history is time-travel. Build on it from **[Python](clients/python)**,
**[Go](clients/go)**, **[Zig](clients/zig)**, or **[TypeScript](clients/ts)** (browser + Node), all
speaking the same [WebSocket + JSON protocol](docs/PROTOCOL.md).

## Features

> The concurrency model behind the first two features — POVs, the Tetris merge, sequence-number ordering, and commutative field-call folding — is documented in depth in [`docs/architecture.md`](docs/architecture.md).

- **Points of View.** Optimistic concurrency without locking. Each client makes changes in its own private POV — a small sandbox — which eventually propagates into shared POVs and then into the global POV (the whole database). Field calls (`x += 1`) are preferred over field changes (`x = x + 1`) because they merge commutatively.
- **Reviewing a POV before it lands.** A paused POV holds its writes back from its parent, and a review workflow can see and manage them: `diff_pov` lists what the POV changed (added, changed and removed keys, and for `+=`/`|=` the accumulated delta), by key range and keyset-paged; `discard_pov` throws the changes away and frees them; a POV made with `<{.conflicts: "report"}>` (or `"refuse"`) reports (or refuses) a promotion that overwrites a key its parent changed after the fork, while commutative updates never conflict. Over WebSocket, the binary protocol and the Python, Go and TypeScript clients; see [`docs/pov-review.md`](docs/pov-review.md) and the GRC-20 walkthrough in [`examples/grc20-pov`](examples/grc20-pov) ([#746](https://github.com/orlyatomics/orly/issues/746)).

- **Causal ordering.** Merge-conflict resolution defines its "time line" by update causality (per-repo sequence order) rather than clock time. (The 2014 pitch called this the _Flux Capacitor_; that standalone subsystem was retired in [#262](https://github.com/orlyatomics/orly/issues/262) and the ordering now lives in the indy engine.) Internal mechanism; the original pitch also implied a built-in user-facing "time travel" query operator which the engine never grew — but the *capability* falls out in user-space by encoding a version axis in the key and folding on read, with no engine changes. See the [bitcoin](examples/bitcoin-time-travel/), [GRC-20](examples/grc20-pov/), and [prediction-market](examples/prediction-market/) examples.

- **Orlyscript.** A high-level, compiled, type-safe, functional query and programming language. Sources are compiled to `.so` packages that `orlyi` loads at runtime. Supports inline tests, native compilation, and most of the niceties of a real language rather than just a query DSL. Key ranges page by keyset (`keys (T) @ <[...]> after last take n`), so a deep page costs what the first one does; see [paging through keys](docs/walkthrough.md#paging-through-keys).

- **Stored records.** Create records with `new` and replace an existing record with whole-value assignment (`*<[key]>::(record_type) <- value;`), or update individual fields. Deleting and re-creating the same key in one effect block is a conflicting update; use assignment instead. See the [record replacement example](docs/walkthrough.md#replacing-a-stored-record).

- **Sum types (tagged unions).** Typed heterogeneous values — `<| Text(str) | Number(int) | Deleted |>` — that construct, store (including *sets* of differently-tagged variants), and read back like any other value, with exhaustive compile-time-checked `when` matching and payload binders (`Number(n): n + 1`). This lets a heterogeneous event log be folded into its current state *in the engine* — latest-write-wins, tombstones, formatting — rather than in the driver; see the [GRC-20 example](examples/grc20-pov/). Variants may also be **recursive** ([#103](https://github.com/orlyatomics/orly/issues/103)): `tree is <| Leaf(int) | Branch(<{.l: tree, .r: tree}>) |>;` declares a binary tree, constructed through the type's name (`tree.Leaf(1)`, `tree.Branch(<{...}>)`) and folded with ordinary recursive functions plus `when`. Recursive values are fully first-class: they compute, compare, live in sets, **store and read back** (`<-` / `*`), and travel to clients over the wire — including mutually-recursive variant groups ([#115](https://github.com/orlyatomics/orly/issues/115), [#116](https://github.com/orlyatomics/orly/issues/116)) — and can be **widened** to a superset variant with `as` ([#104](https://github.com/orlyatomics/orly/issues/104)).

- **Single-node first.** Fail-over / replication / hundreds of thousands of transactions per second on one node. Sharding was designed for but never built; that part of the original pitch is currently aspirational. The master/slave pair (attach, join-time sync, live replication, failover promotion, bulk-import hand-off) is exercised end-to-end in CI ([#367](https://github.com/orlyatomics/orly/issues/367)/[#501](https://github.com/orlyatomics/orly/issues/501)); note that bulk imports are a solo-server operation — a replicating master refuses them ([#498](https://github.com/orlyatomics/orly/issues/498)).

- **Restart durability.** A disk-backed `orlyi` gives everything durable back after a graceful stop/restart ([#435](https://github.com/orlyatomics/orly/issues/435), gated in CI by `tests/restart_test.sh`): global data, sessions (`resume session` works across restarts), index-id mappings, and the installed-package registry (packages auto-reinstall; an uninstall stays uninstalled). Graceful shutdown waits (up to 30 s) for Tetris to promote every POV's backlog and then flushes dirty memory layers, so it loses no acknowledged write unless that wait runs out (paused POVs aren't waited for), and it finishes under write load ([#440](https://github.com/orlyatomics/orly/issues/440), [#744](https://github.com/orlyatomics/orly/issues/744)), drains live client connections ([#460](https://github.com/orlyatomics/orly/issues/460)), and completes even against an unresponsive replication peer ([#461](https://github.com/orlyatomics/orly/issues/461)). One deliberate caveat: **POVs are ephemeral** — un-promoted pov-local updates do not survive a restart, and a pre-restart pov id is refused with a clean error rather than silently resurrected empty ([#439](https://github.com/orlyatomics/orly/issues/439)); see [`docs/PROTOCOL.md`](docs/PROTOCOL.md).
- **Crash durability.** An `ok` is not a durability promise on any POV, safe or fast: the write has committed in memory, and it is on disk once Tetris has promoted it to the global POV and the global POV's memory merge has written it. A crash can lose the newest acknowledged writes, never older ones: per POV, what survives is a prefix of the commit order, batches survive whole or not at all, `+=` is applied once, and the loss is at most the Update pool's worth of transactions. `tests/kill_campaign.sh` checks this by SIGKILLing `orlyi` under write load 40 times; see [`docs/durability.md`](docs/durability.md) ([#730](https://github.com/orlyatomics/orly/issues/730)).

- **A reopened store is checked before it serves.** At open, `orlyi` checks that every block on disk belongs to exactly one file, base image or append log, and that each repo's files cover disjoint sequence ranges. A leaked block or an overlapping range is logged loudly; a block a live file owns that the allocator thinks is free refuses the open, because the next file written could overwrite it (`--open_check=false` skips the check, for recovery) ([#700](https://github.com/orlyatomics/orly/issues/700)).

- **A full disk refuses writes, not the server.** When free space falls below a reserve (`--disk_reserve_pct`, default 10%; or `--disk_reserve_mb`) plus what running merges still need, `orlyi` refuses writes with `"status": "insufficient_storage"` and keeps serving reads, new sessions and new POVs. The last part of the reserve is kept for session state and the file map, so they keep working on a disk that data has filled. Writes resume by themselves when space comes back. The reporting port's `Write Admission` line shows the state ([#590](https://github.com/orlyatomics/orly/issues/590)).

- **Full memory pools refuse writes, not the server.** Writes wait in fixed-size memory pools until merges flush them, and the merges need room in those pools to work. `orlyi` keeps a quarter of each pool for them (`--memory_reserve_pct`, default 25) and refuses writes that would use it with `"status": "insufficient_memory"`, while reads, new sessions and new POVs carry on. A write that would use it first waits, first come first served, up to `--admission_wait_ms` (default 500) for the merges to free room, so a burst is paced rather than refused ([#765](https://github.com/orlyatomics/orly/issues/765)). Writes are accepted again once the merges catch up, usually within seconds. The reporting port's `Memory Admission` line shows the state ([#607](https://github.com/orlyatomics/orly/issues/607)). A single write too big ever to fit through a merge, one with more entries than half the Update Entry pool's reserve, is refused with `"status": "write_too_large"` instead. That one is not retryable: split the batch. The limit is `--update_entry_pool_size` × `--memory_reserve_pct` / 100 / 2 (25,000 entries at the defaults), and the pool shrinks with the memory budget (`--memory_budget_mb` or the container limit), so small containers hit it sooner; see [`docs/PROTOCOL.md`](docs/PROTOCOL.md) ([#687](https://github.com/orlyatomics/orly/issues/687)).

- **A read too big for memory is refused, not run.** Each method call has a budget of rows it may walk and result memory it may build, derived from the memory budget (a sixteenth of it, at least 16 MiB, and one row per 256 bytes of that: 1,048,576 rows and 256 MiB at 4 GiB). A call that passes either is stopped where it is and refused with `"status": "read_too_large"`, instead of running on until `orlyi` runs out of memory. Set them with `--read_budget_rows` and `--read_budget_mb`; `--read_budget_mb=0` turns the budget off ([#694](https://github.com/orlyatomics/orly/issues/694)). Values a call computes without reading rows, such as `[0..n] as [int]`, are charged as they are built, and the elements its sequences yield count as steps (`--read_budget_steps`), so a runaway computation is refused within its budget too ([#729](https://github.com/orlyatomics/orly/issues/729)).

## Quick start

**Docker** — the engine itself is Linux-only, so this is also the macOS/Windows
path. The published image is multi-arch (`linux/amd64` and `linux/arm64`), so
Apple Silicon pulls a native image rather than running under emulation. A cold
`docker run ... repl` answers in ~2s:

```sh
docker run --rm -p 127.0.0.1:8082:8082 ghcr.io/orlyatomics/orly
```

That's a solo mem-sim server with the `sample`, `graph`, and `market` example
packages pre-compiled and installable, speaking the
[WebSocket + JSON protocol](docs/PROTOCOL.md) on `ws://127.0.0.1:8082/` —
point any client driver ([`clients/`](clients/)) or the
[MCP server](clients/mcp/) at it. `orlyc` and the source headers are in the
image, so it compiles your own `.orly` packages too
(`docker exec <ctr> orlyc -o /var/lib/orly/packages yourpkg.orly`). Extra
`docker run ... <flags>` pass straight through to `orlyi`
([#530](https://github.com/orlyatomics/orly/issues/530)).

`-p 127.0.0.1:8082:8082` publishes the port on this machine's loopback only.
Without a shared secret Orly has no authentication, so `-p 8082:8082`, which
publishes it on every interface of the host, lets anyone who can reach the host
read and write the database; do that only on a network you trust, or set a
token. See [Security](#security).

The `repl` mode shown at the top of this README is the same image
([#538](https://github.com/orlyatomics/orly/issues/538)).

**Memory.** `orlyi` sizes its fiber stacks, caches and pools from a memory
budget, and a container's `--memory` limit caps it, so
`docker run --memory=1g ...` plans for 1 GiB and stays inside it. Without a
limit the image plans for 4 GiB. Set `-e ORLY_MEMORY_BUDGET_MB=<MiB>` to
change that (a smaller `--memory` still wins). The startup log says what it
chose:

```
memory budget: 1024 MiB, from the cgroup limit in /sys/fs/cgroup/memory.max (free RAM 14599 MiB, --memory_budget_mb=4096 is larger)
memory plan: 732 MiB of the 1024 MiB budget; 356 MiB fixed (process baseline 32, append log 4, mem_sim_mb 256, mem_sim_slow_mb 64), 128 MiB kept for the heap, the rest scaled by 0.131836
```

The smallest limit the image runs in is **768 MiB** (`--memory=768m`). Below
747 MiB, `orlyi` refuses to start and says how much it needs. The floor is the
mem-sim volumes (`ORLY_MEM_SIM_MB` + `ORLY_MEM_SIM_SLOW_MB`, 320 MiB by
default) plus about 430 MiB: the process, 128 MiB for the heap, and pools just
large enough that their merge reserve holds one memory merge's copy of 1,000
writes, with at least 64 fiber frames. Smaller mem-sim volumes lower it by the
same amount. At 768 MiB the image passes the write-and-read-back, pool-pressure
and memory-full smokes. Measured on native arm64
([#669](https://github.com/orlyatomics/orly/issues/669)):

| `--memory` | idle | peak, pool-pressure smoke |
| -- | -- | -- |
| 768 MiB | 524 MiB | 569 MiB |
| 1 GiB | 719 MiB | 752 MiB |
| 2 GiB | 1.40 GiB | 1.44 GiB |
| 4 GiB | 2.81 GiB | 2.85 GiB |

Compiling a package inside the container (`docker exec ... orlyc`) needs about
400 MiB more, nearly all of it the `g++` run that builds the package; the test
server `orlyc` stands up for a package's `test` blocks peaks at about 150 MiB,
after `g++` exits, and a package without tests gets none. Leave that room
between the budget and the limit, for example
`--memory=1280m -e ORLY_MEMORY_BUDGET_MB=800` (measured peak 948 MiB).

**From source** — system dependencies (Ubuntu 24.04):

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

The three production binaries land in `../out_orly/debug/` (or `../out_orly/release/`):

| Binary | Purpose |
| --- | --- |
| `orly/orlyc` | Orlyscript compiler |
| `orly/server/orlyi` | Database server |
| `orly/client/orly_client` | Interactive client shell |

`orlyc` compiles incrementally (#312): recompiling an unchanged package into
the same output directory skips codegen and the gcc/link step outright (keyed
on a source-content hash plus the compiler's build stamp; `--debug` bypasses
the cache). Check-only modes: `--syntax-only` stops after parsing,
`--semantic-only` after type-checking — neither writes output — and
`--transient-cc` keeps only the linked `.so`, removing the generated C++
intermediates.
Diagnostics name only your code's position; `--compiler-locations` appends
the compiler source line that raised each one, for filing a compiler bug.

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

The same job also builds `orlyi` and `orlyc` under TSan and runs the
memory-drain smoke against that server
([`clients/smoke/run-tsan-server.sh`](clients/smoke/run-tsan-server.sh), #713),
so WebSocket sessions, Tetris, the merges and the reporting port are covered
too. It fails on any un-suppressed report from `orlyi`, after a negative
control that makes `orlyi` race on purpose and requires TSan to report it.

## Security

By default Orly has **no authentication or authorization**. Any client that can reach
`orlyi` can read and write any data its installed packages reach, and install
or uninstall packages; `set user id` is attribution a client asserts, not an
identity. Traffic isn't encrypted. Run `orlyi` only where every client that
can reach it is trusted: on one host, on a private network, or behind an
application that authenticates its users and talks to `orlyi` for them
([#705](https://github.com/orlyatomics/orly/issues/705)).

- **Listeners bind loopback by default.** On a bare host, the client
  (19380), WebSocket (8082) and reporting (19388) listeners bind `127.0.0.1`.
  `--bind_address=0.0.0.0` listens on every interface, or pass one
  interface's address. `orlyi --log_info` logs each bound address at startup.
- **Replication belongs on a private network.** The replication listener
  (`--slave_port_number`, 19381) binds every interface by default, so a slave on
  another host can reach it. Without a replication token it is just as
  unauthenticated and carries all the data: set `--slave_bind_address` to the
  private network's address.
- **The Docker image binds every interface inside the container**, because
  `docker run -p` needs that. The `-p` mapping then decides who can connect;
  publish to `127.0.0.1` as above unless clients on other machines need it.
- **`compile` over WebSocket is off.** That statement builds the source it is
  sent with the server's C++ compiler, and `install` then loads it into the
  server process, so it lets a client run code inside `orlyi`. It is refused
  with `"status": "remote_compile_disabled"` unless `orlyi` is started with
  `--allow_remote_compile`, both on a bare host and in the image. Compile
  packages with `orlyc` and `install` them instead, as every client and
  example here does.
- **An optional shared secret** ([#710](https://github.com/orlyatomics/orly/issues/710)).
  Start `orlyi` with `--auth_token_file=<path>` (or `ORLY_AUTH_TOKEN_FILE`, or
  the token in `ORLY_AUTH_TOKEN`) and every WebSocket and binary-protocol
  connection must present that token before any statement runs; one without
  it, or with a wrong one, is refused with `"status": "unauthorized"`. A slave
  must present the replication token (`--replication_token_file`, by default
  the same token) to join a master. The Python, Go and TypeScript clients, the
  MCP server, the REPL and `orly_client` read `ORLY_AUTH_TOKEN_FILE` or
  `ORLY_AUTH_TOKEN`. Off by default, and with no token nothing changes on the
  wire. Turning it on means giving the token to **every client and replica**
  first; `docs/PROTOCOL.md` has the order. It is one secret for all clients,
  not user accounts.
- **TLS goes in a reverse proxy.** `orlyi` doesn't speak TLS; keep it on
  loopback and put a proxy such as Caddy in front of it. `docs/PROTOCOL.md`
  has a Caddyfile for the WebSocket and the binary port.

[`docs/PROTOCOL.md`](docs/PROTOCOL.md) has the details.

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

Why it matters: this is exactly the topology AI extraction pipelines have — many agents reading disjoint chunks, emitting overlapping facts into one graph — and every conventional database forces you to invent a coordination protocol on top. Orly gives it to you for free, as a direct consequence of how field calls work. The driver never reads-then-writes; it only ever calls the commutative functions in [`graph.orly`](examples/agent-swarm/graph.orly).

And the graph it builds is **traversable**: cooccurrences are stored as a symmetric `<['adj', a, b]>` adjacency — a trailing-`free` prefix scan the sorted index seeks straight to (index-free adjacency) — so bounded k-hop reachability is a `reduce` over a depth range with a visited set, no new engine primitive. Concurrent multi-writer construction *and* transitive graph queries in one store, which Neo4j (single-primary writes) and Cayley (a query layer over a store) don't combine.

### [`examples/mcp-agent-duet/`](examples/mcp-agent-duet/) — two MCP agents, one shared graph

`agent-swarm` lifted to the layer real agent runtimes use: two independent agent processes, each speaking only [MCP](https://modelcontextprotocol.io) through its own [`orly-mcp`](clients/mcp/) server instance, build one knowledge graph (same [`graph.orly`](examples/agent-swarm/graph.orly) schema) in **one shared POV, concurrently** — the agents share exactly one string, the pov id. The CI-gated verifier checks that corroborated tags keep one provenance record *per agent*, every counter lands, and a `reach()` traversal seeded at an entity only agent A wrote arrives at entities only agent B wrote. The README includes the recipe for running the same topology with two live Claude Code sessions ([#528](https://github.com/orlyatomics/orly/issues/528)).

### [`examples/grc20-pov/`](examples/grc20-pov/) — GRC-20 knowledge graph: event-sourced + concurrent editors + time-travel

A reference impl of Geo / The Graph's [GRC-20](https://github.com/geobrowser/grc-20) knowledge-graph standard. GRC-20 models knowledge as append-only ops (`CreateEntity`, `SetProperty`, `CreateRelation`, `DeleteEntity`) — exactly the shape Orly stores natively. The op is a **typed tagged union** (`<| Text(str) | Number(int) | Relation(str) | Deleted |>`), and every op becomes one `|=` of an event record (`<{.ts: int, .editor: str, .op: ...}>`) into a per-`(entity, property)` history set — a set of variant-bearing records, storable since [#90](https://github.com/orlyatomics/orly/issues/90) / [#96](https://github.com/orlyatomics/orly/issues/96), so the typed op travels end to end with no string packing. Concurrent editors stream into the same shared POV with **no coordination**; reads replay the timestamp-sorted log — latest-write-wins, tombstones, value formatting, time-travel — *in the engine*, via a `reduce` + an exhaustive `when` match, not in the driver.

The demo runs three phases over a corpus of six Greek philosophers:

| Phase | Editor | What |
|---|---|---|
| 1 | `wiki` | biographical facts (`name`, `born`, `died`) |
| 2 | `stanford` | school of thought + `student_of` relations |
| 3 | both | concurrent race on `pythagoras.born` — both events survive in history |

After each phase the demo prints a snapshot reconstructed from the event log; the final editorial diff shows per-editor event counts and overlapping entities. This example is the synthesis of the others: time-travel from [`bitcoin-time-travel`](examples/bitcoin-time-travel/) and [`wikipedia-categories`](examples/wikipedia-categories/), concurrent multi-writer from [`wikipedia-pageviews`](examples/wikipedia-pageviews/) and [`agent-swarm`](examples/agent-swarm/), applied to a published graph standard from another project rather than a synthetic workload — and it owns the GRC-20 op vocabulary and the entire replay *in orlyscript*, using the [sum-type](#features) `reduce` + `when` fold, with the driver reduced to streaming typed events and reading resolved values.

A second package in the same directory, [`claims.orly`](examples/grc20-pov/claims.orly), goes past plain triples: every statement is a claim that keeps its source, so two sources asserting the same value stay two claims, each with a confidence, a time qualifier and citations. Readers choose which sources they trust and get an honest answer (agreed, a conflict with every position side by side, unknown value, no claim, or "only sources you didn't select say so"), with identity by entity id and never by name, and methods for point reads, one-hop neighbours, keyset range pages and cross-source listings ([#747](https://github.com/orlyatomics/orly/issues/747)).

### [`examples/crdt-text/`](examples/crdt-text/) — collaborative text editing: the database *is* the CRDT merge

A Google-Docs-style collaborative editor where Orly's commutative storage **is** the conflict-free merge — no operational transform, no central reconcile, no lock. It's a [Logoot](https://hal.inria.fr/inria-00432368/document) sequence CRDT: every character gets a dense, totally-ordered position id, so the document is an unordered *set* of `(position, char)` entries and the visible text is the engine's read-time fold — sort by position, drop tombstoned, concatenate (the same `sorted_by` + `reduce` machinery as `grc20-pov`, over a *sequence* instead of an event log). Two editors stream inserts and deletes into one shared POV with **no coordination** and converge; concurrent inserts at the same spot both survive (distinct dense positions, ties broken on `(site, clock)`), so nothing is lost. The one piece of real CRDT logic — `between(p, q)`, the dense-order position allocation — is ~12 lines in the driver; the merge, convergence, tombstones, and time-travel (version history) are all the engine. The self-check asserts three independent readers render byte-identical text.

All six examples ship two equivalent drivers — Python (`./run.sh`) and Go (`./run-go.sh`) — and are smoke-tested in CI on every push.

## Walkthrough

[`docs/walkthrough.md`](docs/walkthrough.md) — compile an Orlyscript package with `orlyc`, load it into a running `orlyi`, and invoke a method on it via `orly_client`. The full pipeline end to end.

[`docs/PROTOCOL.md`](docs/PROTOCOL.md) — the WebSocket + JSON client protocol an application uses to talk to a running `orlyi` (the path the `examples/` drivers use): connection, statements, and JSON marshaling.

**Client libraries** implement that protocol so apps don't hand-roll it: [`clients/python`](clients/python) (`orly`), [`clients/go`](clients/go) (`orly`), [`clients/zig`](clients/zig) (`orly`), and [`clients/ts`](clients/ts) ([`@orlyatomics/orly`](https://www.npmjs.com/package/@orlyatomics/orly) on npm — typed, browser + Node). The `examples/` drivers run on them. For AI-agent runtimes, [`clients/mcp`](clients/mcp) ([`orly-mcp`](https://www.npmjs.com/package/orly-mcp), `npx -y orly-mcp`) wraps the TS driver in an [MCP](https://modelcontextprotocol.io) server, so any MCP-speaking agent can use a running `orlyi` as conflict-free shared memory — N agents share a POV id and write concurrently with the engine as the merge ([#526](https://github.com/orlyatomics/orly/issues/526)).

**Interactive orlyscript** — [`clients/repl`](clients/repl) ([`orly-repl`](https://www.npmjs.com/package/orly-repl) on npm) is a REPL against a running `orlyi`: type an expression and see its value, define functions and call them, write through a POV and read back — no package files by hand. Each entry is compiled with `orlyc` into a synthetic package and installed behind the scenes ([#535](https://github.com/orlyatomics/orly/issues/535)).

## Supported platforms

Linux. Verified on Ubuntu 24.04.

**x86-64** and **aarch64**. Both are built and released against: the published
docker image is a multi-arch manifest, each architecture built on its own native
runner and gated by the same smokes — the MCP smoke, an in-container `orlyc`
compile, and the REPL — run on that architecture's own image.

The port itself needed exactly two changes, which says more about the codebase
than about the port: `_mm_prefetch` became `__builtin_prefetch`, and a `-msse2`
flag that was a no-op on x86-64 was dropped ([#548](https://github.com/orlyatomics/orly/issues/548)). There is no inline
assembly anywhere in the tree, and the fiber fast path is `setjmp`/`longjmp`
bootstrapped from `ucontext` — both POSIX — so there was no hand-rolled context
switch to port. aarch64 runs `make test` in CI: 1753 build jobs, 203 test
binaries, zero failures on a native arm runner.

Two aarch64-specific defects have been found and fixed, and they share one
shape worth knowing: a fiber that has migrated OS threads reads a thread-local
through a thread pointer the compiler computed before the switch, so it gets the
*previous* thread's value. x86-64 re-derives the TLS base at every access, so
only arm shows it, and only in optimised builds. `orlyc` deadlocked
([#554](https://github.com/orlyatomics/orly/issues/554)), and the `v0.1.0`
arm64 image segfaulted just after the first write
([#578](https://github.com/orlyatomics/orly/issues/578), fixed in `v0.1.1`).
The image smokes passed `v0.1.0` anyway, because that crash lands after the
write has already answered.

So the debug `make test` job above stays **informational**, and what guards the
class is two other CI checks ([#556](https://github.com/orlyatomics/orly/issues/556)).
A lint rejects any bare thread-local that isn't allowlisted with a reason it can
never be read across a fiber switch. An `aarch64 release smoke` job builds
`orlyi`/`orlyc` release on an arm runner, compiles a package, and writes through
every POV flavour on 20 fresh servers, checking that each one survives.

Earlier releases probably work; not re-tested in the revival pass.

## Toolchain

| Component | Version |
| --- | --- |
| gcc | 13.3.0 |
| C++ standard | `-std=c++23` |
| boost (system) | 1.83 — includes `boost::beast` for the WebSocket surface |
| Python (for `lang_test.py`) | 3.12 |
| Go (for the optional Go driver in `examples/`) | 1.22+ |

Build flags live in [`root.jhm`](root.jhm); per-target overrides in `debug.jhm` / `release.jhm` / `bootstrap.jhm`.

## Project status

Dormant from 2019 until early 2026. A modernization pass brought the codebase back to building and testing cleanly on a current toolchain — `make debug`, `make test`, `make release`, and the Orlyscript `lang_test.py` harness all pass on Ubuntu 24.04 + gcc 13 — and a substantial language arc followed: sum types / tagged unions, recursive and mutually-recursive variants (storable and client-transmissible), variant widening, and recursive-return type verification. On the engine side, the LSM merge / disk-compaction subsystem — dormant since the original codebase because of an inverted scheduler deadline — was reactivated and its latent correctness and stability bugs fixed, making writes O(N) instead of O(N²) ([#227](https://github.com/orlyatomics/orly/issues/227)). See [`CHANGELOG.md`](CHANGELOG.md) for what's landed and [#10](https://github.com/orlyatomics/orly/issues/10) for the original revival status; the open issues track an engine-integrity and test-hardening backlog (latent revival defects plus sanitizer / coverage gaps). Contributions welcome.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the conventions CI enforces — build/test commands and the `TODO(#nnn)` comment rule. The build system (`jhm`) lives in [`jhm/`](jhm/); the bootstrap path is documented in [`bootstrap.sh`](bootstrap.sh). There's no formal style guide beyond that; match the surrounding code.

### IDE / clangd

Every successful `make debug` (or any `jhm` invocation that compiles C/C++) writes a [JSON Compilation Database](https://clang.llvm.org/docs/JSONCompilationDatabase.html) to `compile_commands.json` at the repo root. clangd, clang-tidy, IWYU, and most C++-aware editors pick it up automatically. The file is regenerated on every build, so it's `.gitignore`d.

---

<sub>README.md © 2010–2026 Atomic Kismet Company. Licensed under [Creative Commons Attribution-ShareAlike 4.0 International](http://creativecommons.org/licenses/by-sa/4.0/).</sub>
