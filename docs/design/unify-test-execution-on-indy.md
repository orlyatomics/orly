# Design: Unify on indy — run orlyc's `test{}` blocks on indy, retire the SPA flux_capacitor engine

> Status: **PROPOSED (RFC)** — tracking #262. Orly ships two storage engines. The real one is
> **indy** (what `orlyi` serves). The other — the 2014-era **SPA flux_capacitor**
> (`orly/spa/`, ~9.7K LOC; `flux_capacitor` alone 5.7K) — survives for exactly
> one reason: it runs the `test{}` blocks a package declares, at *compile time*,
> inside `orlyc`. This RFC proposes finishing the half-built indy-side test
> runner, switching `orlyc` onto it, and deleting the SPA engine.

## 0. The one-paragraph version

`orlyc`, after compiling a package, runs its `test{}` blocks and fails the
compile if any assertion fails (`orly/orlyc.cc:111-121`). It does this by
standing up an in-process SPA `TService` and calling `RunTestSuite`. That is the
**only** consumer of the SPA engine. The package code being tested is **already
engine-agnostic** — the test runner's signature is
`Atom::TCore (*Runner)(Package::TContext &, const TArgMap &)`
(`orly/package/api.h:94`), and `orlyi` already executes that exact runner against
an `Indy::TContext`. So nothing about the *tests* needs SPA; `orlyc` just never
got an indy path. Build that path and the second engine — and its entire class
of concurrency footguns (e.g. #259) — goes away.

## 1. Why two engines exist (and why this one is stuck)

The indy-side test runner was **started and abandoned half-built**. In
`orly/server/session.cc`:

- `TSession::RunInPrivateChildPov` (≈575) already does the hard part: it opens a
  child POV repo, builds an `Indy::TContext` + `Indy::TIndyContext`, runs the
  test `func(indy_context)`, and collects the resulting `TEffects`. **Done.**
- But the **commit** of those effects is `#if 0`'d out (≈616-638) behind a
  `// TODO: finish implementation` — it never builds the `TMetaRecord` / pushes
  the transaction.
- `TSession::RunTestBlock` (≈642) is a stub: `return true`.
- `TSession::RunTestSuite` (≈476) is a stub: the intended design sits in an
  `#if 0` block (install package → `ForEachTest` → fast shared POV → pause →
  with-block → `RunTestBlock`), then `return true`.

So `orlyc` fell back to the SPA engine, whose equivalent path **is** finished
(`orly/spa/service.cc` `RunTestSuite`/`RunTest`/`RunTestAndPromoteOnceIfEffects`).
That fallback is the only thing keeping ~10K lines of legacy engine alive.

## 2. Feasibility (why this is finishable, not a rewrite)

- **The package code is engine-neutral.** `Package::TContext` and
  `Indy::TContext` are siblings under `Orly::TContextBase`
  (`orly/context_base.h`); the generated `Runner` takes the abstract context.
  `orlyi`'s normal method-call path already runs these runners against
  `Indy::TContext` (`orly/server/session.cc:156,334`), as does the Tetris-merge
  assertion checker (`repo_tetris_manager.cc:353`).
- **The write/commit machinery already exists** on the indy side — the normal
  session write path commits effects via `NewTransaction` → `Push` →
  `Prepare` → `CommitAction`. The stubbed test-commit (`RunInPrivateChildPov`
  `#if 0`) is the same shape; it can reuse that path.
- **`orlyi` already test-executes on indy in spirit** — the missing work is the
  three stubs above plus an *in-process* embedding so `orlyc` can drive indy
  without a network server.

## 3. Migration plan (phased, each independently shippable)

**P1 — Finish the indy test runner (server-side first).**
Implement `RunInPrivateChildPov`'s commit, `RunTestBlock` (iterate cases, call
each `Runner`, read the bool result, recurse `SubCases`, isolate/rollback between
cases), and `RunTestSuite` (the `#if 0` design: per-test fresh POV, with-block,
pause). Validate by running a package's `test{}` block through `orlyi` and
matching SPA's pass/fail. Decouples "finish the runner" from "embed in orlyc".

**P2 — Lightweight in-process indy for `orlyc`.**
`orlyc` has no indy instance today. Add a minimal embedding — a `TManager` + a
global repo + a `TSession`, `--mem_sim`, tiny caches, no listener/scheduler
beyond what test execution needs. Mirror `orlyi`'s setup
(`orly/server/orlyi.cc`) but stripped to the compiler's short-lived, single-
threaded, no-network reality. This is the heaviest new code.

**P3 — Switch `orlyc` behind a flag (`--test-engine=indy|spa`, default spa).**
Run `lang_test.py` against **both** engines and diff: identical pass/fail across
all 158 cases is the gate. Any divergence is a semantic-parity bug to fix in P1.

**P4 — Flip default to indy.** Bake on master / the nightly. Keep SPA reachable
via the flag for one cycle as an escape hatch.

**P5 — Delete SPA.** Remove `orly/spa/flux_capacitor/`, the SPA `TService` /
`TSpaContext` (`orly/package/rt.h`), the `spa` binary
(`orly/spa/spa.cc` — confirm vestigial), and migrate/retire the SPA-only
consumers (`orly/manual.cc`, `orly/exit.test.cc`, `orly/sample.test.broken.cc`).
~10K LOC removed; the `flux_capacitor` POV/Tetris concurrency model (and its
footgun class, #259) is gone; `orlyc` and `orlyi` share one engine end to end.

## 4. Risks / parity points to nail in P1↔P3

- **Test isolation.** SPA runs each test in a fresh private child POV that is
  simply discarded (no promotion to anything durable). Indy must give the same
  throwaway semantics — a per-test child repo that is dropped, so one test's
  writes never leak into the next. (Cf. the existing "test-block effect rollback"
  expectation in the lang tests.)
- **Read-your-writes within a test.** A `test{}` can write then read its own
  write in the same case; SPA's `RunTestAndPromoteOnceIfEffects` makes that work
  via promote-then-read. Indy must commit the effect into the test's POV so a
  later read in the same case sees it.
- **The `TMetaRecord` / predicate-results TODO.** The stubbed commit references
  meta-args-by-name + predicate results that were never finished. Tests likely
  don't need the full meta record (no replication/notification at compile time),
  so a minimal meta record may suffice — to be confirmed.
- **`with`-block setup**, sub-case nesting, and verbose/failure reporting must
  match SPA's output (lang_test compares it).
- **`orlyc` startup cost.** Embedding indy must stay cheap — `orlyc` runs per
  package in `lang_test` (158×) and in every build. `--mem_sim` + tiny caches;
  measure that we don't regress compile-with-tests latency.

## 5. Payoff

One engine instead of two: ~10K LOC deleted, a 2014 concurrency subsystem (POV /
flux Tetris / `TSync`) and its whole footgun class retired, `orlyc` and `orlyi`
sharing the same storage semantics so "passes its tests at compile time" means
"behaves the same on the server". The maintenance/bug surface that produced the
#259 race lineage disappears.

## 6. Open questions

- Is the `spa` binary (`orly/spa/spa.cc`) used by anyone, or already vestigial?
- Does any lang-test rely on a SPA-specific behavior that indy doesn't reproduce
  (the P3 diff will tell us)?
- Can the per-test indy POV be made cheap enough that 158 sequential test
  packages don't slow the suite vs SPA?
