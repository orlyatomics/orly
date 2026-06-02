# Wikipedia hourly pageviews — concurrent `+=` demo

The motivating workload for [issue #49](https://github.com/orlyatomics/orly/issues/49). 8 concurrent WebSocket writers all do `*<['views', lang, page, hour]>::(int) += n` into the same hot keys; the demo self-checks that every increment lands.

## The trick

Orly's `+= n` is a field **call**, not a read-then-write. Two sessions concurrently doing `x += 5` against the same key each emit a deferred `{Add, 5}` mutation at the storage layer; the read path folds them via `Rt::Mutate` back into the correct sum. No row lock, no read-modify-write race, no lost updates.

Equivalent operations in other databases:

| Database | Statement | Notes |
|---|---|---|
| Postgres | `UPDATE views SET n = n + 1 WHERE page = ?` | Row lock; N writers serialize on hot pages. |
| Redis | `INCR views:<page>` | Atomic but single-threaded server. |
| Cassandra | `UPDATE views SET n = n + 1 WHERE ...` | Counter columns, with consistency caveats. |
| Orly | `*<['views', lang, page, hour]>::(int) += n` | Field call; lock-free; N writers in N sessions all land their increments. |

## Run it

```sh
cd examples/wikipedia-pageviews
./run.sh
```

The wrapper requires `make debug` to have produced `orlyi` and `orlyc`. It kills any prior demo instance, starts a fresh `orlyi`, and runs `demo.py` which spawns 8 concurrent WebSocket writers (250 events each = 2000 events total) hammering 96 hot keys (4 pages × 24 hours).

Exit code is non-zero if the verifier finds any per-key counter that disagrees with the independently-tracked ground truth.

## What the demo proves

- **No lost updates under contention.** Without the issue-#49 fix (phases 1–3), this demo recovers ~1/8 of the writes — each writer's reads against a stale view clobber the others when resolved-as-Assign writes hit storage. With the fix, all 5,938 events are accounted for.
- **The fold composes commutatively across POVs.** Concurrent writes go through Tetris merge from the shared POV to the global repo; the read path walks both layers and folds same-mutator runs back into a value.

## Related demos

- [`bitcoin-time-travel/`](../bitcoin-time-travel/) — same fold pattern over integer addition with multi-branch (multiverse) keys. Demonstrates user-space time-travel.
- [`wikipedia-categories/`](../wikipedia-categories/) — same fold pattern over set union. Demonstrates the polymorphic-monoid claim: the fold trick works for any commutative+associative operator.

This one demonstrates the third leg of that story: **the same `+= n` field-call works under concurrent writers**, which is the assumption the other two demos make about Orly's `+=` semantics but don't themselves exercise.
