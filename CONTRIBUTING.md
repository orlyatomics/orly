# Contributing to Orly

Thanks for your interest in Orly. This document covers the conventions CI
enforces; see [`README.md`](README.md) and [`docs/`](docs/) for architecture
and background.

## Building & testing

Orly builds with gcc 13 / `-std=c++23` on Ubuntu 24.04 (see the
[README status note](README.md) for the supported toolchain).

```sh
make debug        # bootstrap jhm + build every target (debug)
make test         # run the C++ unit tests
make release      # optimized build
python3 tools/lang_test.py -d tests/lang_tests   # Orlyscript language suite
```

Run `lang_test.py` from the repo root; it shells out to `orlyc`, whose paths
are repo-root-relative. Expect `156 passed / 2 xfail`.

## Comments and the TODO convention

The 2014-era codebase attached an empty `/* TODO */` doc-stub to nearly every
declaration — ~5,800 of them, pure noise. Those have been stripped, and CI now
keeps the tree clean. Two rules, enforced by
[`tools/maint/lint_todos.py`](tools/maint/lint_todos.py) (the `todo-lint` CI
job):

1. **No bare `/* TODO */` stubs.** A doc-comment should say something. Write a
   real comment — verified against the implementation, not guessed from the
   signature — or leave none.

2. **Every `TODO` references a tracked issue: `TODO(#1234)`.** A task worth
   remembering is worth an issue; a task not worth an issue is not worth a
   comment. Bare (`// TODO`), free-text (`// TODO: fix this later`), and
   untracked TODOs fail the lint. File an issue first, then reference it:

   ```cpp
   // TODO(#278): CompactOpemMap is a no-op; implement or remove the call.
   ```

Run the lint locally before pushing:

```sh
python3 tools/maint/lint_todos.py            # whole tree
python3 tools/maint/lint_todos.py --base origin/master   # only your new TODOs
```

Doc-stub cleanup tooling lives in
[`tools/maint/strip_todo_stubs.py`](tools/maint/strip_todo_stubs.py).

## Pull requests

Keep a PR to one concern. CI (`.github/workflows/ci.yml`) must be green:
build + unit tests, the release build, the Orlyscript language suite, the
end-to-end examples, and the `todo-lint` and ThreadSanitizer gates.

### CI build caches

Compiler snapshots are separate for each OS, architecture, compiler identity
and jhm configuration. Only successful master-push builds write: the debug,
release, ASan and TSan producers on x64, and debug/release producers on ARM.
Other CI jobs, pull requests and manual workflows restore only; there is no
fallback across configurations or architectures.

The bootstrap cache contains only `jhm` and `make_dep_file`, keyed by their
complete compiler-reported source/header dependencies (including system
headers), compiler/linker/runtime inputs and absolute source layout. It has
no fallback key. `make bootstrap` still runs nycr generation. Normal nightly
builds bypass both caches; sanitizers keep their compiler cache but rebuild
bootstrap seeds. A new cache namespace starts cold and warms on master.

## Releasing

Releases are cut by a maintainer. Versions follow [`CHANGELOG.md`](CHANGELOG.md);
the three npm packages and the Go client move together.

1. Fold the changelog fragments (`python3 tools/fold_changelog.py`) and move
   `[Unreleased]` under the new version heading. Keep an empty `[Unreleased]`
   with its `### Fixed` subheading, or the next fold breaks.
2. Bump `version` in `clients/ts`, `clients/mcp` and `clients/repl`
   (`package.json`), and the `go get ...@vX.Y.Z` line in
   [`clients/go/README.md`](clients/go/README.md). Merge that as a PR.
3. Tag `vX.Y.Z` on the merge commit and push it. `docker.yml` publishes the
   multi-arch image (`:vX.Y.Z` and `:latest`) on a `v*` tag; then
   `gh release create vX.Y.Z`.
4. Publish to npm from an interactive terminal: `tools/npm_publish.sh`
   (`--dry-run` first). Each publish needs an npm 2FA code. The script skips
   versions already on the registry and waits for the driver to resolve before
   building `orly-mcp` and `orly-repl`, so it is safe to rerun.
5. Tag the Go client. It is a nested module, so its tag carries the directory
   prefix and is separate from the repo tag:
   `git tag clients/go/vX.Y.Z <commit> && git push origin clients/go/vX.Y.Z`.
   Then check `go list -m github.com/orlyatomics/orly/clients/go@vX.Y.Z`
   resolves.
