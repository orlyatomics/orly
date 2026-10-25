# Profile-guided release builds

`make release-pgo` builds `orlyi` and `orlyc` twice: once instrumented, then again guided by a profile of the
server doing real work. The result lands in `../out_orly/release`, where `make release` puts its output.

```
make release-pgo                              # portable x86-64 build
make release-pgo PGO_FLAGS=--march=x86-64-v3  # also target Haswell and later (AVX2, BMI2)
docker build --build-arg PGO=1 --build-arg PGO_MARCH=x86-64-v3 .
```

`tools/pgo/pgo_build.sh` is what runs; `--help` lists its options (`--profile-dir`, `--jobs`, `--skip-train`).

## What it does

1. Builds `orlyi` and `orlyc` with `-fprofile-generate` (non-atomic counters, `-fprofile-update=single`).
2. Runs the training workload against that build and collects the `.gcda` files.
3. Rebuilds with `-fprofile-use -fprofile-correction -fprofile-partial-training`.

Only `release.jhm` is modified while it runs, and it is restored on exit. `CCACHE_DISABLE=1` is set, because a cache
would hand back objects built with another profile (or none). Expect the whole thing to take several times as long as
`make release`; the training smokes add about ten minutes.

## The training workload

The training set is the repository's own smokes in `clients/smoke`, run against the mem-sim `orlyi` each one starts:

| smoke | what it exercises |
| --- | --- |
| `ws-scale` | point and prefix reads at 1, 4 and 16 sessions |
| `read-fold` | reads that fold across layers while keys change |
| `layer-count` | reads at many disk layers under ingest |
| `read-fanout` | many concurrent readers |
| `keyset-paging` | paged reads (`skip`, `limit`) |
| `pool-pressure` | write-pool pressure and merging |

These cover the hot paths of the read side (cursor stepping and merging, key and value decode, the fast-runner
dispatch) and the write and merge paths. They run against the in-memory simulator, so the disk controller and the
real-disk merge code get little or no training; `-fprofile-partial-training` optimizes untrained code as a normal build does,
not for size.

## Regenerating the profile

Regenerate whenever the code under `orly/indy`, `orly/server`, `orly/rt` or `orly/sabot` changes materially, and
whenever the toolchain changes. A stale profile is harmless (`-Wno-coverage-mismatch` makes the compiler ignore the
functions that no longer match) but it stops helping for the functions that changed.

- To retrain, just run `make release-pgo` again. Training always runs unless `--skip-train` is given.
- To reuse a profile for a rebuild of the same sources (for example to change `--march`), pass `--skip-train
  --profile-dir <dir>`.
- The profile is keyed by the object files' paths, so it only matches a rebuild in the same checkout directory
  and the same build config. It is not a checked-in artifact and it is not portable between machines' checkouts.
- To change the workload, edit the `smoke` lines in `tools/pgo/pgo_build.sh`. Keep the run to a few minutes per
  smoke; a longer run does not make the profile better, only the weights larger. The script fails if fewer than 50
  profile files come out, which catches a training run that never started the server.

## `--march`

`--march=x86-64-v3` compiles for Haswell (2013) and later CPUs. The binary crashes with an illegal instruction on
older ones, so it is off by default and the published images do not use it.

## Requirements

What `make release` needs, plus Node.js and npm (the smokes' client).

## Result

The profile this script trains on is made from the repository's own smokes, and it does **not** by itself
make the server keep ingest healthy under the read-latency scorecard described in [tuning.md](tuning.md).

Measured on the 28-core, 56-thread machine in tuning.md, two paired runs each (an ABBA order, the tuned flag set
on both arms), against a plain release build of the previous main as the reference:

| build under test | paged-read p99 | point-read p99 | longest write wait | arm outcome |
| --- | --- | --- | --- | --- |
| plain release build, tuned flags | 8.8, 9.1 ms | 1.6 ms | 8.9 s, 11.1 s | ingest limits missed, 27 and 30 writes refused |
| `make release-pgo` (smoke-trained), with the paged-read skip change | 8.5, 8.4 ms | 1.4 to 1.5 ms | 11.9 s, 8.5 s | ingest limits missed (4 and 8 writes over 1 s) |
| `make release-pgo` (smoke-trained), without the skip change | 8.6, 9.1 ms | 1.3 to 1.6 ms | 6.8 s, 10.8 s | ingest limits missed (10 and 4 writes over 1 s) |
| reference arm, default thread layout | 10.7 to 11.3 ms | 1.5 to 1.6 ms | 0.8 to 1.1 s | ingest limits met (one run had a single write just over 1 s), page limit missed |

What this shows:

- The read-latency gain comes from the pinning and pool settings in tuning.md. It is there on every build above.
- A smoke-trained profile and the paged-read skip change, together or separately, did **not** bring the write
  waits back inside 1 s with those settings. The skip change makes no measurable difference to that ingest result
  here, and neither does the smoke-trained profile.
- An earlier build, using a profile trained on a heavier, scorecard-shaped workload (and a larger version of the
  skip change), did pass both limits on two paired runs. What differs between that build and this one is not
  isolated: the training workload is the leading suspect, but that has not been shown.

So treat `make release-pgo` as a reproducible way to build with profile guidance, not as proof that it meets the
scorecard. To train on your own workload, replace the `smoke` lines in `tools/pgo/pgo_build.sh` with a run of
that workload and repeat the comparison above before relying on it. An ingest that fails with the tuned flags on a
plain build is the case to check first.
