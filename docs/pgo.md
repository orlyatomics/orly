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

PGO_RESULT_PLACEHOLDER
