# Profile-guided release builds

`make release-pgo` builds `orlyi` and `orlyc` twice: once instrumented, then again guided by a profile of the
server doing real work. The result lands in `../out_orly/release`, where `make release` puts its output.

```
make release-pgo                              # portable x86-64 build
make release-pgo PGO_FLAGS=--march=x86-64-v3  # also target Haswell and later (AVX2, BMI2)
docker build --build-arg PGO=1 --build-arg PGO_MARCH=x86-64-v3 .
```

`tools/pgo/pgo_build.sh` is what runs; `--help` lists its options (`--profile-dir`, `--jobs`, `--skip-train`,
`--no-disk-train`).

## What it does

1. Builds `orlyi` and `orlyc` with `-fprofile-generate` (non-atomic counters, `-fprofile-update=single`).
2. Runs the training workload against that build and collects the `.gcda` files.
3. Rebuilds with `-fprofile-use -fprofile-correction -fprofile-partial-training`.

Only `release.jhm` is modified while it runs, and it is restored on exit. `CCACHE_DISABLE=1` is set, because a cache
would hand back objects built with another profile (or none). Expect the whole thing to take several times as long as
`make release`; the training adds about fifteen minutes.

## The training workload

The training has two parts. First the repository's own smokes in `clients/smoke`, each against the mem-sim `orlyi`
it starts:

| smoke | what it exercises |
| --- | --- |
| `ws-scale` | point and prefix reads at 1, 4 and 16 sessions |
| `read-fold` | reads that fold across layers while keys change |
| `layer-count` | reads at many disk layers under ingest |
| `read-fanout` | many concurrent readers |
| `keyset-paging` | paged reads (`skip`, `limit`) |
| `pool-pressure` | write-pool pressure and merging |

These cover the hot paths of the read side (cursor stepping and merging, key and value decode, the fast-runner
dispatch). They never touch the disk controller, the write-ahead log or the code that merges files on a real
volume, because the simulator stands in for all of it.

Then `tools/pgo/train_disk.sh` runs the same instrumented `orlyi` on a real volume: a sparse 16 GiB file attached
as a loop device, with an orly volume created on it by `orly_dm`. It runs `layer_count.mjs` (16 writers committing
32-key transactions, plus a reader) for 150 s, then `pool_pressure.mjs`, stops the server with SIGTERM, reopens the
volume, writes for another 20 s and stops it again. That trains ingest to disk, the disk merges, the WAL and its
retention, the reopen path and a clean stop. The data is the sample package's synthetic integer keys and values.

The disk part is not optional in effect. Without it the merge path is compiled as code that never runs, and under a
heavy write load the merges then fall behind ingest: on the scorecard in [tuning.md](tuning.md) a smoke-only profile
let write waits reach 5 to 12 s, where the same source with a profile that included disk ingest kept every write
under 1 s (see Result below). `--no-disk-train` exists for environments that cannot attach a loop device, such as a
`docker build`, which uses it.

## Regenerating the profile

Regenerate whenever the code under `orly/indy`, `orly/server`, `orly/rt` or `orly/sabot` changes materially, and
whenever the toolchain changes. A stale profile is harmless (`-Wno-coverage-mismatch` makes the compiler ignore the
functions that no longer match) but it stops helping for the functions that changed.

- To retrain, just run `make release-pgo` again. Training always runs unless `--skip-train` is given.
- To reuse a profile for a rebuild of the same sources (for example to change `--march`), pass `--skip-train
  --profile-dir <dir>`.
- The profile is keyed by the object files' paths, so it only matches a rebuild in the same checkout directory
  and the same build config. It is not a checked-in artifact and it is not portable between machines' checkouts.
- To change the workload, edit the `smoke` lines in `tools/pgo/pgo_build.sh` or the runs in
  `tools/pgo/train_disk.sh`. Keep each to a few minutes; a longer run does not make the profile better, only the
  weights larger. Whatever replaces them must still write enough to a real volume to keep the disk merges busy. The script fails if fewer than 50
  profile files come out, which catches a training run that never started the server.

## `--march`

`--march=x86-64-v3` compiles for Haswell (2013) and later CPUs. The binary crashes with an illegal instruction on
older ones, so it is off by default and the published images do not use it.

## Requirements

What `make release` needs, plus Node.js and npm (the smokes' client), Linux, and passwordless `sudo` for the disk
training: it runs `losetup` to attach and detach the loop device and `setcap cap_sys_admin,cap_sys_nice=ep` on the
instrumented `orlyi`, which cannot open a real volume as a normal user without them. The final `orlyi` is rebuilt
after training and carries no capabilities. The sparse image lives in a temporary directory (`WORK` overrides it)
and needs a few GiB of free space while it runs. Without passwordless `sudo` the build stops before it starts
unless `--no-disk-train` is given.

## Result

Measured on the 28-core, 56-thread machine in [tuning.md](tuning.md), with the tuned flag set from that page on
both arms, each run an ABBA pair (two arms of each build, interleaved). The reference arm is a `make release-pgo`
build whose profile came from the smokes alone, which is what this script made before the disk training was added.

| build under test | runs | longest write wait | writes over 1 s | refused | paged-read p99 | point-read p99 |
| --- | --- | --- | --- | --- | --- | --- |
| smoke-trained profile (reference) | 4 arms | 5.9 to 11.5 s | 4 to 9 | 26 to 41 | 8.5 to 9.4 ms | 1.3 to 2.0 ms |
| disk-trained profile, this script | 4 arms | 0.67 to 0.83 s | 0 | 0 | 10.1 to 10.7 ms | 1.9 to 2.6 ms |
| an earlier main, unchanged, with a profile from one scorecard-shaped run on a real volume | 2 arms | 0.79, 0.88 s | 0 | 0 | 9.0, 9.4 ms | 1.9, 2.1 ms |

What this shows:

- **The training workload decides whether ingest keeps up.** The last row is unchanged source code with a different
  profile, and it meets the ingest limits where the
  reference misses them by a factor of five to ten. With a smoke-only profile the real-disk merge, fold, WAL and file-service code is barely run during
  training (a handful of counted calls against several hundred), so the compiler lays it out as cold code, and
  under a write-heavy load the disk merges fall behind the flushes. The disk training brings those counts close to
  the scorecard-shaped profile's.
- The disk-trained build does a little more work per run than the reference (about 370 writes per arm against
  about 300, because none stall), so the reads it serves at the same time share the machine with more merging.
  Its paged-read and point-read p99 under ingest were slightly above the 10 ms and 2 ms limits in these runs;
  with no ingest running its point-read p99 was about 0.3 ms.

So build with the disk training whenever the server will take a sustained write load. `--no-disk-train` gives a
profile that is fine for read-mostly use and for the smokes, but not for ingest. To train on your own workload,
change the runs in `tools/pgo/train_disk.sh` and repeat the comparison above before relying on it.
