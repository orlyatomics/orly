# An engineering tour of Orly

The source still carries a lot of 2014-era C++. This page is a guide to what is
distinctive underneath it, with a link to the code, the test or the CI job for
each point.

## The core idea: writers that do not conflict

- **POVs.** Every client works in a private point of view; updates carry a
  per-repo sequence number, not a wall clock, and flow upward by promotion
  (private, shared, global). [`architecture.md`](architecture.md) §1.
- **Commutative merges.** `x += 1` and `s |= {e}` are deferred commutative
  mutations with no read-assertion, so concurrent writers on one key both
  promote and the value is folded on read. A read-modify-write (`x = x + 1`)
  carries an assertion and does conflict. Two kinds of write, treated
  differently on purpose: [`architecture.md`](architecture.md) §1.
- **Tetris promotion.** Competing updates are merged without locks and with a
  deterministic winner: [`architecture.md`](architecture.md) §3, and the
  measured throughput design in
  [`design/concurrent-merge-throughput.md`](design/concurrent-merge-throughput.md).
- **Compiled orlyscript.** `orlyc` compiles a package to C++ and a shared
  object that `orlyi` loads; its `test{}` blocks run on the same engine as
  production. [`walkthrough.md`](walkthrough.md).

## Tested the hard way

- **Sanitizers gate the build.** The `tsan` and `asan-smoke` jobs in
  [`ci.yml`](../.github/workflows/ci.yml) build the concurrency tests and
  `orlyi` itself under ThreadSanitizer and AddressSanitizer, and fail on any
  un-suppressed warning. The suppressions are listed in
  [`orly/tsan.supp`](../orly/tsan.supp).
- **Fault injection.** The storage layer has a fault-injection harness:
  [`orly/indy/fault_injection.test.cc`](../orly/indy/fault_injection.test.cc).
- **A kill-and-recover campaign.** [`tests/kill_campaign.py`](../tests/kill_campaign.py)
  kills `orlyi` under write load, restarts it and checks what survived against a
  stated bound on loss (workflow
  [`kill-campaign.yml`](../.github/workflows/kill-campaign.yml)). The contract
  it enforces is in [`durability.md`](durability.md): how much a crash can lose
  is bounded by memory (the Update pool), not by time.
- **Restart and graceful stop.** [`tests/restart_test.sh`](../tests/restart_test.sh)
  and [`tests/graceful_stop_test.py`](../tests/graceful_stop_test.py) run in CI.

## Refuse rather than fall over

- **Admission.** A write that the disk or memory can't take is refused with
  `insufficient_storage` or `insufficient_memory` instead of aborting the
  server: [`PROTOCOL.md`](PROTOCOL.md).
- **A per-read budget.** A read that would walk too many rows or build too much
  result is stopped and refused with `read_too_large`: [`PROTOCOL.md`](PROTOCOL.md)
  and the `--read_budget_*` flags.
- **An open-time consistency check.** Before it serves, `orlyi` checks that every
  block on disk belongs to exactly one file and that the files of a repo cover
  disjoint sequence ranges: [`durability.md`](durability.md) and the README's
  feature list.

## Reach and releases

- **Clients.** Python, Go, Zig and TypeScript libraries, a REPL, and an MCP
  server, all in [`clients/`](../clients), speaking the protocol in
  [`PROTOCOL.md`](PROTOCOL.md).
- **A published multi-arch image** (`linux/amd64` and `linux/arm64`), built by
  [`docker.yml`](../.github/workflows/docker.yml).
- **A written release process** in [`CONTRIBUTING.md`](../CONTRIBUTING.md), and
  per-release measurements in [`benchmarks.md`](benchmarks.md).

## Where to start reading

| To see | Open |
|---|---|
| a client connection end to end | [`orly/server/ws.cc`](../orly/server/ws.cc) and [`orly/server/session.cc`](../orly/server/session.cc) |
| how a package becomes C++ | [`orly/orlyc.cc`](../orly/orlyc.cc) and [`orly/code_gen/`](../orly/code_gen) |
| the storage engine | [`orly/indy/`](../orly/indy) |
| the language by example | [`examples/`](../examples) and [`tests/lang_tests/`](../tests/lang_tests) |
