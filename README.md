<p align="center">
  <img src="docs/branding/orly.png?raw=true" height="100" alt="Orly" />
</p>

> **Status — 2026 revival.** Orly was dormant from 2019 until early 2026, when a modernization pass brought the codebase back to building and testing cleanly on a current Linux toolchain. `make debug`, `make test`, `make release`, and the Orlyscript `lang_test.py` harness all pass on Ubuntu 24.04 + gcc 13. See [#10](https://github.com/orlyatomics/orly/issues/10) for the full status and remaining gaps. No active feature development.

This is the repository for the Orly non-relational database. It's meant to be fast and to scale for billions of users. Orly provides a single path to data and will eliminate our need for memcache due to its speed and high concurrency.

## Orly features

* **Points of View**: This is our version of optimistic locking or isolation. In traditional databases, clients have to lock the entire database (or at least large swaths of it) before updating it to ensure data remains consistent. In Orly, clients make changes in their own private points of view, which are like small sandboxes. Changes in private points of view eventually propagate into shared points of view and eventually reach the global point of view, which is the whole database. Updates to private points of view don't lock anything: Orly determines later whether, when, and how to reconcile changes from different points of view. We also encourage field calls rather than field changes (e.g., `x += 1` is better than `x = x + 1`).
* **Causal ordering (the _Flux Capacitor_)**: Orly's merge-conflict resolution defines its "time line" by causality rather than clock time. Instead of manipulating timestamps, it records an ordering of events (e.g., update A affects update B, so A "happens before" B), and uses that ordering to decide how concurrent writes from different POVs reconcile into global state. Note: the original 2014 README pitched this as user-facing "time travel" (consistent read at any point in time), but the implementation never exposed past-state queries — POV writes propagate to global rather than freezing snapshots. Time travel as a user-facing feature is achievable, but it lives in user-space; see [`examples/bitcoin-time-travel/`](examples/bitcoin-time-travel/) for a worked pattern (encode the version axis into the key tuple, fold on read) that gives you historical queries and branched-history multiverse queries without engine changes.
* **Query Language**: Orly has its own high-level, compiled, type-safe, functional language called _Orlyscript_. Orlyscript is not just a query language: You can write general-purpose programs in it complete with compile-time unit tests. Orly comes with a compiler that transforms Orlyscript into shared libraries (.so files on Linux), which Orly servers load as packages.
* **Scalability and Availability**: While we eventually plan to develop a sharded Orly machine (and actively design so that we can build such a machine), our current single-node server with fail-over/replication can handle hundreds of thousands of transactions per second. We like to say that Orly will function on a "planetary scale": Your data and computations will not only distribute across a data center, but also across many data centers across the globe. This means that no disaster short of nuking the planet fifty times over or colliding with a gigantic asteroid will destroy your data. (Even those might not be catastrophic: Maybe we'll have data centers running Orly with your replicated data on the moon or Mars.)

## Supported platforms

Linux only, x86-64. Verified on Ubuntu 24.04. Earlier Ubuntu releases probably work too but have not been re-tested in the revival pass.

## Quick start

Install the system dependencies:

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

The four production binaries land at `../out_orly/debug/` (or `../out_orly/release/`):

| Binary | Purpose |
| --- | --- |
| `orly/orlyc` | Orlyscript compiler |
| `orly/server/orlyi` | Database server |
| `orly/spa/spa` | Single-process app server |
| `orly/client/orly_client` | Interactive client shell |

To exercise the Orlyscript test suite against compiled `.orly` programs:

```sh
python3 tools/lang_test.py -d orly/data tests/lang_tests
```

## Toolchain

The revival pass uses:

* gcc 13.3.0
* `-std=c++23`
* boost 1.83 (system, including `boost::beast` for the WebSocket surface)
* Python 3.12 for `lang_test.py`

Build flags live in `root.jhm` (per-target overrides in `debug.jhm` / `release.jhm` / `bootstrap.jhm`).

## Walkthrough

[`docs/walkthrough.md`](docs/walkthrough.md) walks through compiling an Orlyscript package with `orlyc`, loading it into a running `orlyi`, and invoking a method on it via `orly_client` — the full pipeline end-to-end.

## Examples

[`examples/bitcoin-time-travel/`](examples/bitcoin-time-travel/) — a Bitcoin-flavoured ledger that does **time-travel queries** ("balance at block 5") and **branched-history multiverse queries** ("balance on the fork vs the mainnet at block 7"). Uses one small Orlyscript package and a Python driver; smoke-tested in CI on every push.

## Contributing

The build system (`jhm`) lives in `jhm/`; the bootstrap path is documented in `bootstrap.sh`. There's no formal style guide; match the surrounding code.

### IDE / clangd

Every successful `make debug` (or any `jhm` invocation that compiles C/C++) writes a [JSON Compilation Database](https://clang.llvm.org/docs/JSONCompilationDatabase.html) to `compile_commands.json` at the repo root. clangd, clang-tidy, IWYU, and most C++-aware editors pick this up automatically. The file is regenerated on every build, so it's `.gitignore`d.

-----

README.md Copyright 2010-2026 Atomic Kismet Company

README.md is licensed under a Creative Commons Attribution-ShareAlike 4.0 International License.

You should have received a copy of the license along with this work. If not, see <http://creativecommons.org/licenses/by-sa/4.0/>.
