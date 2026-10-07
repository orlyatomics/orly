# A first-time walkthrough

This document walks through compiling an Orlyscript package with `orlyc`, loading it into a running `orlyi`, and calling a method on it from `orly_client`. It exists because piecing this together from `--help` text alone is non-obvious — the protocol grammar is documented as a Nycr file, the server requires four flags before it'll even start, and the client has its own little command language.

Prerequisites: a working `make debug` (see the README quick-start). The debug binaries are what we'll use throughout; `orlyc` invoked through the release tree should also work but the debug tree is what the test suite exercises and what gets a fresh rebuild on every PR.

All paths below assume your repo is at `/home/you/orly/`; adapt as needed. The build output tree lives at `../out_orly/debug/` relative to the repo root.

## The .orly file

There are ~120 sample programs under `tests/lang_tests/general/` and `tests/lang_tests/samples/` that are good first targets. For this walkthrough we'll use `tests/lang_tests/samples/fizzbuzz.orly`, the simplest interesting package:

```orly
package #1;

fizz = (("fizzbuzz" if x % 15 == 0
  else ("fizz" if x % 3 == 0
    else ("buzz" if x % 5 == 0
      else x as str)
  )
)) where {
  x = given::(int);
};

test {
  t1: fizz(.x:[1..15]) as [str] == ["1", "2", "fizz", "4", "buzz", "fizz", "7", "8", "fizz", "buzz", "11", "fizz", "13", "14", "fizzbuzz"];
};
```

Two things to notice:
- `package #1;` declares the package name as `fizzbuzz` (taken from the filename) and the version as `1`. When the server loads this package it'll be addressable as `fizzbuzz.1`.
- The `test { }` block runs at compile time, not at server time. `orlyc` will compile, then run those tests, and refuse to produce a `.so` if they fail.

### Replacing a stored record

Use whole-value assignment (`<-`) to replace an existing record, just as for
a scalar. Use `new` only when creating the key:

```orly
p_t is <{.a: str, .b: int}>;
put = (((true) effecting {
  *<['k', k]>::(p_t) <- v;
} if *<['k', k]>::(p_t?) is known else (true) effecting {
  new <['k', k]> <- v;
})) where {
  k = given::(str);
  v = given::(p_t);
};
```

Calling `put(.k: "x", .v: <{.a: "one", .b: 1}>)` and then
`put(.k: "x", .v: <{.a: "two", .b: 2}>)` replaces both fields. Assignment
also replaces nested records. To change just one field, assign to that field,
for example `*<['k', "x"]>::(p_t).a <- "two";` inside an effect block.

Do not use `delete (p_t) @ <['k', k]>;` followed by `new <['k', k]> <- v;`
in the same effect block. Effects on the same key must combine into one
compatible update; a deletion cannot combine with an insertion and reports
“Conflicting updates.” Assignment is a single replacement update, not a
commutative merge like `+=` or `|=`.

### Paging through keys

