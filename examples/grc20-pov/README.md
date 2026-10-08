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
./run-claims.sh  # claims, provenance and honest answers (see below)
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

## Claims, not triples (`claims.orly`)

The replay above answers "what is the current value?" with one value. A knowledge graph that pulls from many sources needs more than that: sources disagree, some know a value exists without knowing it, a reader trusts some sources and not others, and every statement should trace back to its evidence. [`claims.orly`](claims.orly) is a second package in this directory that models those needs in orlyscript, on the engine as it is today. [`claims_demo.py`](claims_demo.py) walks through it and checks every result, and `./run-claims.sh` runs it end to end (CI runs it in the `examples/*` job).

### The model

A statement is a **claim**: an entity, a property, a value, and the source that asserted it. The source is part of the key:

```
<['claim', entity, property, source]>::(claim_t)       the claim
<['cite',  entity, property, source]>::({cite_t})      its evidence
<['out',   entity, property, target, source]>::(edge_t)  a link claim
<['in',    target, property, entity, source]>::(edge_t)  ... its reverse
<['seen',  entity, source]>::(bool)                    who describes whom
<['prop',  property, entity, source]>::(bool)          who claims p of whom
<['num',   property, n, entity, source]>::(bool)       Number claims, by value
<['name',  text, entity, source]>::(bool)              name claims, by text
```

So when `wiki` and `sep` both say Socrates was born in 470 BC, that is two claims, each with its own confidence, time qualifier and citations. A source holds one claim per (entity, property) and revises it in place. Links are many-valued, so their target is part of the key too. The last four keys are indexes the write methods maintain with `or= true`, which is commutative, so concurrent sessions and batches can set the same index row without conflicting.

The values are typed:

```orly
value_t  is <| Text(str) | Number(int) | Unknown |>;
period_t is <| Always | Since(int) | Until(int) | Between(<{.since: int, .until: int}>) |>;
claim_t  is <{.value: value_t, .confidence: int, .period: period_t}>;
cite_t   is <{.work: str, .locator: str}>;
```

`Unknown` is a claim in its own right: "he was born, nobody recorded when". It is not the same as no claim.

### Honest answers

Every read takes the set of sources the reader trusts and returns one of five answers:

```orly
answer_t is <|
  NoClaim |                 /* no source claims anything             */
  NotSelected({str}) |      /* only these unselected sources do      */
  UnknownValue({str}) |     /* these selected sources: a value exists, not known */
  Agreed(position_t) |      /* selected sources agree on one value   */
  Conflict([position_t])    /* ... or don't: every position, sorted  */
|>;
position_t is <{.value: value_t, .sources: {str}}>;
```

A conflict is never resolved by picking one claim. It comes back as every position with the sources behind it, and the application decides. The whole decision is one orlyscript expression, `decide`, over the claims a range read collects:

```orly
decide = ((answer_t.NoClaim() if length_of all == 0 else
          (answer_t.NotSelected(**all union_map {that.source}) if length_of selected == 0 else
          (answer_t.UnknownValue(**selected union_map {that.source}) if length_of definite == 0 else
          (answer_t.Agreed(position(.value: top, .selected: definite)) if length_of values == 1 else
           answer_t.Conflict(position(.value: **values, .selected: definite) as [position_t])))))) where {
  selected = (**all if that.source in trusted) as [claim_view_t];
  definite = (**selected if not is_unknown(.value: that.value)) as [claim_view_t];
  values   = **definite union_map {that.value};
  ...
};
```

### Methods

| Shape | Method | Returns |
|---|---|---|
| raw cross-source record | `claims(.entity, .property)` | every source's claim, ordered by source, each with its confidence, period and evidence |
| honest answer | `answer(.entity, .property, .trusted)` | `answer_t` |
| answer at a time | `answer_at(.entity, .property, .trusted, .year)` | `answer_t`, counting only claims whose period holds in `year` |
| point read by id | `describe(.entity, .trusted)` | every property any source claims, each with its answer, and every source that describes the entity |
| one-hop neighbours | `neighbours(.entity, .trusted)`, `neighbours_at(..., .year)` | links in both directions with the sources behind each, and a count of link claims left out because their source isn't trusted |
| property range page | `range_page(.property, .lo, .hi, .after_n, .after_entity, .after_source, .limit, .trusted)` | up to `limit` Number claims in `[lo, hi]` after a keyset cursor |
| cross-source listing | `catalogue(.trusted)` | every entity, the sources that describe it, and its label |
| cross-source listing | `disputes(.property, .trusted)` | every entity on which the trusted sources disagree about `property` |
| name lookup | `named(.text)` | candidate ids with the sources that use that name; never merged |
| writes | `assert_text` / `assert_number` / `assert_unknown`, `link`, `cite` | whether a claim was replaced; `cite` returns false, and writes nothing, for a claim that doesn't exist |

The typed writes `assert_claim(.value: value_t, .period: period_t)` and `assert_link(.period: period_t)` are what package code and the inline tests use. A client can't send a variant as an argument yet ([#816](https://github.com/orlyatomics/orly/issues/816)), so `assert_text`, `assert_number`, `assert_unknown` and `link` take flat arguments and the period as two optional years (`.since: -387?`, `.until: unknown int`), and build the typed value in orlyscript.

**Identity is the entity id.** Every key starts from an id, and names are claims like any other. `dnb` calls Plato "Platon", and its claims still join Plato's because they use the id `plato`. Two different people named "Aristotle" have two ids and stay two entities: `named` returns both as candidates and never merges them.

