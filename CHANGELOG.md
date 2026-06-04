# Changelog

All notable changes to Orly.

The project does not currently cut numbered releases. Entries reference the originating pull request and the closed issue where applicable. Format loosely follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
- **Multi-agent knowledge-graph demo** (`examples/agent-swarm/`). N agents concurrently extract entities, tags, mentions, and cooccurrences into one shared POV using `|=` (set union) and `+=` (integer increment) — zero coordination, Python + Go drivers. (#68, 2026-06-03)
- **Wikipedia-pageviews demo** (`examples/wikipedia-pageviews/`). The concurrent-writers integration workload for the issue-#49 arc: 8 writers hammering 96 hot keys, every increment lands. Python + Go drivers. (#59, #61, 2026-06-02)
- **Wikipedia-categories demo** (`examples/wikipedia-categories/`). Same fold-over-keyed-versions trick as the bitcoin demo, with set union as the operator instead of integer addition. (#47, 2026-06-01)
- **Bitcoin time-travel demo** (`examples/bitcoin-time-travel/`). First demo: historical queries + branched-history multiverse queries via a key-encoded version axis. Python + Go drivers. (#43, #44, 2026-06-01)
- **Compaction-time fold of same-mutator runs** (`TFoldDataFile`). Phase-4 of the issue-#49 arc: at compaction, fold commutative mutation runs back into single values to bound on-disk size growth from the field-call mechanism. (#63, closes #55, 2026-06-02)
- **`compile_commands.json`** auto-emitted on every `make debug` for clangd / clang-tidy / IWYU. (#45, 2026-06-01)

### Fixed
- **Concurrent `+= n` field calls compose correctly under contention.** The headline fix of the 2026 arc. Without this, multiple writers calling `x += 1` on the same key in concurrent sessions lost roughly `(N-1)/N` of their writes — the Tetris merge converted same-key field calls to Assigns, clobbering each other. They now `TMutation::Augment` into a single deferred mutation that the read path folds into the correct value. Validated end-to-end by `examples/wikipedia-pageviews/`. (closes #49; #48, #50, #51, #52, #60, 2026-06-01 → 2026-06-02)
- **`TUpdateWalker` reads the on-disk Mutator byte correctly.** Walker was dropping the Mutator on read-back, silently converting commutative writes to Assign at file-load time — defeating the #49 fix as soon as data hit disk. (closes #53, #65, 2026-06-03)
- **Master→slave replication wire format carries the Mutator byte.** Without this, replicated commits arrived as Assign at the slave and lost commutativity across replicas. (closes #54, #66, 2026-06-03)
- **Skip the `TFoldDataFile` pass when the source has no non-Assign entries.** Perf win for Assign-heavy workloads where the fold pass has nothing to do. (closes #64, #67, 2026-06-03)

### Changed
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
