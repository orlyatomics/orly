# Release notes: next release after v0.2.0 (DRAFT)

Draft. Not tagged. The complete list is `CHANGELOG.md` once `tools/fold_changelog.py` has folded
`changelog.d/` into `[Unreleased]` at release time; this page is the summary that goes with the tag. Delete this
file when the release notes are published.

## Highlights

- **Read latency under ingest.** A run of changes to the read path (a point read no longer waits behind a long
  scan, cheaper layer merging and key compares, reads skip disk layers that cannot hold their key, a paged read's
  `skip` builds no key per row) plus guidance on thread placement. With the pinning and the build described in
  [docs/tuning.md](tuning.md), a 64-session mixed read load kept paged-read p99 near 9 ms and point-read p99 under 2 ms
  while ingesting, with no write waiting over 1 s and no refusals, on a 28-core machine.
- **Profile-guided builds.** `make release-pgo` builds `orlyi` and `orlyc` guided by a profile of the repository's
  own smokes ([docs/pgo.md](pgo.md)). Optional; `make release` is unchanged.
- **Reads at a version, bounded history retention, exactly-once writes, write receipts and durable acknowledgments**,
  an online integrity scrub, a POV review workflow, an Orlyscript language server and a browser playground, a Zig
  client, and keyset paging. See the changelog for each.
- **Graceful stop** under write load no longer hangs or loses acknowledged writes, and a clean stop copies live
  POVs' records forward so the next start does not replay the session.

## Breaking changes and things to do when upgrading

- **Replication flags renamed (master/slave to primary/replica)** (#1034, PR #1051 at the time of writing; confirm
  against the merged diff). Final names:

  | old | new |
  | --- | --- |
  | `--address_of_master` | `--address_of_primary` |
  | `--slave_port_number`, `-spn` | `--replica_port_number` |
  | `--slave_bind_address` | `--replica_bind_address` |
  | `--starting_state=SLAVE` | `--starting_state=REPLICA` |

  As of the PR's current head the old spellings still work for this release and print a deprecation line on stderr;
  the replication log text changed (anything that greps `refused a slave from` or `waiting for slave to connect`
  needs the new wording). Wire protocol and on-disk formats are unchanged. If the PR is revised to drop the aliases,
  this entry becomes a hard break and the table is the migration.
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
- The pinning guidance was measured on one machine shape (28 cores, two threads each). The same settings on a plain
  release build, without the paged-read skip and a profile-guided build, passed the paged-read limit but failed the
  ingest limits on both paired runs. Whether the profile built from the repository's own smokes recovers the same gain
  as the profile used for the headline numbers is stated in docs/pgo.md.

## Before tagging (checklist for the releaser)

- [ ] The perf PRs are merged and CI is green on main (confirm the run conclusion, not just the checks list).
- [ ] A release build made with `make release-pgo` passes the scorecard with the documented settings (see docs/pgo.md for what was run).
- [ ] The #1034 rename has merged in its final form; update the table above.
- [ ] `tools/fold_changelog.py` run, version bumped, the release-benchmarks workflow dispatched on the tag.
- [ ] This file deleted or moved into the release.
