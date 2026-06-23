# Design: Cross-package imports (issue #171)

## 0. The key realization

Orly's import is **not** a whole-package namespace import. The grammar
(`orly/orly.nycr:233`) is a **typed, single-symbol alias**:

```
import_def : def -> name is_kwd type opt_name from_kwd package_ref semi;
/* foo is int bar from bar_package; */
```

`foo is int bar from bar_package;` = "bind local name **foo**, of declared type
**int**, to symbol **bar** from package **bar_package**." `package_ref` is either an
alias (`bar_package`) or a literal (`package <a/b/c> #N`).

This is the linchpin: **because the type is declared at the import site**, the
importing package can bind and type-check `foo` using only local information — the
remote package's symbol table is *not* needed for the importer to compile. That
eliminates the hard parts the agents flagged (no qualified-name syntax, no
cross-package scope lookup, no cross-package type inference). Cross-package wiring is
deferred entirely to codegen (`#include` + a namespaced call) and link.

## 1. Current state, per layer (verified)

| Layer | State | Evidence |
|---|---|---|
| Grammar | `TImportDef` exists | `orly.nycr:233`, `:238-240` |
| Synth | `import` → `throw TNotImplementedError` | `synth/def_factory.cc:82-86` |
| Symbol/scope | no import storage; names bind via `TScope::TryGetDef<T>(name)` up the parent chain during a Bind pass | `synth/scope_and_def.{h,cc}` |
| Typecheck | `TScope::TypeCheck()` iterates functions calling `GetType()`; needs all refs bound first | `symbol/scope.cc:65-76` |
| Codegen | `NeededPackages` (`set<Package::TName>`) declared but **never populated**; `WriteImportIncludes` emits `#include "pkg.h"`; `.link.cc` dep-map already iterates it; cross-pkg call naming = `NS<pkg>::F<fn>(ctx,args)` | `code_gen/package.{h,cc}:71,631,442` |
| Driver | builds+links **all** packages in the `packages` map; dead loop would populate it; `GetImportPackages()` doesn't exist | `compiler.cc:172,222-225` |
| `__orly__` root | `found_root` not wired | `orlyc.cc:98` TODO |

Codegen and link are ~80% scaffolded; the missing core is **synth recording the
import** + **codegen populating `NeededPackages` and emitting the namespaced call** +
**the driver building the dependency**.

## 2. Design decisions (proposed)

1. **Semantics:** typed single-symbol alias. `local is <type> [remote] from <pkg>`.
   `local` becomes a def of the declared type in the package scope, backed by an
   "external reference" to `<pkg>`'s top-level symbol `<remote>` (defaults to `local`
   if `opt_name` is empty).
2. **Imported symbol kind (MVP):** a top-level function/value (Orly top-level defs are
   functions; a 0-arg one is a value). Using `local` emits a call
   `::NS<pkg>::F<remote>(ctx, …)`. Importing *types* is out of MVP scope.
3. **No qualified names, no cross-package scope merge.** The alias is the resolution
   mechanism; ordinary `TRef` binding to the local alias is unchanged.
4. **Type checking trusts the declared type (MVP).** The original code never verified
   either. Verifying the declared type against the remote symbol's actual type
   (requires building the remote package) is a Stage-5 safety add, with a clear error
   on mismatch.

## 3. Per-layer implementation plan

**A. Synth (the heart) — `synth/def_factory.cc`, new `synth/import_def.{h,cc}`, `synth/package.{h,cc}`**
- Replace the `throw` with a handler that constructs a new `Synth::TImportDef : TDef`,
  registered in the package scope under `local` (so `TryGetDef("foo")` resolves it).
- It holds: declared `Type::TType` (from `GetType()`), remote name, and the resolved
  package ref.
- When referenced, it yields an Expr of the declared type that codegen emits as the
  external call (see C).
- `Synth::TPackage` accumulates the set of imported package refs; expose
  `GetImportPackages()` (the name the dead driver loop already expects).

**B. Symbol / Expr — `orly/expr/`, `orly/symbol/`**
- Add an `Expr::TImportRef` (or extend `TRef`) carrying {package name, remote symbol,
  declared type}. `GetTypeImpl()` returns the declared type (no inference).

