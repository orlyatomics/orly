# Building from source

The engine is Linux-only. On macOS, [`./dev`](../CONTRIBUTING.md#getting-started-on-macos-or-any-non-linux-machine) builds and runs it in a container; the commands below are for a Linux host.

## Dependencies and build

System dependencies (Ubuntu 24.04):

```sh
sudo apt-get install -y \
  build-essential gcc g++ \
  uuid-dev libgmp-dev libaio-dev libsnappy-dev libgoogle-perftools-dev libabsl-dev \
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

[Release benchmarks](benchmarks.md) document the tag/on-demand suite,
versioned results, A/A noise and same-runner comparisons. The page states the
smoke-scale fixtures and remaining keyset-paging/fixed-hardware coverage.

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


## ThreadSanitizer build

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
documented and suppressed in [`orly/tsan.supp`](../orly/tsan.supp). See
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) and issues
[#177](https://github.com/orlyatomics/orly/issues/177) /
[#184](https://github.com/orlyatomics/orly/issues/184).

The same job also builds `orlyi` and `orlyc` under TSan and runs the
memory-drain smoke against that server
([`clients/smoke/run-tsan-server.sh`](../clients/smoke/run-tsan-server.sh), #713),
so WebSocket sessions, Tetris, the merges and the reporting port are covered
too. It fails on any un-suppressed report from `orlyi`, after a negative
control that makes `orlyi` race on purpose and requires TSan to report it.

## UndefinedBehaviorSanitizer build

A `ubsan` config builds with UndefinedBehaviorSanitizer
(`-fsanitize=undefined,float-cast-overflow`) to find signed overflow,
misaligned or null accesses, bad shifts, out-of-range enum/bool loads and the
like. Like the `asan` config it builds with `-U_FORTIFY_SOURCE`: the fibers
switch stacks with `longjmp`, which glibc's fortified `longjmp` rejects. It is
not part of CI yet. Reports are recoverable (the program keeps
running), so collect them from the log:

```sh
tools/jhm -c ubsan --test      # builds and runs every test under UBSan
```

`jhm --test` prints the output of a *failing* test only, and UBSan reports do
not fail a test, so to see them run the binaries yourself and send the reports
to files:

```sh
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0:report_error_type=1:log_path=/tmp/ubsan/r
mkdir -p /tmp/ubsan
../out_orly/ubsan/orly/indy/context_fold.test
grep -h 'runtime error:' /tmp/ubsan/r.* | sort | uniq -c | sort -rn
```

Orlyscript tests go through a UBSan `orlyc`. Keep `log_path` set here:
`lang_test.py` does not drain a child's output pipe until the child exits, so
the many megabytes of stack traces a plain run prints will hang it.

```sh
tools/jhm -c ubsan orly/orlyc orly/server/orlyi
ORLYC="$PWD/../out_orly/ubsan/orly/orlyc" python3 tools/lang_test.py -d tests/lang_tests
```

On a machine with very large RAM, cap a standalone `orlyi` (cgroup limit) so
it does not size its budget from free memory.

## Supported platforms

Linux. Verified on Ubuntu 24.04.

**macOS development**: on macOS (Apple silicon and Intel), the Linux engine builds and runs in a container through [`./dev`](../CONTRIBUTING.md#getting-started-on-macos-or-any-non-linux-machine) (`./dev build|test|lang|run`), with warm ccache and build volumes yielding ~0.4 s incremental rebuilds; native macOS clients and REPL connect over loopback WebSocket ([#738](https://github.com/orlyatomics/orly/issues/738)).

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

Build flags live in [`root.jhm`](../root.jhm); per-target overrides in `debug.jhm` / `release.jhm` / `bootstrap.jhm`.

## IDE / clangd

Every successful `make debug` (or any `jhm` invocation that compiles C/C++) writes a [JSON Compilation Database](https://clang.llvm.org/docs/JSONCompilationDatabase.html) to `compile_commands.json` at the repo root. clangd, clang-tidy, IWYU, and most C++-aware editors pick it up automatically. The file is regenerated on every build, so it's `.gitignore`d.
