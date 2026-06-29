# Design: Batched / pipelined writes — amortizing the per-write round-trip

> Status: **IMPLEMENTED** (option B, #253). The batch write verb
> `try {pov} pkg method [<{...}>, <{...}>, ...];` folds N method calls into
> **one** transaction; reply is a JSON array of the N per-call results. See
> `TSession::TryBatch` (`orly/server/session.cc`), the `try_batch_stmt` grammar
> rule, `docs/PROTOCOL.md`, and the `call_batch`/`CallBatch`/`callBatch` clients.
> The design below is the original proposal, motivated by the
> write-path characterization in
> [`concurrent-merge-throughput.md`](concurrent-merge-throughput.md) §8–§9: single-write
> latency (~800 µs) sits at the architecture's floor and is ~100% server-side, so
> the only multiplicative write lever left is amortizing the *per-round-trip* cost
> across many writes. The hard part — folding multiple commutative effects into a
> single update — **already exists** (the `#49`/`#232` deferred-entry path), so
> this is mostly protocol + a session entry point, not a new engine subsystem.

## 0. The key realization

Every per-write cost we measured is paid **per round-trip**, and the dominant
ones are fixed regardless of how much the call actually mutates:

| stage | ~µs | per | amortizable in a batch? |
|---|---:|---|---|
| parse (statement text → CST + arg→Sabot) | ~90 | request | **yes** — parse one frame, not N |
| Try: pov-open + build TContext/TIndyContext + pkg/func lookup | large | request | **yes** — once per batch |
| Try: `func->Call` (method body exec) | ~8 | call | no — intrinsic per call |
| txn build + **commit + destruction** (`AppendUpdate`, replication enqueue, pool frees) | ~half of Try | **transaction** | **yes** — commit once, not N times |
| asio/beast I/O + loopback + reply serialize | ~280 | round-trip | **yes** — one frame in, one out |

A single `try {pov} pkg method <args>;` does exactly one method, builds exactly
one transaction, and replies once. Drive a bulk load (agent-swarm ingest,
wiki-pageviews, any `+=`/`|=` fan-in) and the context build, the commit/destroy,
the parse, and the full network round-trip are each multiplied by N — even
though the N writes could share all of it.

**The opening:** the engine **already** folds multiple commutative effects into a
single update. `TSession::Try` (`orly/server/session.cc` ~171–265) collects a
method's effects into a `deferred_entries` list, sorts by `TKey`, and emits **one**
`TUpdate` with N `AddEntry` calls inside **one** `transaction->Push/Prepare/CommitAction`.
A batch of method calls maps directly onto that: run each call, accumulate every
call's effects into the *same* effect set, emit one update, commit once.

## 1. Current state (verified)

- **Transport:** one WS text frame = one orlyscript statement string, terminated
  `;`, parsed by `parse_stmt` into a `TStmt` AST and dispatched by `TStmtVisitor`
  in `orly/server/ws.cc`. `try` is `TTryStmt` (`ws.cc:244`), grammar
  `try_stmt : stmt -> try_kwd pov_id:id_expr package:name_list method_name:name args:obj_expr semi;`
  (`orly/client/program/program.nycr:136`).
- **Execution:** `TTryStmt` builds a `TClosure` from the arg record and calls
  `GetSession()->Try(TMethodRequest(pov_id, fq_name, closure))` →
  `TSession::Try` runs the method, builds + commits one transaction, returns one
  `TMethodResult`, which the visitor JSON-marshals into the single reply.
- **Clients:** `call(pov, pkg, method, args)` builds the statement string and does
  one `send` (one recv). `clients/python/orly/__init__.py:151`, mirrored in
  `clients/go` and `clients/ts`. The example drivers loop this per element.

## 2. Design options

### A. Transport-level statement batch (frame-level)
One WS frame carrying N statement strings; server parses + runs each as today,
replies with an array of N results.
- **Amortizes:** parse-frame overhead and **the network round-trip** (one frame
  in / out instead of N). Real, since I/O is ~35% of per-write cost.
- **Does NOT amortize:** context build or commit — each statement is still its own
  `Try` → its own `TContext` → its own transaction. The biggest server-side cost
  (commit/destroy + context) stays ×N.
- **Cost:** small. No grammar change to the statement itself; a framing convention
  (e.g. a JSON envelope, or split-on-`;` already implied by the protocol).

### B. Batch try — same method, N arg records, one transaction  *(recommended v1)*
A new statement that invokes **one** `(pkg, method)` against **N** argument
records, folding all effects into a single transaction:
```
try {<pov>} <pkg> <method> [ <{...args1}>, <{...args2}>, ... ];
```
- **Amortizes everything amortizable:** parse once, **one** `TContext`/pov-open/
  func lookup, run `func->Call` N times accumulating into one effect set, **one**
  `TUpdate` + **one** commit, **one** reply frame.
- **Matches the headline workload exactly:** bulk load / fan-in is almost always
  the *same* method applied to many inputs (`add_mention`, `bump`, `add_cooccur`).
- **Cost:** one grammar rule + a `TSession::TryBatch` entry point that loops
  `func->Call` and merges effects before the existing commit block.

### C. Heterogeneous batch — N arbitrary (pkg, method, args), one transaction
Fully general: a list of distinct calls committed atomically together.
- **Most powerful** (atomic cross-method multi-write) but the most semantics to
  pin down (per-call package/function resolution, mixed commutative/assign
  ordering, partial-failure policy) and the least-requested. **Out of scope for
  v1; B's server path generalizes to it later** (the fold loop just varies the
  function per element). Tracked as a gated follow-up in
  [#255](https://github.com/orlyatomics/orly/issues/255) — implement on demand.

## 3. Recommendation

Ship **B** (same-method batch into one transaction) as v1, behind the existing
`try` keyword with a list-of-records argument. It captures the full multiplicative
win for the workload that actually batches, reuses the proven deferred-entry fold,
and leaves a clean path to C. Optionally layer A's framing later for the
heterogeneous/streaming case; B alone already collapses the dominant cost.

## 4. Server-side mechanics (option B)

Add `TSession::TryBatch(server, pov_id, fq_name, method_name, vector<TClosure> calls)`
that mirrors `Try` but loops the method body and merges effects before one commit:

1. Open pov, build `TContext` / `TIndyContext`, resolve `func` — **once**.
2. For each arg record: `func->Call(indy_context, prog_args_i)`; collect
   `indy_context.MoveEffects()` into a single accumulating `effects` map. The
   existing per-key logic (`session.cc` 183–211) already handles "many entries
   for one key": defer-safe commutative mutators append to `deferred_entries`
   (folded by the read path), everything else resolves into `op_by_key`.
3. Build **one** `TUpdate` (one `op_by_key`, the sorted `deferred_entries`), one
   `TMetaRecord`, `Push/Prepare/CommitAction` **once**, then the #234 backpressure
   yield once.
4. Reply: a JSON array of N results (per-call return value), or — if the batch is
   pure-effect — a single `{"count": N}`. (Decide in review; array is the safe
   superset.)

### Semantics to pin down (the review surface)

- **Atomicity:** the batch is one transaction → **all-or-nothing**. A throw in any
  call aborts the whole batch (pre-commit, so it unwinds cleanly and replies an
  error — same path #250 verified). This is a *feature* (atomic multi-write) but
  callers must know one bad record rejects the set. v1: all-or-nothing; a
  `best_effort` mode can come later.
- **Intra-batch read isolation:** every call runs against the **same** pre-batch
  snapshot (`TContext` over the pov's repo). There is **no read-your-writes within
  a batch** — call *k+1* does not see call *k*'s mutation. This is correct and
  desirable for commutative fan-in (the whole point is they commute), but it must
  be documented: a batch is a write-coalescing primitive, not a mini-transaction
  script. Sequential-dependent writes must stay separate calls.
- **Commutative vs assign within a batch:** commutative ops to the same key fold
  (N `Add` entries, summed on read) — exactly the win. Non-commutative `Assign`/
  `Delete` to the same key within one batch collapse in **statement order**
  (last wins), since they share one update's `op_by_key`. State this explicitly.
- **Metadata / replication granularity:** today one `TMetaRecord` per update
  captures the method + args + predicate results. A batch is one update → one meta
  record for N calls. Define what it records (method name + N arg sets, or a batch
  marker) and confirm the replication-notification path (`UpdateProgress`) is fine
  emitting one notification per batch. Flag for review — this is the one place the
  fold changes observable behavior.
- **Predicate/assertion results:** assertions are per-call; a batch with assertions
  is no longer assertion-free and would lose the commutative fast-lane property.
  v1 scope: batches are intended for the commutative/no-assertion path; document
  that an asserting batch still works but commits as one asserted update.

## 5. Protocol & clients

- **Grammar:** add a `try_batch_stmt` rule in `program.nycr` (regenerated via
  nycr) taking `open_bracket obj_expr {, obj_expr} close_bracket` in the args
  position; new `TTryBatchStmt` visitor in `ws.cc` building the `vector<TClosure>`
  and calling `TryBatch`.
- **Reply JSON:** `{"status":"ok","result":[r1, r2, ...]}` (array), documented in
  `docs/PROTOCOL.md`.
- **Clients:** `call_batch(pov, pkg, method, [args, ...])` in `clients/python`,
  `clients/go`, `clients/ts`, building the bracketed-list statement and doing one
  send/recv. The example drivers that loop `call` over a list (agent-swarm ingest,
  wiki-pageviews) switch to one `call_batch`.

## 6. Expected gain & how to measure

Per the §0 breakdown, a batch of B replaces N×(parse + context + commit + I/O)
with 1×(parse + context + commit + I/O) + N×(`func->Call` ~8 µs + per-key fold).
Conservative ~10 calls/batch → effective per-write ~800 µs → ~100–200 µs ≈
**3–5×**; larger batches approach the `func->Call`+fold floor.

**Benchmark:** `examples/agent-swarm/benchmark.py` (the existing perf repro, FLAT
verdict from #227) and `concurrent_bench.py` — add a `--batch N` mode and report
events/sec batched vs unbatched on the same ingest. wiki-pageviews (96 hot keys,
commutative `+=`) is the integration check: batched ingest must produce the
**identical** 96 counters + 4 totals it does today (the demo already self-checks
exact values, so correctness is gated for free).

## 7. Risks / out of scope

- **Only helps batchable workloads.** A strict one-write-at-a-time latency-bound
  caller stays at the ~800 µs floor; this is throughput, not latency.
- **Metadata semantics** (§4) is the one behavior change — settle it in review
  before coding, and keep the replication-notification contract intact.
- **Heterogeneous (C) and best-effort/partial-failure** modes are deferred —
  option C tracked in [#255](https://github.com/orlyatomics/orly/issues/255).
- **No engine/storage change.** If a design step here implies touching the merge
  or storage path, that's a signal the scope is wrong — the whole point is that
  the fold already exists and we are only feeding it more entries per update.

## 8. Open questions for review

1. Reply shape: array of per-call results vs a single count for pure-effect
   batches?
2. Metadata: one meta record per batch recording all N arg sets, or a compact
   batch marker — and is one `UpdateProgress` notification per batch acceptable to
   every current consumer?
3. v1 atomicity: all-or-nothing only, or also a `best_effort` variant from the
   start?
4. Grammar: extend `try` with a list argument (B), or introduce a distinct
   `try_batch` keyword for clarity?
