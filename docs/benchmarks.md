# Release benchmarks

The [`release-benchmarks`](../.github/workflows/release-benchmarks.yml) workflow
runs on `vMAJOR.MINOR.PATCH[-PRERELEASE]` tags and on demand. Complete
measurements are committed as `bench/results/<version>.json` and rendered below
by a results PR. Merge that PR to publish the page on `master` and make the
release available as the next comparison baseline. No Pages site or additional
secret is required: publication uses the repository's normal workflow token.
It needs contents/PR write permission and permission for workflows to open PRs.
It never pushes to `master` or merges its own PR.

## What is measured

[`bench/suite.json`](../bench/suite.json) pins commands, inputs, concurrency,
durations, deadlines and headline directions. The suite runs release binaries:

| Workload | Fixture / concurrency | Headline measurements |
| --- | --- | --- |
| Pool-pressure smoke | 20,000 Update / 40,000 Entry pool; 8 writers; shared POV and POV per writer | writes/s, p99 write latency |
| Batch-backlog smoke | 200-write batches; 8 writers; 25% reserve; 4 in-flight statements | batches/s, p50/p99 batch latency |
| Read-fold smoke | 200 counters × 40 updates; 4 readers | point and range-fold reads/s |
| WebSocket scale smoke | 2,000 groups of 6 keys; 1, 4, 16 sessions; up to 4 client workers | point/prefix reads/s and p50/p99 latency |
| GRC-20 | 128 sources per level, 128 shared targets, 4 edges each, 4 relation revisions; 1, 4, 16 sessions | acknowledged relation events/s; verified one-hop traversals/s |
| Disk restart | 10 keys on a 3 GiB loopback volume, fsync enabled | restart launch to all 10 keys successfully served, including package reload |

GRC-20 uses the unchanged `examples/grc20-pov/grc20.orly` schema. A traversal
enumerates a source's four properties, resolves each relation in the engine,
and follows it to the target's name: nine client calls, not a native traversal
operator. Ingest counts single acknowledged relation appends (including
backpressure/retry time); fixture setup is outside that timer. Every traversal
is checked. These are small, closed-loop, potentially client-limited workloads,
not advertised capacity limits.

Restart is timed from launching the existing disk-backed restart fixture to
successful reads after restart, not merely a listening socket. The benchmark
polls startup every 100 ms; normal restart tests keep their existing polling.
This tiny fixture measures recovery overhead, not recovery of a large corpus.

Every smoke's `METRIC` line is retained in JSON, including pool occupancy,
refusals and other diagnostics. Only the explicitly listed throughput and
latency headlines are regression signals; a peak pool counter is not one.
The smokes' own correctness/admission checks must also pass.

## Comparing releases and noise

Each run uses **one native GitHub-hosted `ubuntu-24.04` x86-64 runner**, four
build workers, GCC release builds, and Node 20. CPU, CPU count, RAM, architecture,
kernel and runner-image version are recorded. This pins a runner *class*, not
the same physical hardware between releases. A dedicated, fixed runner still
needs to be chosen before treating this as a fixed-hardware performance series.

For every workload, three A/A rounds run the identical candidate build twice.
When a previous published measurement exists, its exact commit is rebuilt and
three A/B rounds run the previous and candidate builds **on this same runner**,
using the same current harness and fixture. AB/BA order alternates within each
phase. First publication establishes a baseline; no historical numbers are
invented. Merge results PRs before the next release so a baseline is available.

Tables show medians and min/max spread, the median of paired B/A ratios, and
separate A/A noise rows. For a higher-is-better headline, degradation is
`1 - median(B/A)`; for lower-is-better it is `median(B/A) - 1`. The noise floor
is the greater of the largest symmetric A/A excursion
`max(|B/A - 1|, |A/B - 1|)` in this run and in the previous published run.
**Any headline degradation beyond that floor fails the workflow.** This is an
observed noise bound, not a confidence interval or proof of no regression.

Failed commands, deadlines, missing rounds, missing/nonpositive headlines and
non-finite numbers fail loudly; incomplete results stay in artifacts and are
not committed. Complete measurements that show a regression are still offered
as a results PR, with the failing verdict intact. Existing version data and
pending results PRs are never overwritten by a rerun; new shots stay in artifacts.
A changed suite fingerprint or runner blocks the comparison and fails rather
than silently reporting green. The release-image workflow remains independent.

## On demand and dry-run

After the workflow is available on `master`, exercise the whole measurement
path on a branch without publication:

```sh
gh workflow run release-benchmarks.yml --repo orlyatomics/orly \
  --ref <branch>
```

No tag is required for this dry-run. Download the `release-benchmark-output`
artifact for JSON, every per-round log and a rendered `preview/docs/benchmarks.md`.
To measure an existing release, add `-f version=vX.Y.Z`. Add `-f publish=true`
only to open its results PR. New release tags publish automatically; publication
failures (including repository token restrictions) leave the same artifact for
manual commit. Publishing the evidence does not clear a failing benchmark run.

To re-render already committed data without running benchmarks:

```sh
python3 tools/maint/release_bench.py render --history bench/results --page docs/benchmarks.md
```

## Remaining coverage

- Keyset range **pages** await [#735](https://github.com/orlyatomics/orly/issues/735).
  Prefix scans and range folds above are not represented as keyset paging.
- Fixed physical hardware per series needs a dedicated runner configuration.
  Same-runner paired comparisons reduce cross-machine bias but do not provide it.
- Larger, durable graph/ingest and restart datasets can extend the smoke-scale
  fixtures once their size and storage hardware are chosen.

## Published measurements

<!-- benchmark-results:start -->

No published measurements yet.

<!-- benchmark-results:end -->
