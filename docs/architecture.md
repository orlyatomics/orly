# Orly Architecture — Concurrency & Merge Model

This document describes how Orly achieves **lock-free, optimistic,
commutative-merge** concurrency — the core of the project's pitch. It is the
design map for the subsystems under `orly/server/` and `orly/indy/`. (It
complements `docs/walkthrough.md`, which is the operational compile/load/invoke
pipeline.)

> **History ([#262](https://github.com/orlyatomics/orly/issues/262)):** Orly once
> shipped a second, standalone implementation of this model — the 2014-era SPA
> *Flux Capacitor* (`orly/spa/flux_capacitor/`), which ordered promotion with an
> in-memory causal DAG (`TUpdate`/`TEvent`/`TLink`, `LocalCauseCount`). It
> survived only as the engine `orlyc` used to run compile-time `test{}` blocks
> and was **removed** in #262; `orlyc` now runs tests on the same **indy** engine
> `orlyi` serves. This document describes the live indy implementation, which
> orders updates by **per-repo sequence number** (§4) — there is no separate
> causal graph.

The concurrency model is implemented in code, not in this doc — file/type names
are given so you can read the real thing. Line numbers are deliberately omitted
(they drift); search by type or method name.

---

## 1. The model in one breath

Every client works in its own private sandbox — a **Point of View (POV)**.
Changes made in a POV are **updates**; each update is stamped with a **per-repo
sequence number**, not a wall clock. Updates flow *upward* through a tree of POVs
(private → shared → global) by **promotion**, and the **Tetris** merge decides,
without locks, which competing updates may promote into a shared parent. Within a
single POV updates promote in sequence order (oldest first); across competing
POVs the merge picks a deterministic winner (§3). The global POV is the committed
database.

Two kinds of write get very different treatment, and the difference is the
whole game:

- A **field change** (`x = x + 1`) is a read-modify-write. It carries an
  assertion ("x was what I read"), so two of them on the same key *conflict* —
  Tetris promotes one and retries/fails the other.
- A **field call** (`x += 1`, `s |= {e}`) is a **deferred commutative
  mutation** (`{Add, 1}`, `{Union, {e}}`). It carries no read-assertion, so two
  of them on the same key *do not conflict* — both promote, and the value is
  reconstructed by folding the mutations on read. This is why N concurrent
  writers can all `+=` the same hot key and lose nothing.

```
            ┌─────────────────────────┐
            │       global POV        │   committed database (the sink)
            └────────────▲────────────┘
                         │ promote
              ┌──────────┴──────────┐
              │     shared POV      │      optional shared layers (0+)
              └──────────▲──────────┘
                         │ promote
        ┌────────────────┼────────────────┐
   ┌────┴─────┐    ┌─────┴────┐      ┌─────┴────┐
   │ private  │    │ private  │ ...  │ private  │   one private POV per session
   │  POV     │    │  POV     │      │  POV     │
   └──────────┘    └──────────┘      └──────────┘
   updates enter at the leaves and flow up (audience = Private / Shared)
```

---

## 2. Points of View

**Code:** `orly/server/pov.{h,cc}` (the durable POV object); each POV owns an
indy storage repo (`orly/indy/repo.*`).

A POV is a `TPov` durable object (`orly/server/pov.{h,cc}`). It records:

- a `SessionId`;
- an **audience** — `Private` (a leaf; one per session/transaction, the
  optimistic sandbox a client mutates) or `Shared` (an intermediate layer that
  lets several sessions share a common base before they diverge);
- a **policy** — `Safe` (synchronous disk writes) or `Fast` (asynchronous);
- the chain of **shared parents** up to the global POV (`TSession::GlobalPovId`),
  which has no parent and holds the committed state of the whole database.

There is no separate POV class hierarchy or in-memory causal graph — the tree is
just `TPov`s linked by their shared-parent chain, each backed by an indy repo.

### POVs ride on indy repos

Each POV owns a storage **repo** in the indy layer (`TPov::GetRepo` in
`orly/server/pov.cc` → `orly/indy` `TRepo`). The repo tree **mirrors** the POV
tree: a POV's repo has its parent POV's repo as its `ParentRepo` (or the global
repo directly). A repo holds the in-memory layer, the key→version mapping, and a
per-repo **sequence-number** counter (`NextUpdate`, see §4). A read builds a
`TContext` (`orly/indy/context.cc`) over the repo **and all of its ancestor
repos** and folds across them (§4), so a child POV transparently reads through to
what its parents and the global POV hold.

### Failure is one-way

`Fail()`ing a POV's repo is **irreversible**: it stops participating in
promotion, and the server sends the owning session a `TPovFailure` notification
(`orly/notification/pov_failure.h`). Any of that POV's updates that had not yet
promoted are lost. A POV fails when one of its updates loses a merge conflict
enough times in a row (see §3).

---

## 3. The Tetris merge

**Code:** `orly/server/tetris_manager.*` and `orly/server/repo_tetris_manager.*`
(the merge over real repos). States are exercised in
`orly/server/tetris_manager.test.cc`.

Each shared/global POV has a `TPlayer`; each child POV that feeds it is a
`TChild`. The player runs a round (`TRepoTetrisManager::TPlayer::Play`) whenever
children have work:

1. **Snapshot.** Each ready child is `Refresh`ed: it `Peek`s its **next** update
   — the one at its repo's lowest unpopped sequence number
   (`GetSequenceNumberStart`) — and reads that update's `TMetaRecord` (recorded
   args, timestamp, random seed, and the **expected predicate results** — the
   assertions captured at write time). A child's `Age` increments each round.
2. **Order.** Ready children are sorted **oldest-`Age` first** (`SortsBefore`) —
   a deterministic priority, so a starved writer eventually wins.
3. **Promote.** In that order, `TChild::Play` re-runs the update's method against
   a snapshot `TContext` of the parent's current state (`TestAssertions`) and
   compares the fresh predicate results to the recorded ones. If they match, the
   update **promotes**: `transaction->Push(parent_repo, update)` +
   `transaction->Pop(child_repo)` (so the parent assigns it a new sequence
   number) and the session gets an `Accepted` notification. **At most one**
   assertion-bearing update promotes per round (the others retry next round,
   against the now-mutated parent). If an update's assertion fails **10 rounds in
   a row** (`FailureCount >= 10`), its repo is `Fail()`ed.

**Conflict resolution without locks** falls out of the assertion + ordering:
when two updates touch the same key, the first to promote mutates the parent, so
the second's assertion no longer holds and it retries (or eventually fails).
There is no mutex on the key — serialization is the deterministic `Age` ordering
plus the snapshot-based assertion replay. This is the mechanism for **field
changes** (read-modify-write); a **field call** carries no predicate results
(`IsAssertionFree`), so it never conflicts (see §5).

> **Commutative fast-lane (#234, `--tetris_commutative_fastlane`, default off).**
> Because assertion-free (commutative) children provably cannot conflict, the
> fast-lane promotes **all** ready ones each round — each in its own
> transaction — instead of one piece per round; assertion-bearing children keep
> the at-most-one-per-round discipline. See `docs/design/concurrent-merge-throughput.md`.

> Note: folding of commutative mutations is **not** done inside Tetris
> promotion — Tetris promotes the deferred mutations upward as-is; the folding
> happens on the read/compaction path (§5). The two mechanisms are
> complementary.

---

## 4. Ordering by sequence number

**Code:** `orly/indy/sequence_number.h`, `orly/indy/repo.h`,
`orly/indy/transaction_base.*`, `orly/indy/context.cc`, `orly/indy/present_walker.h`.

There is no separate causal graph. `TSequenceNumber` (`uint64_t`) is a
**per-repo** counter (`TRepo::NextUpdate`): every update pushed to a repo takes
the next value. That single number does two jobs.

**Promotion / pop order (within a repo).** A repo's updates promote and pop in
sequence order: the "popper" may only take the update at the oldest-unpopped
boundary (`GetSequenceNumberStart()`, enforced by the sequence-number-start
check in `orly/indy/transaction_base.cc`). Tetris (§3) Peeks exactly that update
from each child and decides *which child* wins a contested key, by `Age`. On
promotion the parent repo assigns the update **its own** next sequence number, so
numbering stays monotonic in the repo an update lands in. The same boundary is
the persistence/replication queue discipline (oldest unflushed first).

**Read resolution (across the repo chain).** A point read walks the `TContext`'s
repo chain (the repo + all ancestors) and resolves a key that appears at several
entries/levels by sequence number: for the same key the **highest sequence wins**
(`present_walker.h` `TItem::operator<`), i.e. last-writer-wins for plain assigns.
A run of same-mutator **commutative** entries is instead *folded* rather than
shadowed (§5) — that is what lets a `+=` survive alongside the value it adds to.

### Time travel

The original 2014 pitch implied user-facing "time-travel" queries. The **engine
has no native time-travel mechanism** — sequence ordering is purely an internal
merge/storage device. Historical queries are achievable in **user-space** by
encoding the version axis into the key and folding over it on read (`reduce` + a
monoid). See `examples/bitcoin-time-travel/` (balance at a block height;
branched-history multiverse) and `examples/wikipedia-categories/` (set-union over
years).

---

## 5. Commutative field calls (the headline property)

**Code:** `orly/var/mutation.{h,cc}` (`TMutation` / `TMutator` / `Augment`),
`orly/rt/mutate.h` (the runtime-typed counterpart), the session deferred-
commutative path (`orly/server` session layer), `orly/indy/update.cc` (entries
carry their `TMutator`), and `orly/indy/disk/fold_data_file.*`
(compaction-time fold).

A field call lowers to a **deferred mutation** carrying its operator —
`+=` → `{Add, n}`, `|=` → `{Union, s}`, and similarly for the other
commutative-and-associative operators (`Add`, `Mult`, `Or`, `And`, `Xor`,
`Union`, `Intersection`, `SymmetricDiff`). Crucially it is **not** resolved to a
value at write time. The value is reconstructed by **folding** the run of
same-mutator mutations for a key when it is read. Three places cooperate:

1. **Compose-time** — `TMutation::Augment` combines two same-mutator commutative
   mutations into one (e.g. `{Add,3}` + `{Add,7}` → `{Add,10}`). Mixed or
   non-commutative mutators refuse to combine.
2. **Read-path fold** — when a key is read, the run of same-mutator entries is
   folded into the resolved value; the per-entry `TMutator` is preserved through
   the storage layers (`orly/indy/update.cc`, `orly/indy/disk/data_file.cc`) so
   the read path knows how to fold.
3. **Compaction fold** — `TFoldDataFile` folds same-mutator runs on disk at
   compaction time so the field-call mechanism doesn't grow files without bound
   (issue #49, phase 4).

Because field calls carry no read-assertion, two of them on the same key from
different POVs **do not conflict in Tetris** — both promote, and the read folds
them. That is the difference that makes the headline workload work: N concurrent
writers each `+= 1` a hot key and every increment lands.

> This is the 2026 "#49 arc." Before it, concurrent `+=` lost roughly
> `(N-1)/N` of its writes because the merge coerced same-key field calls to
> assigns. (A stale comment in `orly/var/mutation.test.cc` still says the
> consumers are "not yet wired up" — that predates the rest of the arc; the
> mechanism is wired and validated end-to-end.)

---

## 6. A worked example — two sessions, one counter

```
Session A: *<['views', page]>::(int) += 1     Session B: ... += 1
```

1. Each session has its own private POV. A's `+= 1` becomes a `{Add, 1}`
   deferred mutation in A's POV/repo; B's likewise in B's POV/repo. Neither
   reads the current value, so neither carries an assertion.
2. Each update is the lowest-sequence entry in its repo, so both are offered to
   the parent's Tetris game (`IsAssertionFree`, so neither can conflict).
3. Both promote — there is no assertion to conflict, because `{Add,1}` and
   `{Add,1}` commute. They land in the parent (eventually global) as two
   `{Add,1}` entries on the key.
4. A reader of the key gets the **fold** of the run: `0 + 1 + 1 = 2`. No lock
   was taken; no increment was lost.

Contrast a **field change** version (`x = x + 1` in each session): each carries
"x was 0", so only one promotes; the other's assertion fails and it
retries/fails — the classic lost-update serialization that field calls avoid.

---

## 7. Code map

| Concern | Where |
|---|---|
| POV graph (private/shared/global), durable POV object (audience, policy, parents) | `orly/server/pov.{h,cc}` |
| Promotion + Tetris merge (over real repos) | `orly/server/{tetris_manager,repo_tetris_manager}.*` |
| Per-repo sequence numbers (ordering) | `orly/indy/sequence_number.h`, `orly/indy/repo.h` |
| Storage repos / memory layer / mapping | `orly/indy/repo.*`, `orly/indy/manager*.*` |
| Updates & per-entry mutator | `orly/indy/update.{h,cc}` |
| Commutative mutations (`Augment`) | `orly/var/mutation.{h,cc}`, `orly/rt/mutate.h` |
| Compaction-time fold | `orly/indy/disk/fold_data_file.*` |
| Transactions (push/pop/promote) | `orly/indy/transaction_base.*` |

---

## 8. What's verified, and what isn't

**Tested:**
- Commutative compose — `orly/var/mutation.test.cc` (`Augment` over Add/Mult/…).
- Cross-POV merge semantics — `orly/indy/context_xrepo.test.cc` (a `+=1` in a
  child POV and a `+=1` in global read back as `2`), `orly/indy/context_fold.test.cc`.
- Tetris promotion/conflict states — `orly/server/tetris_manager.test.cc`.
- Compaction fold — `orly/indy/disk/fold_data_file.test.cc`.
- **Concurrent integration** — `examples/wikipedia-pageviews/` runs 8 concurrent
  WebSocket writers against hot keys and asserts zero lost updates; CI runs it as
  the workload-level proof of the field-call property.

**Known gaps (tracked):**
- No memory-level data-race validation — CI runs **no ThreadSanitizer/ASan/UBSan**
  (issue #177). Functional merge correctness is tested; data-race freedom of the
  lock-free paths is not.
- Several persistence/load paths are unimplemented stubs (issue #173 — see the
  persistence boundary below); cross-package imports were never re-ported (issue #171).

### Persistence boundary (issue #173)

Orly **does** persist and restart, via `orlyi` on a real disk engine:

- **`orlyi` on a real disk engine.** The indy server runs on `TDiskEngine` (`orly/server/server.cc`)
  with fsync, an append log, and named instances; memory layers flush to disk and the merge /
  compaction passes run, so a live repo's reads union its in-memory and on-disk layers.

What issue #173's stubs gate is narrower: the indy **L0 manager's path that turns a saved
on-disk repo/durable-object image back into a live in-memory object** — used for in-process
repo reopen / raw-image reload. That path was never ported, and now fails with an explicit
"not implemented" error (rather than a bare `TODO`) if ever reached:

- **Repo reconstruct** — `ReconstructRepo` → `TManager::ConstructRepo` (`orly/indy/manager.cc`)
  never resolves a reconstructed repo's parent or TTL.
- **Durable-object load** — `L0::TManager::Open`'s load-from-disk arm (`orly/indy/manager_base.h`)
  and the `Delete`/`Save`/`TryLoad` overrides (`orly/indy/manager.h`), gated off because
  `TObj::OnDisk` is never set and the loader is `#if 0`'d.

So: durability/restart at the **server** layer works via the indy disk engine; what is missing is
the indy-layer raw repo/object reload-from-image. Wiring that up is a separate effort.
