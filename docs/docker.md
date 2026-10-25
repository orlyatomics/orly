# Running the Docker image

The engine itself is Linux-only, so this is also the macOS/Windows
path. The published image is multi-arch (`linux/amd64` and `linux/arm64`), so
Apple Silicon pulls a native image rather than running under emulation. A cold
`docker run ... repl` answers in ~2s:

```sh
docker run --rm -p 127.0.0.1:8082:8082 ghcr.io/orlyatomics/orly
```

That's a solo mem-sim server with the `sample`, `graph`, and `market` example
packages pre-compiled and installable, speaking the
[WebSocket + JSON protocol](PROTOCOL.md) on `ws://127.0.0.1:8082/` —
point any client driver ([`clients/`](../clients/)) or the
[MCP server](../clients/mcp/) at it. `orlyc` and the source headers are in the
image, so it compiles your own `.orly` packages too
(`docker exec <ctr> orlyc -o /var/lib/orly/packages yourpkg.orly`). Extra
`docker run ... <flags>` pass straight through to `orlyi`
([#530](https://github.com/orlyatomics/orly/issues/530)).

`-p 127.0.0.1:8082:8082` publishes the port on this machine's loopback only.
Without a shared secret Orly has no authentication, so `-p 8082:8082`, which
publishes it on every interface of the host, lets anyone who can reach the host
read and write the database; do that only on a network you trust, or set a
token. See [`SECURITY.md`](../SECURITY.md).

The `repl` mode shown at the top of the [README](../README.md) is the same image
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

A disk merge folds its merged file's `+=` runs in memory, which takes about
three times the file's size, so a merged file larger than a sixteenth of the
budget is kept unfolded (`--max_fold_input_mb`, 0 for no limit), unless a
key in it has a `+=` run of more than 64 entries. With that
cap, a heavy read load at a 6 GiB budget peaked at 5.7 to 5.9 GiB RSS
([#988](https://github.com/orlyatomics/orly/issues/988)). A merge of any size
leaves runs of up to 64 such entries unfolded (`--min_fold_run`, 0 to fold
every one): the fold rewrites every key in the file, so folding for a few short
runs kept merges from keeping up with ingest
([#1037](https://github.com/orlyatomics/orly/issues/1037)).

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
