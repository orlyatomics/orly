# Orly Architecture — Concurrency & Merge Model

This document describes how Orly achieves **lock-free, causally-ordered,
commutative-merge** concurrency — the core of the project's pitch. It is the
design map for the subsystems under `orly/server/` and `orly/indy/`. (It
complements `docs/walkthrough.md`, which is the operational compile/load/invoke
pipeline.)

> **Update ([#262](https://github.com/orlyatomics/orly/issues/262)):** Orly used
> to ship a second, standalone implementation of this model — the 2014-era SPA
> *Flux Capacitor* (`orly/spa/flux_capacitor/`) — which survived only as the
> engine `orlyc` used to run a package's compile-time `test{}` blocks. It has
> been **removed**; `orlyc` now runs tests on the same **indy** engine `orlyi`
> serves. The POV / Tetris / commutative-fold model below is real and lives in
> indy (`orly/server/{pov,tetris_manager,repo_tetris_manager}.*`, `orly/indy/`).
> The key implementation difference from the removed Flux Capacitor: indy orders
> updates by **per-repo sequence number** (§4) rather than a separate in-memory
> causal DAG. Passages below still phrased around the Flux Capacitor's
> causal-graph types (`TLink`, `LocalCauseCount`, the in-memory POV graph)
> describe that retired implementation and are pending a rewrite against indy's
> sequence-number model; the code-map (§7) points at the live indy files.

The concurrency model is implemented in code, not in this doc — file/type names
are given so you can read the real thing. Line numbers are deliberately omitted
(they drift); search by type or method name.

---

## 1. The model in one breath

Every client works in its own private sandbox — a **Point of View (POV)**.
Changes made in a POV are **updates**; an update is a node in a causal graph,
not a clock-stamped record. Updates flow *upward* through a tree of POVs
(private → shared → global) by **promotion**, and the **Tetris** merge decides,
without locks, which competing updates may promote into a shared parent. The
**Flux Capacitor** is the machinery that keeps this promotion **causally
ordered** — an update may only promote once it has no unresolved causes in its
current POV. The global POV is the committed database.

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
            │       TGlobalPov         │   committed database (the sink)
            └────────────▲────────────┘
                         │ promote
              ┌──────────┴──────────┐
              │     TSharedPov      │      optional shared layers (0+)
              └──────────▲──────────┘
                         │ promote
        ┌────────────────┼────────────────┐
   ┌────┴─────┐    ┌─────┴────┐      ┌─────┴────┐
   │TPrivate  │    │TPrivate  │ ...  │TPrivate  │   one private POV per session
   │  Pov     │    │  Pov     │      │  Pov     │
   └──────────┘    └──────────┘      └──────────┘
   updates enter at the leaves and flow up
```

---

## 2. Points of View

**Code:** `orly/server/pov.{h,cc}` (the durable POV object); each POV owns an
indy storage repo (`orly/indy/repo.*`).

A POV is a node in a tree:

- `TPov` — abstract base; holds a pointer to the shared causal-graph `Root` and
  to the `TGlobalPov`, the per-key version index (`TKVIndex`), the list of
  promotable ("trailing") updates, and a `TSync` (recursive read/write lock)
  guarding its state.
- `TGlobalPov` — the single root; the committed state of the whole database. It
  has no parent.
- `TSharedPov` — zero or more intermediate layers; lets several sessions share
  a common base before they diverge.
- `TPrivatePov` — a leaf; one per session/transaction. This is the optimistic
  sandbox a client mutates.
- `TParentPov` / `TChildPov` — mixins providing the parent↔child links
  (`FirstChildPov`/`NextChildPov` sibling lists, a `ParentPov` back-pointer).

The durable representation (`orly/server/pov.h`) records a POV's `SessionId`,
its **audience** (`Private` / `Shared`), its **policy** (`Safe` = synchronous
disk writes / `Fast` = asynchronous), and the chain of shared parents up to
global.

### POVs ride on indy repos

Each POV owns a storage **repo** in the indy layer (`TPov::GetRepo` in
`orly/server/pov.cc` → `orly/indy` `TRepo`). The repo tree **mirrors** the POV
tree: a private POV's repo has its parent POV's repo as its parent repo (or the
global repo directly). A repo holds the in-memory layer, the key→version
mapping, and per-repo **sequence numbers** (see §4). So "a POV" is really two
parallel structures kept in lock-step: the in-memory causal graph
(flux_capacitor) and the storage repo (indy).

### Failure is one-way

A POV's flow state is `Running` → `Paused` → `Failed`. `Fail()` is
**irreversible**: the POV stops participating in promotion (`PartTetris`), fires
its `OnFail` callback, and the server sends the client a `TPovFailure`
notification (`orly/notification/pov_failure.h`). Any of that POV's updates that
had not yet promoted are lost. A POV fails when one of its updates ages out or
loses a merge conflict (see §3).

---

## 3. The Tetris merge

**Code:** `orly/server/tetris_manager.*` and `orly/server/repo_tetris_manager.*`
(the merge over real repos). States are exercised in
`orly/server/tetris_manager.test.cc`.

The metaphor: each promotable update is a **piece** trying to "land" in the
parent POV. The merge is a repeated game (`TTetrisPiece::PlayTetris`), each
round:

1. **Age & expire.** Every candidate piece increments its age; a piece older
   than its `max_age` is `Fail()`ed (its write is discarded, its POV fails).
2. **Order.** Surviving pieces are sorted **oldest-first**, then by **larger
   key-count first** — a deterministic priority so the merge is reproducible.
3. **Assert & promote.** In that order, each piece's **assertion** is checked
   against a snapshot of the parent POV's current state (`TContext`). If it
   holds, the piece **promotes** (its mutation applies to the parent) and its
   state becomes `Promoted`. If not, it stays `Undecided` and is retried next
   round — after the higher-priority pieces have mutated the parent.

A piece is in exactly one of three states: `Undecided`, `Promoted`, `Failed`.

**Conflict resolution without locks** falls out of the assertion + ordering:
when two pieces touch the same key, the first to promote mutates the parent, so
the second's assertion no longer holds and it must wait (or eventually age out).
There is no mutex on the key — serialization is the deterministic age ordering
plus the snapshot-based assertion. This is the mechanism for **field changes**
(read-modify-write).

> Note: folding of commutative mutations is **not** done inside Tetris
> promotion — Tetris promotes the deferred mutations upward as-is; the folding
> happens on the read/compaction path (§5). The two mechanisms are
> complementary.

---

## 4. Causal ordering (the Flux Capacitor)

**Code:** `orly/indy/sequence_number.h`, `orly/indy/repo.h`,
`orly/indy/transaction_base.*`. (The retired Flux Capacitor implemented this as
an in-memory causal DAG of `TUpdate`/`TEvent`/`TLink`; indy instead orders by
per-repo sequence number — see the note below.)

The Flux Capacitor is not one class — it is the causal-graph machinery that
orders promotion by **causality, not wall-clock time**. The pieces:

- **Updates are events** in a "happened-before" DAG. A `TLink` is a directed
  cause→effect edge between updates.
- Each update tracks `LocalCauseCount` — how many of its causes are still in
  the *same* POV. An update with `LocalCauseCount == 0` is a **trailing
  update**: it has no unresolved local dependency and is therefore eligible to
  promote (it sits on its POV's `FirstTrailingUpdate` list). As causes are
  added/removed (`OnLocalCauseJoin` / `OnLocalCausePart`) the update unlinks
  from / relinks to that list.
- **Promotion** (`TUpdate::Promote`) moves a trailing update to the parent POV,
  re-counts its causes in that new context, and re-links it if it still has
  local causes. When an update promotes past the global POV it is deleted (its
  effect has been folded into the committed state / flushed to disk).

So causality is never violated: an update can only move up once everything it
depends on within its POV has already moved up. Tetris (§3) decides *which* of
the currently-promotable updates wins a contested key; the Flux Capacitor
decides *when* an update is allowed to be a candidate at all.

### Sequence numbers are storage, not merge order

`TSequenceNumber` (`uint64_t`, `orly/indy/sequence_number.h`) is a **per-repo**
counter assigned as updates are pushed to storage. It does **not** determine
merge order (the causal graph does). It identifies updates within a repo for
persistence/replication and gives the "popper" a queue discipline:
`GetSequenceNumberStart()` is the oldest unpopped update, and a pop/discard may
only take the update at that boundary (this is the sequence-number-start check
in `orly/indy/transaction_base.cc`).

### Time travel

The original 2014 pitch implied user-facing "time-travel" queries. The **engine
has no native time-travel mechanism** — the Flux Capacitor's causal ordering is
purely an internal merge device. Historical queries are achievable in
**user-space** by encoding the version axis into the key and folding over it on
read (`reduce` + a monoid). See `examples/bitcoin-time-travel/` (balance at a
block height; branched-history multiverse) and `examples/wikipedia-categories/`
(set-union over years).

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

1. Each session has its own `TPrivatePov`. A's `+= 1` becomes a `{Add, 1}`
   deferred mutation in A's POV/repo; B's likewise in B's POV/repo. Neither
   reads the current value, so neither carries an assertion.
2. Both updates become trailing (`LocalCauseCount == 0`) and are offered to the
   parent's Tetris game.
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
