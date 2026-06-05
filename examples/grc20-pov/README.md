# GRC-20-shaped knowledge graph on Orly

A small reference demo aimed at the [Geo / The Graph](https://github.com/geobrowser/grc-20) team: their **GRC-20** standard models knowledge as an event-sourced property graph (`CreateEntity` / `SetProperty` / `CreateRelation` / `DeleteEntity` ops, append-only). Orly stores exactly that shape natively — every op is one set-union into a per-(entity, property) history set, concurrent editors stream into the same shared POV with **no coordination**, and the replay that turns a log into the current graph runs **inside the engine**.

```
GRC-20 op             →   one `|=` into  <['hist', e, p]>::({<{.ts,.editor,.op}>})
Resolve property      ←   sort history by .ts, `reduce` to the latest, `when` over its op
Time-travel as-of T   ←   same, filtered to events with .ts ≤ T
```

The op is a **typed sum type**, not a string-packed `(kind, val)` pair:

```orly
op_t  is  <| Text(str) | Number(int) | Relation(str) | Deleted |>
evt_t is  <{.ts: int, .editor: str, .op: op_t}>
```

The history of one (entity, property) pair is a **set of these variant-bearing records** — storable because a set of differently-tagged variants is homogeneous on disk ([issue #96](https://github.com/orlyatomics/orly/issues/96)). The typed op travels end to end: the driver hands the engine a typed value and reads back a resolved typed value — no string packing, no driver-side `if kind == "L"` dispatch.

## What moved into the engine

This demo is the capstone of Orly's sum-types line ([#95](https://github.com/orlyatomics/orly/issues/95) / [#96](https://github.com/orlyatomics/orly/issues/96)). The replay — latest-write-wins, tombstones, value formatting, time-travel — used to live in the driver. It now lives in orlyscript, as a fold over the event set:

```orly
resolve_as_of = (((**((((**hist_for(.entity: entity, .property: property))
                        if (that.ts <= as_of))           /* time-travel cutoff   */
                       as [evt_t]) sorted_by lhs.ts < rhs.ts) /* chronological    */
                  ) reduce (start seed if false else that)    /* latest .ts wins  */
                 ).op);                                        /* the resolved op  */

display_as_of = ((op) when {                /* exhaustive match over the op kind */
  Text:     op.Text;
  Number:   (op.Number) as str;
  Relation: "-> " + op.Relation;
  Deleted:  "(absent)";                     /* tombstone (and empty history)     */
}) where { op = resolve_as_of(...); ... };
```

`reduce`'s seed is a `Deleted` sentinel, so an empty (or fully-filtered) history resolves to "absent" for free, and `when` is checked for exhaustiveness at compile time. **Compare `demo.py`'s `reconstruct` to [the previous version](https://github.com/orlyatomics/orly/commits/master/examples/grc20-pov/demo.py): the latest-wins loop, the `event_key` tiebreaker, and `format_value` are all gone** — the driver enumerates entities and asks the engine `display_as_of`.

## Run it

```sh
cd examples/grc20-pov
./run.sh       # python driver
./run-go.sh    # go driver
```

Both wrappers require `make debug` to have built `orlyi` + `orlyc`. The Go wrapper additionally needs the `go` toolchain. The two drivers exercise the same scenario and self-check the same invariants. Compiling `grc20.orly` also runs its inline test suite (40+ assertions over the resolve/display/tombstone/time-travel/lifecycle paths).

## The three phases

The demo runs a small corpus of six Greek philosophers as if collaboratively edited by two GRC-20 sources:

| Phase | Editor | What |
|---|---|---|
| 1 | `wiki` | `CreateEntity` + `SetText name` + `SetInteger born` + `SetInteger died` per philosopher |
| 2 | `stanford` | `SetText school` per philosopher + `CreateRelation student_of` (Plato→Socrates, Aristotle→Plato) |
| 3 | both | both editors **concurrently** rewrite `pythagoras.born` from separate WebSocket sessions |

After each phase a snapshot is printed — each one a **time-travel read** at that phase's `.ts` cutoff — plus a final editorial diff (events per editor, overlap).

## What you'll see

Snapshot 1 (after phase 1) — only `wiki`'s biographical facts, formatted by the engine:

```
=== snapshot 1: end of phase 1 (wiki only) ===
  aristotle (Person): born=-384, died=-322, name=Aristotle
  plato (Person): born=-428, died=-348, name=Plato
  ...
```

Snapshot 2 (after phase 2) — `stanford` has merged in; relations render via the `Relation` arm:

```
=== snapshot 2: end of phase 2 (wiki + stanford) ===
  aristotle (Person): born=-384, died=-322, name=Aristotle, school=Peripateticism, student_of=-> plato
  plato (Person): born=-428, died=-348, name=Plato, school=Platonism, student_of=-> socrates
  ...
```

Phase 3 — the race:

```
[phase 3] wiki + stanford concurrently overwrite pythagoras.born
  history for pythagoras.born after race (3 events, ts-sorted):
    1780700632132  wiki
    1780700632518  stanford
    1780700632518  wiki
  -> engine-resolved current value: pythagoras.born = -570
```

Two writers from independent WebSocket sessions land events (here even at the **same millisecond**); every event survives in history. The engine's `reduce` resolves the latest as the current value — the alternative-source claim isn't lost, exactly the editorial-workflow semantics GRC-20 needs.

```
=== editorial diff ===
  stanford     9 events on  6 entities
  wiki        25 events on  6 entities
  overlap: 6 entities edited by both (stanford ∩ wiki): ['aristotle', ...]
```

## Why this maps to GRC-20

| GRC-20 concept | Demo realisation |
|---|---|
| `CreateEntity(id, type)` | `create_entity(id, ts, editor, type)` → typed marker on `"__entity"` + catalogue |
| `SetText(id, prop, text)` | `set_text(...)` → `op_t.Text(text)` event |
| `SetInteger(id, prop, n)` | `set_number(...)` → `op_t.Number(n)` event |
| `CreateRelation(id, prop, target)` | `set_relation(...)` → `op_t.Relation(target)` event |
| `DeleteEntity(id)` | `delete_entity(...)` → `op_t.Deleted` on `"__entity"` |
| op type tag | the `op_t` variant arm — typed, exhaustively matched, not a string |
| event-sourced log | history set, replayed by `reduce` over the `.ts`-sorted events |
| append-only | set union is the only write operation |
| multi-editor merge | concurrent `|=` from independent WS sessions, no locks |
| current value | `reduce` to the latest `.ts`, `when` over the op |
| tombstone delete | the `Deleted` arm — `when` collapses it to absent |
| historical query | the same fold with an `.ts ≤ T` filter (`display_as_of`) |

GRC-20's full type system is 13 data types; this demo exercises TEXT, INTEGER, and RELATION (sufficient to prove the pattern), plus the tombstone. A production binding would add the remaining arms to `op_t` and store binary GRC-20 ops directly.

## Schema (the entire engine side)

```orly
op_t  is <| Text(str) | Number(int) | Relation(str) | Deleted |>;
evt_t is <{.ts: int, .editor: str, .op: op_t}>;

<['hist', entity, property]>::({evt_t})   -- set of typed event records for one (e, p)
<['entities']>::({str})                   -- set of every entity id
<['props',  entity]>::({str})             -- set of every property written on e
```

Writes are typed appends (`set_text` / `set_number` / `set_relation` / `clear_prop`, plus `create_entity` / `delete_entity`); reads are the in-engine replay (`resolve` / `display` / `is_present` / `entity_live`, each with an `_as_of` time-travel form). The engine guarantees no event is ever lost, no matter how many editors write at once, because `|=` is a commutative deferred-mutation field call ([#49](https://github.com/orlyatomics/orly/issues/49) / PRs #50/#51/#52); storable record sets are [#90](https://github.com/orlyatomics/orly/issues/90), storable variant sets [#96](https://github.com/orlyatomics/orly/issues/96).

## Caveats

- **Driver-side ms timestamps** for the event timeline assume all editors share the same clock. In a real decentralised deployment, replace with a consensus-ordered sequence number or a hybrid logical clock; the schema doesn't change (`.ts` is just an `int`).
- **Same-ms tiebreaker** falls out of the engine's total order on event records — deterministic, not principled. GRC-20 itself defers ordering to the on-chain proposal queue, which the driver would feed into `.ts`.
- **No compaction** of the history set in this demo. Real workloads would fold `(entity, prop)` history into a checkpoint past some age cutoff — the same `reduce`, kept on the tail.
- **Six philosophers** is enough to prove the pattern, not a workload benchmark. For throughput under contention, see [`examples/agent-swarm/`](../agent-swarm/) and [`examples/wikipedia-pageviews/`](../wikipedia-pageviews/).

## Related demos

- [`examples/wikipedia-categories/`](../wikipedia-categories/) — same fold-on-read pattern with set-union values, version axis in the key (year). This demo extends it to *heterogeneous typed* events with editor attribution and an in-engine `reduce`/`when` replay.
- [`examples/agent-swarm/`](../agent-swarm/) — multi-writer commutative field calls (`+=`, `|=`) under contention, same lost-update-free guarantee.
- [`examples/bitcoin-time-travel/`](../bitcoin-time-travel/) — the original time-travel-via-key-axis demo with integer-addition values (account balances).

## Files

| File | What |
|---|---|
| `grc20.orly` | The Orlyscript package: typed `op_t`/`evt_t`, typed appends, in-engine `resolve`/`display`/`is_present`/`entity_live` (+ `_as_of` forms), catalogue + counts, 40+ inline tests |
| `demo.py` | Python WebSocket driver: 3 phases, time-travel snapshots, editorial diff, self-check |
| `demo.go` + `go.mod` + `go.sum` | Go WebSocket driver, equivalent to Python |
| `run.sh` / `run-go.sh` | End-to-end wrappers: compile, start fresh orlyi, run the chosen driver |