**C. Codegen — `code_gen/package.cc`, `code_gen/*`**
- During package build, insert each imported package's `Package::TName` into
  `NeededPackages` → `WriteImportIncludes` emits the `#include`, `WriteLink` emits the
  dep-map entry (both already wired).
- Emit `Expr::TImportRef` as `::NS<pkg>::F<remote>(ctx, …)` using the existing
  namespace/printer convention.

**D. Driver — `orly/compiler.cc`**
- Add `TPackageBuilder::GetImportPackages()` (delegates to `Synth::TPackage`).
- Re-port the loop: resolve each `package_ref` → `TRelPath`, skip if already in
  `packages`, require `found_root`, insert a `TPackageBuilder`, `todo.push`. The
  end-of-Compile link step already links them all.

**E. `__orly__` root + package_ref resolution — `compiler.cc`, `orlyc.cc`**
- Walk up from the source file to find the dir containing `__orly__`; resolve
  `package_ref` relative to it. Literal ref → `<path>.orly` + version; alias ref →
  (see open question 2).

## 4. Staged delivery (reviewable vertical slices)

- **Stage 0** — this doc + a *failing* two-package lang test (`b.orly` exports a fn;
  `a.orly` imports & calls it; expected output recorded).
- **Stage 1 — Synth.** Import def + scope registration + `GetImportPackages()`. Gate:
  a package using `import` no longer throws; a semantic-only compile of the importer
  binds and type-checks `foo` against the declared type.
- **Stage 2 — Codegen.** Populate `NeededPackages`; emit the namespaced call + include.
  Gate: generated `a.h/.cc` contain `#include "b.h"` and `::NSb::Fbar(ctx,…)`.
- **Stage 3 — Driver + root.** `GetImportPackages` loop + `__orly__` root + ref→path.
  Gate: `orlyc a.orly` builds `b` then `a` and links one `.so`.
- **Stage 4 — End-to-end.** The two-package program runs and returns the expected
  value; add to `tests/lang_tests` (and a `examples/` demo if worthwhile).
- **Stage 5 (optional) — Type safety.** Build the remote package and verify the
  declared type matches the remote symbol's real type; clear error on mismatch.

Each stage is its own PR; Stages 1–3 are independently mergeable behind the fact that
no shipping `.orly` uses `import` yet.

## 5. Decisions (MVP scope — assumptions, revisable)

These were the open questions; resolved to the following defaults to define the MVP. Any
can be revisited, but the staged plan is built on them:

1. **Imported symbol kind** — **functions/values only.** Importing types is out of MVP
   scope (changes resolution materially); revisit after the value path works.
2. **Alias resolution** — **literal refs only for MVP** (`package <a/b/c> #N`,
   unambiguous path + version). `alias_package_ref` (a bare name) is deferred until we
   know whether a `using`/alias table is intended; it errors with a clear "alias imports
   not yet supported — use a literal `package <…> #N` ref" message in the meantime.
3. **Version selection** — N/A for MVP (literal refs carry an explicit version).
4. **Cyclic/mutual imports** — **forbidden for MVP**: the work-queue dedup handles DAGs;
   a detected cycle is a clear compile error. Mutual recursion across packages (codegen
   forward-decl/include ordering) is a follow-up.
5. **Type verification** — **trust the declared type in MVP** (as the original did);
   verify against the remote symbol's real type in Stage 5 with a clear mismatch error.

## 6. Risks

- Synth/Expr binding + typecheck are the recently-modernized core (#105/#128 variant /
  optional conformance). The new import def/expr must not regress those — Stage 1 runs
  the full `lang_test` suite.
- Codegen cross-package emission must match the exact `NS`/`F` naming and `ctx`-passing
  convention; mismatch = link errors. Stage 2 asserts on generated text.
- `Compile()` builds dependencies in sequence sharing the global Synth context
  (`static mutex Compiling` + `ClearErrors()`); building multiple packages per call
  must reset per-package synth state cleanly (existing loop already calls
  `BuildSymbols` per builder — confirm no cross-package state bleed).
