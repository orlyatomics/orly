# Tuning orlyi for read latency under ingest

This page is the configuration that kept read latency inside a fixed set of limits while the server ingested writes
continuously, and how to check your own machine against the same kind of scorecard. Nothing here changes
correctness; every item is a flag or a placement choice. The defaults are the right starting point on a small
machine. The settings below matter on a large one (dozens of cores) that serves reads and absorbs a sustained write
load at once.

## The scorecard

The scorecard is a closed-loop ingest running while a set of read sessions runs against the same server. The
numbers below are for one such scorecard; use your own query mix and limits, and keep the shape.

- **Load.** 64 sessions. Writes run closed-loop at whatever rate the server accepts. Reads run alongside, covering
  point lookups, expansions, integer range scans, text scans, key-range reads and paged reads (`skip` and `limit`),
  and a separate open-loop mixed load at 5,000 operations per second.
- **Limits.** Paged-read p99 at most 10 ms. Point-read p50 at most 250 us and p99 at most 2 ms. No write waits
  longer than 1 s. No refusals (no `read_too_large`, no write refused for a full pool). A restart reopens the
  database in at most 2 s.
- **Box.** An x86-64 machine with 28 physical cores and 56 hardware threads (two per core; thread *i* and thread
  *i*+28 share a core), NVMe storage, and the database on a loop device with `--do_fsync`.

The runnable harness for this scorecard is not part of this repository. The release benchmarks
([benchmarks.md](benchmarks.md)) are the in-repo suite. To reproduce the scorecard you need a load generator that
produces the shapes above; the settings in this page are what the server side needs.

## The configuration that passed

```
orlyi ... \
  --slow_cores=0 --slow_cores=8 \
  --mem_merge_cores=2 --mem_merge_cores=10 \
  --disk_controller_cores=4 --disk_controller_cores=12 \
  --disk_merge_cores=0 --disk_merge_cores=1 --disk_merge_cores=8 --disk_merge_cores=9 \
  --fast_cores=3 --fast_cores=5 --fast_cores=6 --fast_cores=7 --fast_cores=11 \
  --fast_cores=13 --fast_cores=14 --fast_cores=15 --fast_cores=16 --fast_cores=17 \
  --fast_cores=18 --fast_cores=19 --fast_cores=20 --fast_cores=21 \
  --num_ws_threads=8 \
  --update_pool_size=150000 --update_entry_pool_size=3000000 \
  --mem_interval=15 --read_yield_us=1000
```

The process is also held to 44 of the machine's 56 threads (`taskset -c 0-21,28-49`), and its eight websocket
threads are pinned to threads 28-30, 32 and 36-38, 40 (see below). On two paired runs this gave a paged-read p99
of about 9.0 ms (the same build on the default layout was 10.7 to 11.1 ms), point-read p50 of about 228 us and
p99 of about 1.6 ms, no write wait over 1 s, and no refusals. Those runs used a build with the paged-read skip
change and a profile built from a scorecard-shaped training workload. The same settings on a plain release build
brought paged-read p99 to 8.8 to 9.1 ms but failed the ingest limits on both paired runs: the longest write wait
was 9 to 11 s (limit 1 s) and 27 to 30 writes were refused. A build made with `make release-pgo` (profile from the
repository's own smokes), with or without the paged-read skip change, behaved the same way: reads inside the limits,
ingest outside them ([pgo.md](pgo.md) has the table). So the pinning alone moves the read numbers, but whether
ingest stays healthy depends on something else about the build that this page cannot yet name. Check ingest on
your own build before adopting the settings.

What each part does:

- **Fourteen fast cores, each on its own physical core.** Fast cores run the read statements. Their hyper-thread
  siblings (here 31, 33-35, 39 and 41-49) are left with nothing on them. See the next section.
- **Disk merges on cores of their own** (`--disk_merge_cores`, here 0, 1, 8, 9) rather than the cores the memory
  merges and controllers use. A long disk merge on a core that also runs a memory merge delays the root flush that
  frees update-pool entries.
- **Larger update pools** (`--update_pool_size`, `--update_entry_pool_size`). Under sustained ingest a small pool
  runs out between memory merges and writes are refused. These are the sizes the passing runs used. They cost memory in proportion to their size, so keep `--memory_budget_mb` in step.
- **`--mem_interval=15`.** The minimum milliseconds between memory merges of a repo. A short interval keeps the
  number of memory layers a read must walk low.
- **`--read_yield_us=1000`.** The passing runs used a 1000 us read time slice, which was the default then. The
  default is now 50 us, which helps point reads that share a runner with long scans, but on this workload it raised
  paged-read p99 by about 4% (8.5 to 8.9 ms on paired runs), so keep 1000 us to reproduce these numbers.

## Hyper-thread siblings

On a machine with two hardware threads per core, a thread running on one sibling shares the core's execution
resources with whatever runs on the other. For read latency the finding was consistent: **the siblings of the fast
cores should be idle**, not merely free of other fast runners. On this box:

| fast cores | siblings | page p99 |
| --- | --- | --- |
| 12, siblings idle | idle | 9.4 to 9.6 ms (but point p99 2.6 ms: too few cores) |
| 14, siblings idle | idle | about 9.0 ms |
| 14, siblings idle, plus four more fast runners placed on the siblings (16 fast threads) | busy | about 11.3 ms (the gain is gone) |
| 14, two of the cores with their siblings used by websocket threads | partly busy | 10.1 to 10.2 ms |

So the rule of thumb is to give the fast runners physical cores and put the other pinned threads (websocket
threads, merges, controllers) on the other physical cores' threads. Count in cores, not threads: more fast threads
sharing cores did not help, and twelve cores was too few for the point reads, so fourteen is where this box landed.

Finding the siblings: the kernel lists them in
`/sys/devices/system/cpu/cpu<N>/topology/thread_siblings_list` (for example `3,31`). orlyi reads these at startup,
and logs a note (syslog, info) naming each fast core whose sibling carries another pinned thread, as `fast/other`
pairs. The layout is never changed for you.

orlyi does not pin its websocket threads. Run it under `taskset -c <cpus>` to keep the whole process off the cores
you reserved, and pin the websocket threads (`taskset -p -c <cpus> <tid>`, thread ids from `/proc/<pid>/task`)
onto siblings of cores that run slower work.

## Core flags and the defaults

A core flag replaces the default for its pool (`--fast_cores`, `--slow_cores`, `--mem_merge_cores`,
`--disk_merge_cores`, `--disk_controller_cores`; each is repeatable). The pools you do not give are placed from the
default table around the CPUs the flags you gave use. Give all five when you want exact control, as above.

## Build

A profile-guided build is a separate, additive step: see [pgo.md](pgo.md), which also says what it did and did not fix. Compiling for the machine's CPU
(`--march=x86-64-v3` on the PGO build) is also optional, and makes the binary Haswell-and-later only.

## Checking a run

For each of your runs, keep the per-shape latency percentiles, the number of refused writes and reads, the longest
write wait, and the time to reopen. A run that meets the limits with the siblings idle and misses them with the
siblings busy is the same effect as in the table above; a run that misses both ways is a different problem
(an ingest the pools or merges can't keep up with, or reads walking too many layers).
