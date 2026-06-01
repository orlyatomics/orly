# Wikipedia categories: same trick, different monoid

A second worked example proving the polymorphic-monoid claim from
[`examples/bitcoin-time-travel/`](../bitcoin-time-travel/): the same
fold-over-keyed-versions pattern works for any commutative monoid.
The bitcoin demo used **integer addition** (balances). This one uses
**set union** (category membership).

## The point

> "Pick the operator and identity, get time travel for that domain. For free."

This demo is the receipt for that claim.

```
bitcoin:     [0..h]            reduce start 0               + delta_at(addr, that)
this demo:   [since..target]   reduce start (empty {str})   | growth_in(cat, that)
```

Same fold. Same key-encoded version axis. Same "the past is structurally
frozen because nobody writes the same tuple twice." Different type, different
operator.

## Run it

```sh
cd examples/wikipedia-categories
./run.sh       # python driver
./run-go.sh    # go driver
```

Both drivers exercise the same scenario and self-check the same snapshot
queries.

## What you'll see

The driver ingests a hand-curated timeline of when various programming
languages and cryptocurrencies first appeared (sourced from Wikipedia's
"first appeared" fields, rounded to year), then queries the category
membership *as of* a range of historical years. Truncated output:

```
=== Programming languages ===
  1957  ( 1): FORTRAN
  1965  ( 5): ALGOL, BASIC, COBOL, FORTRAN, Lisp
  1975  ( 8): ALGOL, BASIC, C, COBOL, FORTRAN, Lisp, Pascal, Smalltalk
  1985  (11): + Ada, C++, Objective-C
  1995  (21): + Erlang, Haskell, Java, JavaScript, Lua, PHP, Perl, Python, Ruby, Visual Basic
  2005  (24): + C#, Scala, Groovy
  2015  (33): + Clojure, Dart, Elixir, Go, Julia, Kotlin, Rust, Swift, TypeScript
  2024  (35): + Mojo, Zig
```

You can literally watch the set grow over time. The bitcoin demo prints a
number trajectory; this one prints a set trajectory. Same shape.

## Schema

| Key | Value |
|---|---|
| `<['cat_growth', cat, year]>` | `{str}` — the set of articles added to `cat` in `year` |

## Read function (the headline)

```orly
members_at = (
  [since..target] reduce start (empty {str}) | growth_in(.cat: cat, .year: that)
) where {
  cat    = given::(str);
  since  = given::(int);
  target = given::(int);
};
```

That's the entire engine. Compare to the bitcoin demo's `balance_at`:

```orly
balance_at = (
  [0..h] reduce start 0 + delta_at(.branch: branch, .addr: addr, .h: that)
) where { ... };
```

Identical structure; the `start` value and the binary operator are the only
differences.

## Why the past is frozen

Each `(cat, year)` tuple is written at most once by the driver (the driver
pre-aggregates if multiple events land in the same bucket). Once written,
the entry is immutable: future writes target different `(cat, year)`
tuples. Reading at any historical year returns the same answer forever.

This is the same property the bitcoin demo exploits, applied to a different
domain.

## Other monoids that fit this shape

| Domain | Value type | Operator | Identity |
|---|---|---|---|
| Counters (this is bitcoin) | `int` | `+` | `0` |
| **Set membership (this demo)** | `{T}` | `\|` | `empty {T}` |
| List append | `[T]` | `++` | `[]` |
| Min / max | `T` | `min` / `max` | `+∞` / `-∞` |
| Last-write-wins | `T?` | take the latest | unknown |

Implement each variant in ~80 lines of Orlyscript and you get historical
queries for that domain.

## Caveats

- **Add-only.** Real Wikipedia categories churn (articles get removed
  too). A removal-aware version would track a parallel `cat_removed`
  set and do `members_added - members_removed` at read time. Out of
  scope for the v1 demo, which is showing the polymorphic-monoid point,
  not modeling Wikipedia in full fidelity.
- **Year-resolution.** Wikipedia revision data is hour-resolution; this
  demo rounds to year for clarity. The pattern doesn't care about the
  resolution of the time axis -- substitute `(unix_epoch_hour)` for
  `(year)` and you get hourly precision at the cost of much more
  per-key overhead.
- **Hand-curated dataset.** Real timing comes from `dumps.wikimedia.org`
  category-link tables; the demo uses a small illustrative slice
  hand-typed in `demo.py` / `demo.go` for ease of reading.

## Files

| File | What |
|---|---|
| `wiki.orly` | The Orlyscript package: `growth_in`, `add_growth`, `members_at`, `count_at`, plus 20 inline tests |
| `demo.py` | Python WebSocket driver, ~150 lines |
| `demo.go` + `go.mod` + `go.sum` | Go WebSocket driver, equivalent to Python |
| `run.sh` / `run-go.sh` | End-to-end wrappers |
