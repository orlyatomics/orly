<p align="center">
  <img src="docs/branding/orly.png?raw=true" height="120" alt="Orly" />
</p>

<p align="center">
  <a href="https://github.com/orlyatomics/orly/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/orlyatomics/orly/actions/workflows/ci.yml/badge.svg" /></a>
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

- **Orlyscript.** A high-level, compiled, type-safe, functional query and programming language. Sources are compiled to `.so` packages that `orlyi` loads at runtime. Supports inline tests, native compilation, and most of the niceties of a real language rather than just a query DSL. Key ranges page by keyset (`keys (T) @ <[...]> after last take n`), so a deep page costs what the first one does; see [paging through keys](docs/walkthrough.md#paging-through-keys). Record fields may use keywords (`.id`, `.to`, `.start`) in literals, access and types; see [field naming](docs/walkthrough.md#record-field-names-and-keywords).

- **Stored records.** Create records with `new` and replace an existing record with whole-value assignment (`*<[key]>::(record_type) <- value;`), or update individual fields. Deleting and re-creating the same key in one effect block is a conflicting update; use assignment instead. See the [record replacement example](docs/walkthrough.md#replacing-a-stored-record).

- **Sum types (tagged unions).** Typed heterogeneous values — `<| Text(str) | Number(int) | Deleted |>` — that construct, store (including *sets* of differently-tagged variants), and read back like any other value, with exhaustive compile-time-checked `when` matching and payload binders (`Number(n): n + 1`). This lets a heterogeneous event log be folded into its current state *in the engine* — latest-write-wins, tombstones, formatting — rather than in the driver; see the [GRC-20 example](examples/grc20-pov/). Variants may also be **recursive** ([#103](https://github.com/orlyatomics/orly/issues/103)): `tree is <| Leaf(int) | Branch(<{.l: tree, .r: tree}>) |>;` declares a binary tree, constructed through the type's name (`tree.Leaf(1)`, `tree.Branch(<{...}>)`) and folded with ordinary recursive functions plus `when`. Recursive values are fully first-class: they compute, compare, live in sets, **store and read back** (`<-` / `*`), and travel to clients over the wire — including mutually-recursive variant groups ([#115](https://github.com/orlyatomics/orly/issues/115), [#116](https://github.com/orlyatomics/orly/issues/116)) — and can be **widened** to a superset variant with `as` ([#104](https://github.com/orlyatomics/orly/issues/104)).

- **Single-node first.** Fail-over / replication / hundreds of thousands of transactions per second on one node. Sharding was designed for but never built; that part of the original pitch is currently aspirational. The primary/replica pair (attach, join-time sync, live replication, failover promotion, bulk-import hand-off) is exercised end-to-end in CI ([#367](https://github.com/orlyatomics/orly/issues/367)/[#501](https://github.com/orlyatomics/orly/issues/501)); note that bulk imports are a solo-server operation — a replicating primary refuses them ([#498](https://github.com/orlyatomics/orly/issues/498)).

- **Restart durability.** A disk-backed `orlyi` gives everything durable back after a graceful stop/restart ([#435](https://github.com/orlyatomics/orly/issues/435), gated in CI by `tests/restart_test.sh`): global data, sessions (`resume session` works across restarts), index-id mappings, and the installed-package registry (packages auto-reinstall; an uninstall stays uninstalled). Graceful shutdown refuses new writes, waits for Tetris to promote every POV's backlog for as long as it keeps promoting (up to `--stop_promote_budget_s`, default 300 s, logging progress every 5 s), and then flushes dirty memory layers, so it loses no acknowledged write unless that wait runs out (paused POVs aren't waited for), and then it logs exactly how many acknowledged updates it will lose ([#769](https://github.com/orlyatomics/orly/issues/769)); give `docker stop --time` at least that budget. It finishes under write load ([#440](https://github.com/orlyatomics/orly/issues/440), [#744](https://github.com/orlyatomics/orly/issues/744)), drains live client connections ([#460](https://github.com/orlyatomics/orly/issues/460)), and completes even against an unresponsive replication peer ([#461](https://github.com/orlyatomics/orly/issues/461)). One deliberate caveat: **POVs are ephemeral** — un-promoted pov-local updates do not survive a restart, and a pre-restart pov id is refused with a clean error rather than silently resurrected empty ([#439](https://github.com/orlyatomics/orly/issues/439)); see [`docs/PROTOCOL.md`](docs/PROTOCOL.md).
- **Crash durability.** An `ok` is not a durability promise on any POV, safe or fast: the write has committed in memory, and it is on disk once Tetris has promoted it to the global POV and the global POV's memory merge has written it. A crash can lose the newest acknowledged writes, never older ones: per POV, what survives is a prefix of the commit order, batches survive whole or not at all, `+=` is applied once, and the loss is at most the Update pool's worth of transactions. `tests/kill_campaign.sh` checks this by SIGKILLing `orlyi` under write load 40 times; see [`docs/durability.md`](docs/durability.md) ([#730](https://github.com/orlyatomics/orly/issues/730)).
- **Exactly-once writes.** A write that carries an operation id (`try ... <{.op_id: "<uuidv7>"}>;`) is applied at most once however often it is sent, so a client that lost its connection can send it again; on a safe POV that holds across a SIGKILL and restart. Opt-in per call: a write with no id is byte for byte what it was. The Python, TypeScript, Go and MCP clients take an id per call and resend only when they have one. See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) ([#733](https://github.com/orlyatomics/orly/issues/733)).
- **Pipelined statements on one WebSocket connection.** A client can wrap a statement as `{"id": 17, "stmt": "...", "read": true}` and keep several in flight on one connection: reads run alongside each other and are answered as they finish, tagged with their `id`; a write waits for the statements before it and holds back the ones after, so a client reads its own writes. Bare statements behave as before. `--ws_pipeline_depth` (default 16) bounds how far ahead a connection may get. The clients do not take it yet. See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) ([#1033](https://github.com/orlyatomics/orly/issues/1033)).
- **Reads at a version.** `try {<global-pov>} ... <{.at: V}>;` (or `try {<global-pov>} at V ...;`) reads the store as it was at version V, the number a write receipt carries, back to the oldest version the disk merges have kept; anything older or not yet committed is refused with the readable range. `try ... at time "<RFC 3339>"` and `at save "<name>"` read at the version a time (with `--retention_seconds`) or a save point names, and `version of`, `save point`, `drop save point`, `hold version` and `release` complete the surface ([#889](https://github.com/orlyatomics/orly/issues/889)). Global POV and read-only calls only, over WebSocket; the clients do not take it yet. See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) ([#745](https://github.com/orlyatomics/orly/issues/745)).

- **A reopened store is checked before it serves.** At open, `orlyi` checks that every block on disk belongs to exactly one file, base image or append log, and that each repo's files cover disjoint sequence ranges. A leaked block or an overlapping range is logged loudly; a block a live file owns that the allocator thinks is free refuses the open, because the next file written could overwrite it (`--open_check=false` skips the check, for recovery) ([#700](https://github.com/orlyatomics/orly/issues/700)). `orlyi --create=false --check_only` runs the same check on a stopped store, prints what it found and exits without serving: status 0 if clean, 1 for problems that would not stop it opening, 2 if unsafe ([#748](https://github.com/orlyatomics/orly/issues/748)).

- **A full disk refuses writes, not the server.** When free space falls below a reserve (`--disk_reserve_pct`, default 10%; or `--disk_reserve_mb`) plus what running merges still need, `orlyi` refuses writes with `"status": "insufficient_storage"` and keeps serving reads, new sessions and new POVs. The last part of the reserve is kept for session state and the file map, so they keep working on a disk that data has filled. Writes resume by themselves when space comes back. The reporting port's `Write Admission` line shows the state ([#590](https://github.com/orlyatomics/orly/issues/590)). A safe POV's write-ahead log is refused the same way when its ring (`--wal_mb`) is full of records that no data file holds yet: the write is refused with `insufficient_storage` before anything is applied, and accepted again once a flush and checkpoint free room. Client writes leave an eighth of the ring for the server's own promotion writes (and for `pause`/`unpause`), so a full ring drains rather than hangs; a single safe write whose log record is more than half that eighth is refused with `"status": "write_too_large"`, and a replica waits for room rather than refusing a replicated write ([#891](https://github.com/orlyatomics/orly/issues/891)). The ring also keeps room for the promotions of updates still waiting in POVs, so it can't fill to the point where none of them can move ([#959](https://github.com/orlyatomics/orly/issues/959)).

- **Full memory pools refuse writes, not the server.** Writes wait in fixed-size memory pools until merges flush them, and the merges need room in those pools to work. `orlyi` keeps a quarter of each pool for them (`--memory_reserve_pct`, default 25) and refuses writes that would use it with `"status": "insufficient_memory"`, while reads, new sessions and new POVs carry on. A write that would use it first waits, first come first served, up to `--admission_wait_ms` (default 500) for the merges to free room, so a burst is paced rather than refused ([#765](https://github.com/orlyatomics/orly/issues/765)). Writes are accepted again once the merges catch up, usually within seconds. The reporting port's `Memory Admission` line shows the state ([#607](https://github.com/orlyatomics/orly/issues/607)). A single write too big ever to fit through a merge, one with more entries than half the Update Entry pool's reserve, is refused with `"status": "write_too_large"` instead. That one is not retryable: split the batch. The limit is `--update_entry_pool_size` × `--memory_reserve_pct` / 100 / 2 (25,000 entries at the defaults), and the pool shrinks with the memory budget (`--memory_budget_mb` or the container limit), so small containers hit it sooner; see [`docs/PROTOCOL.md`](docs/PROTOCOL.md) ([#687](https://github.com/orlyatomics/orly/issues/687)).

- **A read too big for memory is refused, not run.** Each method call has a budget of rows it may walk and result memory it may build, derived from the memory budget (a sixteenth of it, at least 16 MiB, and one row per 256 bytes of that: 1,048,576 rows and 256 MiB at 4 GiB). A call that passes either is stopped where it is and refused with `"status": "read_too_large"`, instead of running on until `orlyi` runs out of memory. Set them with `--read_budget_rows` and `--read_budget_mb`; `--read_budget_mb=0` turns the budget off ([#694](https://github.com/orlyatomics/orly/issues/694)). Values a call computes without reading rows, such as `[0..n] as [int]`, are charged as they are built, and the elements its sequences yield count as steps (`--read_budget_steps`), so a runaway computation is refused within its budget too ([#729](https://github.com/orlyatomics/orly/issues/729)).
- **A long read doesn't hold up short ones.** Statements run as fibers on a few fast runners, and a fiber doesn't give way while it computes. So a method call that has run for 50 µs without a break (`--read_yield_us`) yields to another statement that is ready to run on its runner, and a point read waits about that long behind a scan rather than for all of it. It checks the clock every 8 rows it walks (`--read_yield_rows`), and it counts toward its runner's load as it goes, so new statements go elsewhere ([#1015](https://github.com/orlyatomics/orly/issues/1015)).
- **Writes can commit off the fast runners.** A write's commit, the work after its method has run, can take a share of the fast cores the reads need under a heavy ingest. `--edit_runner=slow` commits each write on a slow runner (`--slow_cores`) and brings the statement back after; `--edit_runner=spill` does so only while a slow runner has fewer than `--edit_spill_load` commits on it. The default, `fast`, commits where the statement ran. `slow` trades some ingest latency for read throughput ([#1038](https://github.com/orlyatomics/orly/issues/1038)).

## Quick start

**Docker.** This is also the macOS and Windows path, since the engine is
Linux-only. The image is multi-arch (`linux/amd64` and `linux/arm64`):

```sh
docker run --rm -p 127.0.0.1:8082:8082 ghcr.io/orlyatomics/orly
```

That's a solo server with the `sample`, `graph` and `market` example packages,
speaking the [WebSocket + JSON protocol](docs/PROTOCOL.md) on
`ws://127.0.0.1:8082/`. Point any [client](clients/) or the
[MCP server](clients/mcp/) at it. `orlyc` is in the image, so it compiles your
own `.orly` packages too. Keep the `127.0.0.1:` in `-p`: without a token Orly
has no authentication (see [Security](#security)).
[`docs/docker.md`](docs/docker.md) covers memory limits, sizing and flags.

**From source** on Ubuntu 24.04 (on macOS, use [`./dev`](CONTRIBUTING.md#getting-started-on-macos-or-any-non-linux-machine)):

```sh
make debug       # build everything
make test        # C++ unit tests
make release     # LTO-built production binaries
python3 tools/lang_test.py -d orly/data tests/lang_tests   # Orlyscript suite
```

Binaries land in `../out_orly/<config>/`: `orly/orlyc` (compiler),
`orly/server/orlyi` (server) and `orly/client/orly_client` (shell).
[`docs/building.md`](docs/building.md) has the apt packages, the sanitizer
builds, supported platforms, the toolchain and clangd setup.

## Security

By default Orly has **no authentication and no TLS**: any client that can reach
`orlyi` can read and write everything. Listeners bind loopback on a bare host;
an optional shared secret (`--auth_token_file`) and a TLS reverse proxy are
available. Read [`SECURITY.md`](SECURITY.md) before exposing it to a network.

## Examples

| Example | What it shows |
| -- | -- |
| [`agent-swarm`](examples/agent-swarm/) | 8 agents build one knowledge graph concurrently, colliding on hot keys, with provenance and zero lost writes |
| [`mcp-agent-duet`](examples/mcp-agent-duet/) | Two MCP agents share one graph through one POV id; includes a two-Claude-Code-session recipe |
| [`prediction-market`](examples/prediction-market/) | A parimutuel market: concurrent bets, prices folded on read, price history by time travel |
| [`wikipedia-pageviews`](examples/wikipedia-pageviews/) | 8 writers `+=` the same hot counters; every increment lands |
| [`bitcoin-time-travel`](examples/bitcoin-time-travel/) | Balance at a block height and on a fork, by putting the version and branch in the key |
| [`wikipedia-categories`](examples/wikipedia-categories/) | The same time-travel trick with set union instead of addition |
| [`grc20-pov`](examples/grc20-pov/) | A GRC-20 knowledge graph: typed event log, concurrent editors, replay in the engine, sourced claims |
| [`crdt-text`](examples/crdt-text/) | Collaborative text editing where the storage merge is the CRDT |
| [`recursive-variants`](examples/recursive-variants/) | Storing recursive sum types and reading them back over the wire |
| [`playground`](examples/playground/) | A browser playground for writing and running Orlyscript |

Each example's README explains the design and how to run it.

## Documentation

- [`docs/tour.md`](docs/tour.md): an engineering tour, how it is tested, and where to start reading the source.
- [`docs/walkthrough.md`](docs/walkthrough.md): compile a package, load it into `orlyi`, call a method.
- [`docs/architecture.md`](docs/architecture.md): POVs, the Tetris merge and commutative folding.
- [`docs/PROTOCOL.md`](docs/PROTOCOL.md): the WebSocket + JSON protocol.
- [`docs/durability.md`](docs/durability.md): what an `ok` promises, and what survives a crash.
- [`docs/tuning.md`](docs/tuning.md) and [`docs/benchmarks.md`](docs/benchmarks.md): performance tuning and benchmark results.
- **Clients:** [Python](clients/python), [Go](clients/go), [Zig](clients/zig), [TypeScript](clients/ts) ([`@orlyatomics/orly`](https://www.npmjs.com/package/@orlyatomics/orly)), the [MCP server](clients/mcp) ([`orly-mcp`](https://www.npmjs.com/package/orly-mcp)) and the [REPL](clients/repl) ([`orly-repl`](https://www.npmjs.com/package/orly-repl)).

## Project status

Dormant from 2019 until early 2026. A modernization pass brought the codebase back to building and testing cleanly on a current toolchain — `make debug`, `make test`, `make release`, and the Orlyscript `lang_test.py` harness all pass on Ubuntu 24.04 + gcc 13 — and a substantial language arc followed: sum types / tagged unions, recursive and mutually-recursive variants (storable and client-transmissible), variant widening, and recursive-return type verification. On the engine side, the LSM merge / disk-compaction subsystem — dormant since the original codebase because of an inverted scheduler deadline — was reactivated and its latent correctness and stability bugs fixed, making writes O(N) instead of O(N²) ([#227](https://github.com/orlyatomics/orly/issues/227)). See [`CHANGELOG.md`](CHANGELOG.md) for what's landed and [#10](https://github.com/orlyatomics/orly/issues/10) for the original revival status; the open issues track an engine-integrity and test-hardening backlog (latent revival defects plus sanitizer / coverage gaps). Contributions welcome.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the conventions CI enforces — build/test commands and the `TODO(#nnn)` comment rule. The build system (`jhm`) lives in [`jhm/`](jhm/); the bootstrap path is documented in [`bootstrap.sh`](bootstrap.sh). There's no formal style guide beyond that; match the surrounding code.

---

<sub>README.md © 2010–2026 Atomic Kismet Company. Licensed under [Creative Commons Attribution-ShareAlike 4.0 International](http://creativecommons.org/licenses/by-sa/4.0/).</sub>