`keys (T) @ <[pattern]>` walks every stored key that matches the pattern, in
index order. To read it a page at a time, start each page after the last key
of the previous one (keyset paging, #735):

```orly
/* The page of at most `n` edges of group `g` after edge `last`, and the
   `last` to pass for the next page. Start with a `last` below every edge. */
page = (<{.rows: rows, .last: (rows[length_of rows - 1].e if length_of rows > 0 else last)}>) where {
  g = given::(int);
  last = given::(int);
  n = given::(int);
  rows = row(.k: keys (int) @ <['edge', g, free::(int)]> after <['edge', g, last]> take n) as [<{.e: int, .w: int}>];
};
row = (<{.e: k.2, .w: *k::(int)}>) where {
  k = given::(<[str, int, int]>);
};
```

- `after <[key]>` starts the walk just past `key`; `from <[key]>` starts at it.
  The bound need not be a stored key, and one outside the pattern's range gives
  the whole range or nothing, never keys the pattern doesn't match.
- The bound is a whole key with the pattern's shape: the same member types,
  `desc` members included. Any other type is a compile error that names both.
- The walk seeks to the bound, so a page costs the rows it reads wherever it
  starts. `skip s take n` costs `s + n`: it reads and throws away every row
  before the page, and a deep enough page is refused by the per-read budget
  (`read_too_large`) where the keyset page answers.
- The bound binds tighter than the operators after it, so
  `keys ... after b take n`, `... if <pred>`, `reverse_of`, `sorted_by` and the
  rest apply to the bounded walk.
- Paging follows index order. To page downward, store that key member `desc`
  (`<['edge', g, desc e]>`) and walk `desc free::(int)`. `reverse_of` reverses a
  page after it is read; it doesn't page from the end.
- `after` is a reserved word from this release on.

The server keeps no state between pages. A method returns its rows and the
cursor, and the caller passes the cursor back. The TypeScript, Python and Go
clients have a helper that does this: `pages` in TypeScript (an async
iterator) and Python (a generator), and `Pages` in Go (a callback).

Each page reads the data as it is when that page runs. A row written or
deleted before the boundary (the cursor) is not seen by later pages, and one
after it is. A private POV that reads but doesn't write is no snapshot either:
each page sees its parent POV exactly as the parent sees itself at that
moment, including the parent's writes made after the first page and not yet
promoted, and other POVs' writes once Tetris has promoted them to an ancestor
(checked by `clients/smoke/run-keyset-paging.sh`). One exception, which is a
bug: a POV's delete of a key that an ancestor holds isn't seen, by the POV or
its children, until the delete is promoted
([#791](https://github.com/orlyatomics/orly/issues/791)). For a fixed view,
read every page in one call, or keep a version in the key and page over that.

## Compile to a `.so`

`orlyc` produces a `.cc` then shells out to `g++` to build it into a shared library. Use the `--debug` flag during a first try — release-mode optimisations can mask things and aren't faster end-to-end on a single small package.

```sh
mkdir /tmp/orly_pkgs
../out_orly/debug/orly/orlyc --debug tests/lang_tests/samples/fizzbuzz.orly -o /tmp/orly_pkgs
```

Output looks like:

```
MM_NOTICE: Synth + Symbols
MM_NOTICE: Code Gen
MM_NOTICE: Compiling C++
MM_NOTICE: Running tests
MM_NOTICE: Tests done
```

After this `/tmp/orly_pkgs/` contains:

| File | What it is |
|---|---|
| `fizzbuzz.orly.sig` | A signature/hash of the source — used by the server to identify the package |
| `fizzbuzz.h`, `fizzbuzz.cc`, `fizzbuzz.link.cc` | Intermediate C++ artifacts; safe to ignore |
| `fizzbuzz.1.so` | The thing the server actually loads. `1` here is the version from `package #1;` |

## Start the server

`orlyi` is designed to manage a real disk-backed instance, which is heavier than you usually want for a first try. The `--mem_sim` flag tells it to keep everything in process memory, which is the right mode for exploration:

```sh
../out_orly/debug/orly/server/orlyi \
  --instance_name=hello \
  --starting_state=SOLO \
  --create \
  --mem_sim \
  --no_realtime \
  --package_dir=/tmp/orly_pkgs &
```

What each flag does:

| Flag | Purpose |
|---|---|
| `--instance_name=hello` | Required. Names this instance; used for log lines and on-disk paths if you weren't in mem_sim mode |
| `--starting_state=SOLO` | Required. SOLO means no replication; the alternative is SLAVE for follower mode |
| `--create` | Required (yes, even in mem_sim). Says "create a fresh state" rather than "load existing" |
| `--mem_sim` | Run entirely from memory, no disk volumes needed |
| `--no_realtime` | Don't try to acquire SCHED_FIFO priority. Required when running as a non-root user |
| `--package_dir=...` | Where to look for `.so` packages when `install` is called |

About 5-10 seconds after launch, three ports come up:

| Port | Purpose |
|---|---|
| 19380 | Native client protocol (what `orly_client` connects to) |
| 19388 | HTTP status/reporting endpoint |
| 8082 | WebSocket protocol |

You can confirm with `ss -tln | grep -E "19380|19388|8082"`.

## Connect with the client

```sh
../out_orly/debug/orly/client/orly_client
```

`orly_client` is a small interactive REPL. It opens a TCP connection to `127.0.0.1:19380`, **automatically creates a session for you**, and reads statements from stdin. Each statement ends with a semicolon. Send `exit;` (or close stdin) to leave; the session ID and TTL are printed on exit so you could resume it from another connection with `--session_id=...`.

## Install the package and make a point-of-view

`install` takes the name and version. POVs (points of view) are Orly's transaction/branch concept — to write to or invoke methods on a package, you need a POV to do it in.

```
install fizzbuzz.1;
new safe private pov;
```

`new safe private pov;` prints something like:

```
pov_id = 3210f457-a5a7-9129-5310-282ffa10c160
```

`safe` vs `fast` is a consistency guarantee setting; `private` vs `shared` is whether other sessions can see changes made through this POV. For a single-session exploration, `safe private` is the right default.

## Call a method

`try` is the verb for invoking a method against a POV. The grammar is:

```
try {<pov_id>} <package_path> <method_name> <args>;
```

Args is an Orlyscript object literal `<{.field: value, .field: value}>`. For `fizz(.x: int)`:

```
try {3210f457-a5a7-9129-5310-282ffa10c160} fizzbuzz fizz <{.x: 7}>;
```

Returns `"7"`. Try 10 → `"buzz"`, 15 → `"fizzbuzz"`.

## Cleanup

```sh
kill -INT $(pgrep -f "orlyi --instance_name=hello")
```

It's normal to see a few `FATAL ERROR: Fiber Runner caught exception` lines on shutdown — those are long-blocking IO fibers being interrupted by the signal, not real failures.

## Going further

- **More packages to play with**: `tests/lang_tests/general/` has ~90 small example programs covering different language features (filter, sort, reduce, maps, sets, etc.). Most have inline `test { }` blocks that double as documentation.
- **The full client grammar** lives in `orly/client/program/program.nycr`. The interesting verbs are `install`, `uninstall`, `new ... pov`, `try`, `echo`, `compile`, `list_packages`, `set ttl`, `pause`, `unpause`, `import`.
- **Snapshot-based regression testing**: `python3 tools/lang_test.py -d orly/data tests/lang_tests` compiles every `.orly` file and compares the output against stored `.test.state` snapshots. Pass `--update` to refresh snapshots after a deliberate change.
- **What's failing today**: 3 lang tests have always-failed snapshots and one is a `Not implemented` from the compiler; `python3 tools/lang_test.py` flags these. See [#10](https://github.com/orlyatomics/orly/issues/10) for the broader status of the 2026 revival pass.

## Troubleshooting

- **`orlyi` won't start, complains about realtime priorities** — add `--no_realtime`. The default tries `SCHED_FIFO` which needs root or `CAP_SYS_NICE`.
- **`orlyc` errors with `if constexpr only available with -std=c++17`** — your `orlyc` binary predates [#11](https://github.com/orlyatomics/orly/pull/11). Run `make debug` to rebuild.
- **`'install fizzbuzz' syntax error, expecting Dot`** — the install grammar requires a version: `install fizzbuzz.1;` not `install fizzbuzz;`.
- **`exception ... TNotFound`** during a `try` — most often the package isn't installed in this session. `install fizzbuzz.1;` first.
- **`orly_client` returns `(no session)` on startup but seems to accept commands** — that's fine; it auto-creates a session on the first statement. The session ID is printed when you `exit;`.