**Keyset paging.** `range_page`'s cursor is the last row's key (value, entity, source), not an offset, so a page never re-reads earlier rows into its result and stays correct while writes land. Today the walk still starts at the property's first index row and filters up to the cursor. [PR #793](https://github.com/orlyatomics/orly/pull/793) (`keys ... after <key>`, not merged yet) makes the cursor the walk's starting point, so a deep page costs what the first does. With it, `range_page` starts its walk with `after <['num', property, after_n, after_entity, after_source]>` and drops the cursor comparison from its filter; its signature and results stay the same.

### A session

From `./run-claims.sh` (abridged):

```
=== 1. provenance: every source's claim, side by side ===
  socrates.born  blog  469 BC   confidence 30%  evidence: -
  socrates.born  sep   470 BC   confidence 85%  evidence: Diogenes Laertius, Lives of Eminent Philosophers II.44
  socrates.born  wiki  470 BC   confidence 80%  evidence: -

=== 2. honest answers, by who you trust ===
  socrates.born trusting ['sep', 'wiki']: 470 BC (agreed by ['sep', 'wiki'])
  socrates.born trusting ['blog', 'dnb', 'sep', 'wiki']: CONFLICT: 470 BC ['sep', 'wiki'] vs 469 BC ['blog']
  plato.born trusting ['sep', 'wiki']: CONFLICT: 428 BC ['wiki'] vs 427 BC ['sep']
  socrates.school trusting ['wiki']: not selected (only ['blog', 'sep'] claim it)
  aristotle-of-cyrene.born trusting ['sep', 'wiki']: unknown value (per ['sep'])
  aristotle-of-cyrene.died trusting ['blog', 'dnb', 'sep', 'wiki']: no claim

=== 3. time qualifiers: who led the Academy? ===
  ever:        CONFLICT: 'Plato' ['wiki'] vs 'Speusippus' ['sep']
  in 350 BC: 'Plato' (agreed by ['wiki'])
  in 340 BC: 'Speusippus' (agreed by ['sep'])

=== 4. identity is the id, never the name ===
  'Aristotle' -> aristotle (named so by ['sep', 'wiki'])
  'Aristotle' -> aristotle-of-cyrene (named so by ['wiki'])
  plato's label for ['sep', 'wiki']: Plato
  plato's label for ['dnb']: Platon

=== 6. one-hop neighbours ===
  plato -> head_of    academy     ['wiki']
  plato -> student_of socrates    ['sep', 'wiki']
  plato <- nephew_of  speusippus  ['sep']
  plato <- student_of aristotle   ['sep', 'wiki']

=== 7. range page over a property, keyset cursor ===
  page 1: socrates=-470 (sep), socrates=-470 (wiki), socrates=-469 (blog)
  page 2: plato=-428 (dnb), plato=-428 (wiki), plato=-427 (sep)
  page 3: speusippus=-408 (wiki)

=== 9. a source revises its claim ===
  socrates.born trusting everyone: 470 BC (agreed by ['blog', 'sep', 'wiki'])
```

The corpus is loaded by two sessions writing concurrently into one shared POV, each as one mixed batch (`call_many`). `assert_claim` writes under an `if`, and a batch replays each of its calls when it is promoted ([#751](https://github.com/orlyatomics/orly/issues/751)), so no claim is lost. The 66 inline tests in `claims.orly` run whenever it compiles, and the lang_test suite compiles it as `tests/lang_tests/samples/kg_claims.orly`.

### Limits, and what today's engine shaped

- **One claim per (entity, property, source).** A source that wants to assert several values for one property (two spouses, say) needs the value in the key, as links have their target. Links are the many-valued case here.
- **Revisions leave old index rows behind.** When a source revises a number or a name, the old `num` / `name` row stays. `range_page` and `named` check each row against the claim and skip the stale ones, so results are right, but the rows are still walked.
- **Result types are kept shallow.** `orlyc` names a generated header after a type's full mangled name, so a record that nests a variant of records can exceed the file-name limit ([#815](https://github.com/orlyatomics/orly/issues/815)). That is why an answer carries positions as (value, sources) and the detail of each claim (confidence, period, evidence) comes from `claims`.
- **The upsert repeats its index statements.** `assert_claim` creates or replaces the claim in two expression-level arms (`... if *<[...]>::(claim_t?) is known else ...`), and both arms set the index rows. An `if` inside one effect block would read better, but its untaken assignment arm fails on a first claim ([#817](https://github.com/orlyatomics/orly/issues/817)), and moving the index statements to an outer effect block around the arms applies the arms' effects twice ([#818](https://github.com/orlyatomics/orly/issues/818)).
- **Index inputs are computed in `where`.** A key inside an effect block that calls a function on a variant-typed name doesn't compile yet ([#814](https://github.com/orlyatomics/orly/issues/814)), so `assert_claim` computes the index value before the effect block.
- **Reading your own revision.** A revision overwrites a claim the global POV may already hold. Until [#791](https://github.com/orlyatomics/orly/issues/791) is fixed (PR #796), a POV may not see its own overwrite of such a key until the write is promoted, so the demo waits for the revision to show before checking it.
- **No authorization.** "Trusted sources" is a read filter the caller chooses, not access control. Anyone who can call the package can assert as any source; see [docs/PROTOCOL.md](../../docs/PROTOCOL.md#security-and-trust-model).

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
| `claims.orly` | The claims package: claim-level provenance, evidence, qualifiers, honest answers, and the read shapes above, with 66 inline tests |
| `claims_demo.py` | Python driver for `claims.orly`: loads the corpus from two concurrent sessions, walks every read shape, checks every result |
| `run-claims.sh` | End-to-end smoke for the claims package: compile, start a fresh orlyi, run `claims_demo.py` |
