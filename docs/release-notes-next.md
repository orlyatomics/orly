# Release notes: next release after v0.2.0 (DRAFT)

Draft. Not tagged. The complete list is `CHANGELOG.md` once `tools/fold_changelog.py` has folded
`changelog.d/` into `[Unreleased]` at release time; this page is the summary that goes with the tag. Delete this
file when the release notes are published.

## Highlights

- **Read latency under ingest.** A run of changes to the read path (a point read no longer waits behind a long
  scan, cheaper layer merging and key compares, reads skip disk layers that cannot hold their key, a paged read's
  `skip` builds no key per row) plus guidance on thread placement. With the pinning described in
  [docs/tuning.md](tuning.md), a 64-session mixed read load kept paged-read p99 at 8.4 to 9.1 ms (10.7 to 11.3 ms on
  the default layout) and point-read p99 under 2 ms on a 28-core machine. Whether writes stay inside 1 s while that
  load runs depends on the build; see Known limitations.
- **Profile-guided builds.** `make release-pgo` builds `orlyi` and `orlyc` guided by a profile of the repository's
  own smokes ([docs/pgo.md](pgo.md)). Optional; `make release` is unchanged. It is a reproducible build step, not a
  demonstrated fix for the ingest result below (see Known limitations).
- **Reads at a version, bounded history retention, exactly-once writes, write receipts and durable acknowledgments**,
  an online integrity scrub, a POV review workflow, an Orlyscript language server and a browser playground, a Zig
  client, and keyset paging. See the changelog for each.
- **Graceful stop** under write load no longer hangs or loses acknowledged writes, and a clean stop copies live
  POVs' records forward so the next start does not replay the session.

## Breaking changes and things to do when upgrading

- **Replication flags renamed (master/slave to primary/replica), with no aliases** (#1034, #1051). A script or unit
  file that still passes an old name fails at startup with an unknown-flag error:

  | old | new |
  | --- | --- |
  | `--address_of_master` | `--address_of_primary` |
  | `--slave_port_number`, `-spn` | `--replica_port_number` |
  | `--slave_bind_address` | `--replica_bind_address` |
  | `--starting_state=SLAVE` | `--starting_state=REPLICA` |

  Replication log lines say primary and replica, so anything that greps the old wording needs the new. The wire
  protocol and on-disk formats are unchanged.
- **Default branch renamed from `master` to `main`.** Existing clones update with:
  `git branch -m master main && git fetch origin && git branch -u origin/main main && git remote set-head origin -a`.
  GitHub redirects the old web links; open pull requests were retargeted.
- **On-disk format.** Starting with a retention flag upgrades a version 1 store to version 2, and a store that has
  copied a paused POV is marked version 3; older binaries refuse both and `orly_dm` does not downgrade them. Take a
  backup before the first start of the new binary on a store you may need to roll back.
- **Build and runtime dependencies.** Abseil (`libabsl-dev`) and TCMalloc (`libgoogle-perftools-dev`) are now needed
  to build, and their runtime libraries are in the image.
- **Default thread layout.** Memory merges get cores of their own by default, 12 to 15 CPU machines get a table of
  their own, and the layout follows the CPUs the process may run on (`taskset`, cpusets). A core flag given for some
  pools no longer lets the defaults for the other pools land on the CPUs it claimed.
- **Write-pool behavior.** Memory admission waits a bounded time for room before refusing a write, and a full disk
  or full WAL ring refuses writes with `insufficient_storage` rather than stopping the server.
- **New flags**: `--edit_runner`, `--read_yield_us`, `--read_yield_rows`, `--min_fold_run`, `--max_fold_input_mb`,
  `--device` (repeatable), `--disk_reserve_pct` and `--disk_reserve_mb`. Defaults keep the old behavior except where
  the changelog says otherwise.

## Performance

See the changelog entries marked performance. The ones most visible on the scorecard: #1015/#1016 (small reads get
in while a long read runs), #1017 to #1024 (runner handoff, page cache pins, WAL checksum and record build),
#1029 and #1042 (cheaper range-merge compares), #1036 (skip disk layers a key cannot be in), #1040 (`--edit_runner`),
#1044 and #1047 (walker setup, prefetch pinning).

## Known limitations

- The M1 scorecard's runnable harness is not in the repository, so the headline numbers above are reproducible with
  the settings in [docs/tuning.md](tuning.md) and a load generator of your own, not with a script from this tree.
- The pinning guidance was measured on one machine shape (28 cores, two threads each).
- **Ingest under the tuned settings is not reproduced by the build in this tree.** With the tuned pinning and pool
  flags, a plain release build, `make release-pgo` with the paged-read skip change, and `make release-pgo` without it
  all met the read limits and all missed the ingest limits (longest write wait 6.8 to 11.9 s against a 1 s limit)
  on every paired run. A build trained on a heavier, scorecard-shaped workload passed on two paired runs; what
  separates it from the smoke-trained build is not isolated. docs/pgo.md has the table. Do not state in the
  release that the settings keep writes within 1 s unless the checklist item below has been done.

## Before tagging (checklist for the releaser)

- [ ] The perf PRs are merged and CI is green on main (confirm the run conclusion, not just the checks list).
- [ ] A build from this tree passes the whole scorecard, ingest limits included, with the documented settings. As of
  the last measurement none did (see Known limitations). Either find what the passing build had that this tree does
  not, or ship with the limitation stated.
- [x] The #1034 rename has merged (#1051, #1052) with no aliases; the table above matches.
- [ ] `tools/fold_changelog.py` run, version bumped, the release-benchmarks workflow dispatched on the tag.
- [ ] This file deleted or moved into the release.
