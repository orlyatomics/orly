# Changelog

All notable changes to Orly.

The project does not currently cut numbered releases. Entries reference the originating pull request and the closed issue where applicable. Format loosely follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
- **`when` over optionals.** A `T?` may be matched as the built-in sum `<| Known(T) | Unknown |>` — payload binder `Known(v)`, exhaustive `Known`/`Unknown` — so the `is known … else …` idiom becomes an exhaustive match. Plus a conformance spec pinning where an optional and the equivalent variant agree (`when`, `is`) and intentionally diverge (`==`, via auto-unwrap). Surface unification only; the type-level reduction of `TOpt` to a variant was evaluated and declined. (#167, #168, #105, 2026-06-22)
- **Recursive-return type verification** (#128 Option B). The type checker now verifies a recursive call's result in propagating positions — e.g. `g(x) + 1` where `g` returns `str`, or a recursive value fed to a constructor/argument of the wrong type — via a per-function `TypeCheck`-time fixpoint that also covers mutual recursion. (#165, #166, closes #128, 2026-06-22)
- **Variant widening with `as`** (#104). Widen a variant to a superset variant: direct, optional target, recursive (through a synthesized fold), optional / list / set / dict / nested-container payloads, mutually-recursive groups, and implicit widening at parameter binding and `if`/`when` branch joins. (#155–#164, closes #104, 2026-06-20 → 2026-06-22)
- **Recursive sum types** (#103). Self-referential variants — `tree is <| Leaf(int) | Branch(<{.l: tree, .r: tree}>) |>` — constructed through the type name and folded with recursive functions + `when`. Values **store and read back** (`<-` / `*`) and travel the WebSocket wire (#115), including mutually-recursive variant groups (#116) and self-references nested under records / containers / optionals. (#117, #120, #125, #130–#142, closes #103/#115/#116, 2026-06-12 → 2026-06-15)
- **Sum types / tagged unions** (#95). `<| Text(str) | Number(int) | Deleted |>`: construction, exhaustive compile-time-checked `when` with payload binders (`Number(n): n + 1`), and storage of *sets* of differently-tagged variants (#96). Optional-of-variant (#118) and fold ergonomics for all-recursive / variant-typed `when`/if-else arms (#126). (#97–#101, #99, #121, #127, closes #95/#96, 2026-06-05 → 2026-06-14)
- **Storable records, including set-of-records `|=`** (#90). Records and sets of records are now fully storable end-to-end. (#91–#94, closes #90, 2026-06-05)
- **GRC-20 knowledge-graph demo** (`examples/grc20-pov/`). Event-sourced + concurrent editors + time-travel over the GRC-20 op vocabulary, folded in-engine with `reduce` + an exhaustive `when` over a typed op union. (#89, #100, 2026-06-04 → 2026-06-05)
- **Collaborative-text CRDT demo** (`examples/crdt-text/`). A Logoot sequence CRDT where the engine's commutative storage *is* the conflict-free merge. (#106, 2026-06-05)
- **Subsystem header-documentation pass** across `base/`, `orly/type`, `orly/expr`, `orly/synth`, `orly/code_gen`, `orly/rt`, `orly/indy` (#70). (#78–#88, 2026-06-03 → 2026-06-04)
- **Multi-agent knowledge-graph demo** (`examples/agent-swarm/`). N agents concurrently extract entities, tags, mentions, and cooccurrences into one shared POV using `|=` (set union) and `+=` (integer increment) — zero coordination, Python + Go drivers. (#68, 2026-06-03)
- **Wikipedia-pageviews demo** (`examples/wikipedia-pageviews/`). The concurrent-writers integration workload for the issue-#49 arc: 8 writers hammering 96 hot keys, every increment lands. Python + Go drivers. (#59, #61, 2026-06-02)
- **Wikipedia-categories demo** (`examples/wikipedia-categories/`). Same fold-over-keyed-versions trick as the bitcoin demo, with set union as the operator instead of integer addition. (#47, 2026-06-01)
- **Bitcoin time-travel demo** (`examples/bitcoin-time-travel/`). First demo: historical queries + branched-history multiverse queries via a key-encoded version axis. Python + Go drivers. (#43, #44, 2026-06-01)
- **Compaction-time fold of same-mutator runs** (`TFoldDataFile`). Phase-4 of the issue-#49 arc: at compaction, fold commutative mutation runs back into single values to bound on-disk size growth from the field-call mechanism. (#63, closes #55, 2026-06-02)
- **`compile_commands.json`** auto-emitted on every `make debug` for clangd / clang-tidy / IWYU. (#45, 2026-06-01)

### Fixed
- **Lost-update / create-race hardening** (the #143 arc). Closed several windows where commutative writes could be lost or miscounted under contention: an engine create-race on commutative upsert of an absent key, a compaction fold undercount, a Tetris promotion-window guard, a cross-repo snapshot guard, and an agent-swarm read-back barrier; commutative upsert (`+=` / `|=`) of bare ops in the demos. (#145–#153, closes #143/#151, 2026-06-16 → 2026-06-20)
- **Scheduler teardown race.** (#154, closes #147, 2026-06-20)
- **Variant arm-name macro collision** — an arm named like a C++ macro (e.g. a single capital letter) no longer breaks generated code. (#122, closes #119, 2026-06-14)
- **`filter` deref codegen** + regenerated lang-test state baseline. (#107, #108, closes #73, 2026-06-08)
- **Concurrent `+= n` field calls compose correctly under contention.** The headline fix of the 2026 arc. Without this, multiple writers calling `x += 1` on the same key in concurrent sessions lost roughly `(N-1)/N` of their writes — the Tetris merge converted same-key field calls to Assigns, clobbering each other. They now `TMutation::Augment` into a single deferred mutation that the read path folds into the correct value. Validated end-to-end by `examples/wikipedia-pageviews/`. (closes #49; #48, #50, #51, #52, #60, 2026-06-01 → 2026-06-02)
- **`TUpdateWalker` reads the on-disk Mutator byte correctly.** Walker was dropping the Mutator on read-back, silently converting commutative writes to Assign at file-load time — defeating the #49 fix as soon as data hit disk. (closes #53, #65, 2026-06-03)
- **Master→slave replication wire format carries the Mutator byte.** Without this, replicated commits arrived as Assign at the slave and lost commutativity across replicas. (closes #54, #66, 2026-06-03)
- **Skip the `TFoldDataFile` pass when the source has no non-Assign entries.** Perf win for Assign-heavy workloads where the fold pass has nothing to do. (closes #64, #67, 2026-06-03)

### Changed
- **#128 Option A — `TAny` deferral at constructor / argument sites.** A recursive call's (as-yet-unresolved) result may flow through a variant-constructor payload or a function argument, the substrate for recursive transforms and variant widening; strictly verified later by Option B (above). (#129, 2026-06-15)
- **`base::TOpt` → `std::optional`** across the codebase. (#111, 2026-06-09)
- **Readable JSON lang-test baselines** so a changed baseline shows up in review (#112); CI ccache and an incremental-build mtime fix (#113, #114). (2026-06-09)
- `examples/wikipedia-pageviews/` — 30s WebSocket recv timeout so a hung `orlyi` surfaces as a failure within a bounded window; `DEMO_SCALE=small` knob for CI tractability. (#58, #62, 2026-06-02)

---

## [Revival — 2026-05-30 → 2026-05-31]

The project was dormant from 2019 until early 2026. The revival pass brought the codebase back to building and testing cleanly on Ubuntu 24.04 + gcc 13. See [#10](https://github.com/orlyatomics/orly/issues/10) for the full status and remaining gaps.

Highlights:

- **GitHub Actions CI on every push** — build / test / release / lang-tests / examples jobs, with caching, plus a daily uncached run as a stale-cache guard. (#15 – #20)
- **`docs/walkthrough.md`** — end-to-end compile / load / invoke pipeline document. (#20)
- **C++23 bump.** (#35)
- **WebSocket surface migrated** from vendored `websocketpp` (abandoned upstream) to `boost::beast`. (#34)
- **Repo layout refactor** for the modern toolchain. (#39)
- **Memcache fixes** — delete opcode, version opcode, unknown-opcode handling. (#22, #23, #26)
- **`mem_sim` stripe alignment** correctness fix. (#21)
- **Various build / signal / pump cleanups** across the codebase to clear warnings under gcc 13 + `-std=c++23`.

For the full revival history (50+ merges), see `git log master --merges --before=2026-06-01 --after=2026-05-29`.
