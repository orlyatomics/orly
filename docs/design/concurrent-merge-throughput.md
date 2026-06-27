# Design: Concurrent write throughput — the single-threaded global merge

> Status: **gated RFC** (verified findings + design; not yet scheduled),
> tracked by **issue #234**. Same bar as the merge/graph RFCs — do not
> implement until there is demand, and implement in a fresh session behind the
> TSan gate (#201).

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
