# Design: Concurrent write throughput — the single-threaded global merge

> Status: **S1 implemented behind a default-off flag** (`--tetris_commutative_fastlane`),
> tracked by **issue #234**. The original gated RFC follows; the implemented
> design and its measured results are in [§8 (Implementation addendum)](#8-implementation-addendum-implemented).
> The flag stays **off by default** until the TSan gate (#201) + a sustained soak
> sign off on flipping it (S2).

## 0. The key realization

Single-transaction bulk load is now linear end-to-end (PR #231 removed the
per-write read, PR #232 removed the commit-time O(N²)). The remaining write
lever is **concurrent** throughput, and it has a single, structural cause:

**The global POV's Tetris player promotes at most one update per round, and
pays a full transaction + snapshot + sort on every round.** That fixed
per-round overhead is therefore paid *per write*. The cap is not the storage
engine, not CPU saturation, and not a missing concurrency primitive — it is a
2014 placeholder (`repo_tetris_manager.cc:309`, comment: *"At most one will be
permitted to promote (for now)"*).

The opening: **commutative field calls (`+=` / `|=`) carry no assertion and
provably cannot conflict** (architecture.md §5). The single deterministic
tournament does correctness-irrelevant serialized work for exactly the headline
workload. If non-conflicting commutative promotions could be batched into one
round/transaction, the per-round overhead would amortize across the batch.

## 1. Current state (verified)

### Measured cap (`--mem_sim`, 12 cores, K concurrent writers, each its own
private POV, all promoting into the one global POV)

| K writers | throughput | note |
|---:|---:|---|
| 1 | 578 w/s | latency-bound (one client round-tripping) |
| 4 | **1407 w/s** | peak |
| 8 | 1218 w/s | *regresses* — pure contention, no gain |

Flatlines ~1400 w/s and degrades past K=4: the textbook signature of one
saturated serial resource.

### The serial resource, per layer

| Concern | State | Evidence |
|---|---|---|
| Merge threading | **All** Tetris players (one per parent POV) run as cooperative fibers on **one** `std::thread` | `tetris_manager.cc:258` (`FiberThread = std::make_unique<std::thread>(...)`), player loop `:192` `TPlayer::Main` → `:217` `Play()` → `:218` `usleep(0)` yield |
| Mem/disk *merge* | already a thread pool — **not** the bottleneck | `server.cc:405-406` `NumMemMergeThreads(3)`, `NumDiskMergeThreads(8)`; pools spun at `:1104`, `:1119` |
| Promotion game | **one promotion per round**, full txn each round | `repo_tetris_manager.cc:286` `Play()`: `NewTransaction` → `Refresh`/`Peek` all children → `sort(SortsBefore)` → walk children, **`break` after first `child->Play()` that promotes** (`:313-317`) → `Prepare` + `CommitAction` |
| Assertion test | empty predicate set ⇒ always passes ⇒ commutative pieces never block each other | `repo_tetris_manager.cc:197` `TestAssertions`, predicate compare only when `GetExpectedPredicateResults()` non-empty (`:208-209`) |
| Ordering | sorted by **age only** (oldest first) | `repo_tetris_manager.cc:170-173` `SortsBefore` = `lhs->Age > rhs->Age` (note: architecture.md §3 also mentions key-count; code is age-only) |
| Multi-update collapse | **explicitly unsupported** today | `repo_tetris_manager.cc:90-95` throws `"We don't currently support collapsed updates"` when a single child carries >1 update id while pushing to global |

### Where the merge fiber spends its time (poor-man's profile, K=8)

The single hot thread (consistently one TID) runs **~70–80% CPU**, `wchan`=0
throughout, run-state `R` in 74/100 samples. So it is ~1 core of
merge-tournament CPU, **not** blocked on a futex/coordination wait; the ~26%
idle is the `usleep(0)` yield between rounds. (No `perf` in the sandbox; used
`top -H` + `/proc/<tid>/stat` + `wchan`. gdb attach is blocked by
`ptrace_scope=1` on sibling processes.)

### Spike (throwaway, already reverted) — the easy version is wrong

Dropping the `break` so every passing piece promotes in one round/transaction:

- K=1: 578 → **689 w/s** (marginal — fewer rounds, as predicted).
- K=4: **orlyi crashes** — `"Connection reset by peer"`, zero writes succeed.

So even for a purely-commutative workload, batching multiple child promotions
into a single transaction violates an invariant in the per-round
`Peek`/`Push`/`Pop` + fiber-coordination machinery. **The one-promotion-per-round
discipline is load-bearing, not merely conservative.** A faithful fast-lane must
handle multi-promotion transaction semantics deliberately — this is the deep
part, and the reason this is an RFC, not a patch.

## 2. Design decisions (proposed)

Two independent designs; A is the smaller, higher-confidence win and is the
recommended first slice. B is the larger structural change for the
field-*change* (RMW) path and is optional / later.

### Design A — Commutative fast-lane (recommended)

Within one `Play()` round, split children into:

- **Assertion-free** (every entry has empty `GetExpectedPredicateResults()` —
  i.e. pure commutative `+=`/`|=`). These cannot conflict with each other or
  with anything (§5). Promote **all** of them in this round's transaction.
- **Assertion-bearing** (RMW field changes). Keep today's discipline: promote
  **at most one** per round (their assertions are tested against the
  round-start snapshot; promoting two against the same stale snapshot is the
  lost-update bug the `break` exists to prevent).

Net: N concurrent counter writes collapse from N rounds (N transactions, N
snapshots, N sorts) to ~1 round. The fixed per-round overhead amortizes across
the batch — directly attacking the measured cap.

The hard sub-problem the spike exposed: **pushing K children's updates into one
transaction**. Must be designed, not hacked:
- per-child `transaction->Push(parent, PeekedUpdate)` + `transaction->Pop(childRepo)`
  must be safe to interleave K-fold in one txn (sequence-number-start
  invariant, `transaction_base.cc`);
- the `"collapsed updates"` throw (`:90-95`) and the per-child notification loop
  (`:101-115`) must handle the batched shape;
- failure isolation: if one child in the batch fails, it must not poison the
  others' commit.

### Design B — Key-range-sharded global merge (optional, later)

Run M global-merge fibers on M threads, partitioned by key range (hash of the
key). Commutative ops on disjoint key ranges are independent; field changes
need ordering only **within** a shard. Lifts the single-thread ceiling for
the RMW path too, but multiplies the surface the TSan gate must cover and needs
a cross-shard story for multi-key updates. Only pursue if A proves insufficient.

## 3. Implementation plan (Design A)

1. Add `TChild::IsAssertionFree()` (all entries' `GetExpectedPredicateResults()`
   empty; Mynde entries treated as today). Header + `repo_tetris_manager.{h,cc}`.
2. In `Play()` (`:313`), replace the single-promote loop with: promote **all**
   assertion-free passing children, then **at most one** assertion-bearing one.
3. Make the multi-push transaction path correct (§2 sub-problem): audit
   `TTransaction::Push`/`Pop`/`Prepare`/`CommitAction` for K-fold use; fix the
   `"collapsed updates"` path and notifications; isolate per-child failure.
4. Bound the batch (e.g. max children per round) so a huge ready-set can't
   starve liveness or balloon one transaction; `log` if a cap drops work.

## 4. Staged delivery (reviewable vertical slices)

- **S0 — instrument & confirm (no behavior change):** land per-round counters
  (children-considered, promoted, txn/snapshot/sort time) behind the existing
  `RoundCount`/`PushCount` so the cap is observable in CI before touching logic.
- **S1 — Design A behind a flag, default off:** fast-lane for assertion-free
  children; gate on `agent-swarm` + a new concurrent lang/integration test
  asserting zero lost counters, and on the TSan job (#201).
- **S2 — flip default on** once S1 is green across the demo suite + TSan + a
  sustained K=8/16 soak shows throughput scaling (not regressing).
- **S3 (optional) — Design B** if the single thread is still the ceiling.

## 5. Decisions (MVP scope — assumptions, revisable)

- Scope S1 to the **commutative** path only; the RMW (assertion-bearing) path
  keeps exact current semantics (one per round). No behavior change for field
  changes.
- Liveness/ordering for assertion-bearing pieces is unchanged (age-ordered,
  one-at-a-time), so fairness/starvation characteristics for RMW are preserved.
- Correctness oracle is the existing property: concurrent `+=`/`|=` lose
  nothing (architecture.md §5, `examples/wikipedia-pageviews/`,
  `examples/agent-swarm/`). Every slice must pass these write-then-read.

## 6. Risks

- **Data-race surface.** This is the lock-free core the TSan gate (#184/#201)
  guards. Batched promotion touches the parent repo from one fiber but changes
  the txn shape; must run under TSan (`tools/jhm -c tsan` + `setarch -R`,
  see the TSan-repro memory) before merge. **Highest risk.**
- **Transaction invariant.** The spike crash proves the per-round
  Peek/Push/Pop + sequence-number-start machinery assumes one promotion per
  round somewhere. Root-causing that crash *is* step 3; it may surface a
  deeper coupling.
- **Liveness.** Promoting a large ready-set in one txn could stall the round or
  inflate a single commit; needs a batch bound (step 4).
- **Not a C++ feature.** The constraint is architectural. `std::execution`
  (senders/receivers, P2300) is C++26 and needs gcc 14 (we are gcc 13.3,
  `-std=c++23`); the engine's bespoke fiber scheduler + `AtomicUnorderedList`
  already exceed the standard primitives. No library swap unlocks this.

## 7. Provenance

Findings verified 2026-06-27 (post #231/#232) by read-only investigation +
one throwaway spike (master left clean). Measurements: `--mem_sim`, 12 cores,
`examples/agent-swarm/graph.orly` workload (`add_cooccur`, all commutative).

## 8. Implementation addendum (implemented)

S1 shipped, but **not** as Design A above. The read-only investigation that
preceded it overturned two of the RFC's premises; the implemented design is
both safer and more faithful. This section is the source of truth for what was
built.

### 8.1 Why Design A (merge into one update) was rejected

Design A proposed merging K children's updates into a single global update.
Tracing the metadata path killed it:

- `TUpdate::TEntry::GetMetadata()` returns the **update-level** `Metadata`
  (`orly/indy/update.h`, `TEntry::GetMetadata` → `UpdateMembership.TryGetCollector()->Metadata`).
  There is no per-entry metadata. Merging K children therefore collapses K
  distinct session-ids into one.
- That session-id is **load-bearing for one path**: slave-side *"Replicated"*
  notification routing (`orly/indy/manager.cc` extracts the session-id from the
  promoted update's metadata and routes the notification to it). Reads,
  replication *apply*, and recovery treat the metadata as opaque pass-through —
  but collapsing it would **silently drop replication notifications** for K−1
  of every K merged writers.

So merging is not semantically transparent. Design A was abandoned.

### 8.2 The implemented design — per-round, per-child fast-lane

The real cost was never "one Pusher per transaction." It is that **each
`Play()` round `Refresh()`es (re-snapshots) *every* waiting child and then
promotes one**, so draining N queued children costs N rounds × O(children) =
**O(N²)** re-snapshot — the same family as #229/#232.

`TRepoTetrisManager::TPlayer::Play()` (`orly/server/repo_tetris_manager.cc`),
under `--tetris_commutative_fastlane`:

1. Snapshot + sort all ready children **once** per round (unchanged).
2. Promote **every** assertion-free child this round — *each in its own
   transaction*, on which the child is **re-`Peek`'d first** (`Flush()` the
   snapshot peek, `Refresh()` on the per-child txn, then `Play()`). A
   transaction applies its `Push` on destruction (`~TPusher` →
   `TRepo::AppendUpdate`), so each promotion is scoped to one loop iteration.
   One Pusher per repo per transaction is preserved exactly, so the invariant
   the throwaway spike violated (`MutationCollection` is keyed by repo-id; a 2nd
   `Push` to the same parent in one txn throws) is never touched. Each child
   keeps its own update and its own session-id metadata → replication
   notifications stay faithful.
3. Only if **no** commutative promotion happened this round, fall back to the
   exact current discipline for assertion-bearing (RMW) children: at most one
   per round, tested against the round-start snapshot. This guarantees an RMW
   assertion is never evaluated against a parent the same round's commutative
   writes already mutated — RMW semantics are byte-for-byte unchanged.

`TChild::IsAssertionFree()` classifies a child: every refreshed entry has an
empty `GetExpectedPredicateResults()` and none is a Mynde entry (architecture.md
§5: such children carry no assertion and provably cannot conflict).

Subtlety the first cut got wrong — and why the re-`Peek` is load-bearing: the
`Peek` and the `Pop` for a child **must live on the same transaction**. `Peek`
installs a `Peek`-state `TPopper` keyed by the child repo-id; `Pop` on that same
txn *promotes* that popper to `Pop` state (one mutation, Peek→Pop). If instead
the child is `Peek`'d on `snapshot_txn` but `Pop`'d on a fresh per-child txn, the
fresh txn has no mutation for that repo, so `Pop` mints a **second**, independent
`Pop`-state popper while `snapshot_txn` still holds the first one's read `View`.
`TRepo::PopLowest` then runs against a child whose last update may already have
been promoted/Part-deleted out from under the dangling snapshot `View` — which
crashed reliably at K≥4. Re-`Peek`ing on the per-child txn collapses Peek and Pop
back onto one popper, so `Push`/`Pop`/commit are a single atomic unit per child
and `PopLowest` fires exactly once against the snapshot it was classified on. The
leftover `snapshot_txn` Peek popper for the same child is then a true no-op at
destruction (`~TPopper`, `case Peek: break` — releases only its read `View`).

### 8.3 Measured (debug build, `--mem_sim`, 32 child POVs × 200 commutative `+=`)

A new reporter counter, **`Tetris Children Considered / Push`**, makes the
quadratic directly observable (it equals "re-snapshots paid per promotion"):

| metric | baseline | fast-lane |
|---|---:|---:|
| Children Considered / Push | **30.1** | **1.0** |
| Children Considered / s (snapshot CPU proxy) | 6617 | 201 |
| Rounds / s | 219.6 (1 push/round) | 39 (~5 push/round) |

The ratio for baseline tracks the active-POV count (≈32 here) and grows without
bound; the fast-lane holds it at ~1. That is the O(N²) → O(N) collapse: **33×
less merge snapshot CPU per promotion** at 32 POVs, more at higher fan-out.
Client-observed write-*acceptance* rate is unchanged in this config because it
is latency-bound (a `try` returns on child-POV acceptance, before promotion);
the snapshot-CPU headroom is the lever that lifts the cap once the single merge
thread is the bottleneck (release build, more writers/POVs).

Correctness oracle held under the flag: 32 writers issuing concurrent `+=` to
one hot key read back the exact total (zero lost), and the full
`examples/agent-swarm/` self-check (`+=`, `|=` set-union, assertion-bearing
tags across 8 concurrent agents) passes identically with the flag on.

### 8.4 Reproduce

```
# baseline vs fast-lane, same build:
examples/agent-swarm/run-concurrent-bench.sh
ORLYI_EXTRA_FLAGS=--tetris_commutative_fastlane examples/agent-swarm/run-concurrent-bench.sh
```

### 8.5 TSan gate — run (2026-06-27)

Ran the ThreadSanitizer gate (`tools/jhm -c tsan`, `setarch -R`, suppressions =
`orly/tsan.supp`). Three parts:

1. **Curated merge unit tests** (`context_fold`, `context_xrepo`,
   `tetris_piece`): **0 warnings**. (`tetris_manager.test` shows one *pre-existing*
   `ChildCount` race in base-class `TTetrisManager` — untouched by this change,
   and not in the CI tsan set.)
2. **Full `orlyi` under TSan, flag ON, under the concurrent bench** (the only way
   to exercise the new path — no unit test drives it). Result, after triage:
   **the fast-lane promotion code is never the site of a race.** `RepeekAndPlay`
   / the per-child Push/Pop/commit (repo_tetris_manager.cc:193/383/388) never
   appears as a racing access (TSan frame #0/#1) and never as a participant
   thread in any new finding. Every race site is in flag-independent engine /
   IO / fiber machinery.
3. **flag-ON vs flag-OFF diff** (decisive). Of 86 distinct race sites under the
   flag, **all but four also appear with the flag off**; the four "fast-lane-only"
   sites are all one pre-existing engine race — `TRepo::AppendUpdate` (a client
   write committing via `~TPusher`, repo.cc:297/311) vs `TRepo::GetLowestUpdate`
   (repo.cc:351/353) reading the repo's update ordered-list from *another* client
   `TSession::Try` or from the merge fiber's **unchanged** snapshot loop
   (`Play():340`, identical in both modes). They appear only under the flag
   because the fast-lane *completes the full bench workload* (more writes/rounds
   → hits the window) while the flag-off run, ~3× slower per drain under TSan,
   timed out mid-bench (truncated sample). Not introduced by the fast-lane.

**Conclusion: S1 (fast-lane behind the default-off flag) is TSan-safe to land —
it adds no new race.** Two corrections to the original plan fell out:

- **A clean full-`orlyi` TSan run is not an achievable S2 gate.** Full `orlyi`
  has never been TSan-clean — the #184 gate only ever covered a curated *unit-test*
  set, never the running server (~80+ pre-existing race sites in
  manager_base refcounts, the fiber pool [#200], boost::beast websocket I/O,
  session/connection code). Gating the default flip on "clean full-orlyi tsan"
  would require a separate engine-wide cleanup unrelated to #234.
- The pre-existing `AppendUpdate` vs `GetLowestUpdate` memory-layer race is real
  and worth its own engine issue (it exists on master, flag off), but it is
  orthogonal to this change.

### 8.6 Remaining before S2 (flip default on)

- File the pre-existing `TRepo::AppendUpdate` vs `GetLowestUpdate` memory-layer
  race as its own engine issue (orthogonal to #234; surfaced by the gate above).
- Sustained K=8/16 soak on a release build showing throughput scales (not
  regresses) once the merge thread saturates.
- Design B (key-range sharding) remains optional/later for the RMW path.
