# GRC-20-shaped knowledge graph on Orly

A small reference demo aimed at the [Geo / The Graph](https://github.com/geobrowser/grc-20) team: their **GRC-20** standard models knowledge as an event-sourced property graph (`CreateEntity` / `UpdateEntity` / `CreateRelation` / `DeleteEntity` ops, append-only). Orly stores exactly that shape natively — every op is one set-union into a per-(entity, property) history set, and concurrent editors stream into the same shared POV with **no coordination**.

```
GRC-20 op             →   one `|=` into  <['hist', e, p]>::({<{.ts,.editor,.kind,.val}>})
Reconstruct entity    ←   sort history by .ts, replay events in order
Time-travel as-of T   ←   sort history by .ts, replay events with .ts ≤ T
```

The driver owns the GRC-20 op vocabulary (`CreateEntity`, `SetText`, `SetInteger`, `CreateRelation`, `DeleteEntity`); the engine just stores immutable event records. Each event is a first-class `<{.ts: int, .editor: str, .kind: str, .val: str}>` record, and the history is a set OF those records ([issue #90](https://github.com/orlyatomics/orly/issues/90)) — no string packing, the four typed fields travel end to end. Three commutative orlyscript functions are the entire schema (`append_op`, `register_entity`, `register_prop`).

## Run it

```sh
cd examples/grc20-pov
./run.sh       # python driver
./run-go.sh    # go driver
```

Both wrappers require `make debug` to have built `orlyi` + `orlyc`. The Go wrapper additionally needs the `go` toolchain. The two drivers exercise the same scenario and self-check the same invariants.

## The three phases

The demo runs a small corpus of six Greek philosophers as if they were being collaboratively edited by two GRC-20 sources:

| Phase | Editor | Ops | What |
|---|---|---|---|
| 1 | `wiki` | 24 | `CreateEntity` + `SetText name` + `SetInteger born` + `SetInteger died` per philosopher |
| 2 | `stanford` | 8 | `SetText school` per philosopher + `CreateRelation student_of` (Plato→Socrates, Aristotle→Plato) |
| 3 | both | 2 | both editors **concurrently** rewrite `pythagoras.born` from separate WebSocket sessions |

After each phase a snapshot is printed, plus a final editorial diff (events per editor, overlap).

## What you'll see

Snapshot 1 (after phase 1) — only `wiki`'s biographical facts:

```
=== snapshot 1: end of phase 1 (wiki only) ===
  aristotle (Person): born=-384 [wiki], died=-322 [wiki], name=Aristotle [wiki]
  epicurus (Person):  born=-341 [wiki], died=-270 [wiki], name=Epicurus  [wiki]
  ...
```

Snapshot 2 (after phase 2) — `stanford` has merged in:

```
=== snapshot 2: end of phase 2 (wiki + stanford) ===
  aristotle (Person): born=-384 [wiki], died=-322 [wiki], name=Aristotle [wiki],
                      school=Peripateticism [stanford], student_of=→ plato [stanford]
  ...
```

Phase 3 — the race:

```
[phase 3] wiki + stanford concurrently overwrite pythagoras.born
  history for pythagoras.born after race (3 events, ts-sorted):
    1780613758237  wiki       I  -570
    1780613758647  stanford   I  -575
    1780613758647  wiki       I  -570
```

Two writers from independent WebSocket sessions land events at the **same millisecond**; both events survive in history. The driver's deterministic tiebreaker (lex sort on the event record's `(.ts, .editor, .kind, .val)`) picks one as the "current" value — but the alternative-source claim isn't lost, exactly the editorial-workflow semantics GRC-20 needs.

```
=== editorial diff ===
  stanford     9 events on  6 entities
  wiki        25 events on  6 entities
  overlap: 6 entities edited by both (stanford ∩ wiki): ['aristotle', 'epicurus', ...]
```

## Why this maps to GRC-20

| GRC-20 concept | Demo realisation |
|---|---|
| `CreateEntity(id, type)` | `register_entity` + `append_op(id, '__type', .kind:'L', .val:<type>)` |
| `SetText(id, prop, text)` | `append_op(id, prop, .kind:'L', .val:<text>)` |
| `SetInteger(id, prop, n)` | `append_op(id, prop, .kind:'I', .val:<n>)` |
| `CreateRelation(id, prop, target)` | `append_op(id, prop, .kind:'R', .val:<target_id>)` |
| `DeleteEntity(id)` | `append_op(id, '__deleted', .kind:'D', .val:'')` |
| event-sourced log | history set sorted by the `.ts` field |
| append-only | set union is the only write operation |
| multi-editor merge | concurrent `|=` from independent WS sessions, no locks |
| historical query | replay events with `.ts ≤ target` |

GRC-20's full type system is 13 data types; this demo exercises TEXT, INTEGER, and RELATION (sufficient to prove the pattern). Each event is a typed `<{.ts: int, .editor: str, .kind: str, .val: str}>` record — `kind` (`L`/`I`/`R`/`D`) discriminates how the driver interprets `val`, the same role GRC-20's op tag plays. A production binding would store binary GRC-20 ops directly.

## Schema (the entire engine side)

```orly
<['hist', entity, property]>::({<{.ts: int, .editor: str, .kind: str, .val: str}>})
                                       -- set of event records for one (e, p)
<['entities']>::({str})                -- set of every entity id
<['props',  entity]>::({str})          -- set of every property ever written on e
```

Three functions:

```orly
append_op(.entity, .property, .ts, .editor, .kind, .val)
                                        -- |= one event record into the history
register_entity(.entity)                -- |= entity into the global set
register_prop(.entity, .property)       -- |= property into the entity's prop set
```

That's it. Everything else — op classification, replay, time-travel, editorial diff — happens in the driver. The engine guarantees that no event is ever lost, no matter how many editors are writing at once, because `|=` is a commutative deferred-mutation field call ([#49](https://github.com/orlyatomics/orly/issues/49) / PRs #50/#51/#52); storable record-set values are [#90](https://github.com/orlyatomics/orly/issues/90).

## Caveats

- **Driver-side ms timestamps** for the event timeline assume all editors share the same clock. In a real decentralised deployment, replace with a consensus-ordered sequence number or a hybrid logical clock; the schema doesn't change.
- **Same-ms tiebreaker** is lex order on the event record's `(.ts, .editor, .kind, .val)` — deterministic, not principled. GRC-20 itself defers ordering to the on-chain proposal queue, which the driver would defer to.
- **No compaction** of the history set in this demo. Real workloads would compact `(entity, prop)` history into a checkpoint past some age cutoff. The shape generalises — fold the prefix, keep only the tail.
- **Six philosophers** is enough to prove the pattern, not a workload benchmark. For throughput numbers under contention, see [`examples/agent-swarm/`](../agent-swarm/) (8 concurrent agents, hot-key counters + tag sets) and [`examples/wikipedia-pageviews/`](../wikipedia-pageviews/) (5,938 events, integer counters).

## Related demos

- [`examples/wikipedia-categories/`](../wikipedia-categories/) — same fold-on-read pattern with set-union values, version axis in the key (year). This demo extends to heterogeneous events with timestamped editor attribution.
- [`examples/agent-swarm/`](../agent-swarm/) — multi-writer commutative field calls (`+=`, `|=`) under contention, same lost-update-free guarantee.
- [`examples/bitcoin-time-travel/`](../bitcoin-time-travel/) — the original time-travel-via-key-axis demo with integer-addition values (account balances).

## Files

| File | What |
|---|---|
| `grc20.orly` | The Orlyscript package: `append_op`, `register_entity`, `register_prop`, + lookups + 19 inline tests |
| `demo.py` | Python WebSocket driver: 3 phases, snapshot replay, editorial diff, self-check |
| `demo.go` + `go.mod` + `go.sum` | Go WebSocket driver, equivalent to Python |
| `run.sh` / `run-go.sh` | End-to-end wrappers: compile, start fresh orlyi, run the chosen driver |
