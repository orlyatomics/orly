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
