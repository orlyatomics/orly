# Bitcoin time-travel + multiverse

A small example showing that Orly can serve **time-travel queries**
("what was alice's balance at block 5?") and **branched-history
multiverse queries** ("alice's balance on the fork vs the mainnet at
block 7") entirely in user-space, without engine changes.

The trick is to encode both the version axis (block height) and the
branch axis into the **key tuple**, not into Points of View. Writes
to different tuples never collide, so the past is structurally frozen
— nothing ever overwrites it.

## Run it

Two equivalent drivers ship -- Python and Go. Both do the same
scenario and self-check the same 72 balance values; pick whichever
your environment has.

```sh
cd examples/bitcoin-time-travel
./run.sh       # python driver (requires `pip install websocket-client`)
./run-go.sh    # go driver    (requires the `go` toolchain on PATH)
```

Either wrapper requires `make debug` to have produced `orlyi` and
`orlyc`. Both kill any prior demo instance and run against a fresh
`orlyi`, so they're reproducible end to end.

Both drivers are exercised in CI on every push.

## What the demo does

1. Applies 8 blocks of mainnet activity (credits, transfers).
2. At height 6, forks a `fork` branch from the mainnet — modelling
   a Bitcoin chain reorg where a different miner won the block 6
   race and gave the coinbase to bob instead of alice.
3. Applies 3 alternate blocks on the fork.
4. Queries balances on both branches at every height, prints both
   chains side by side, and self-checks every value.

Truncated sample output:

```
=== mainnet vs fork, per height (* = post-fork) ===
  h     mainnet alice  fork alice    mainnet bob   fork bob
  ----------------------------------------------------------
     0         0.00           0.00           0.00           0.00
     1        50.00          50.00           0.00           0.00
     ...
     5        35.00          35.00           7.00           7.00      (last common)
   * 6        85.00          35.00           7.00          57.00      (diverge)
   * 7        83.00          55.00           7.00          37.00
   * 8        82.00          50.00           7.00          37.00
```

Rows 0–5 are bit-identical across the two branches because reads on
`fork` for `h < 6` recurse through `<['parent', 'fork']>` into
mainnet's keyspace. Rows 6–8 differ because they're reading
fork-local writes.

## How it works

### Schema

| Key | Value | Meaning |
|---|---|---|
| `<['delta', branch, addr, h]>` | int | The per-block delta to apply at this (branch, addr, height) tuple |
| `<['parent', branch]>` | str | This branch's parent, if forked |
| `<['fork_h', branch]>` | int | The height at which this branch forked from its parent |

### Reads

```orly
delta_at(branch, addr, h) =
  if branch has a parent AND h < fork_h(branch):
    delta_at(parent(branch), addr, h)          # recurse into parent
  else:
    *<['delta', branch, addr, h]>::(int) ?? 0  # local lookup

balance_at(branch, addr, h) =
  [0..h] reduce start 0 + delta_at(branch, addr, that)
```

That's the whole engine. `balance_at` is a fold over the height axis;
`delta_at` is a one-step recursion through the branch tree.

### Why the past is frozen

Each block writes to a unique key tuple `(branch, addr, h)`. Once
written, the tuple is never written again — because future blocks
have different `h`, and other branches have different `branch`. So a
read at any historical `(branch, addr, h)` returns the same value
forever, even after arbitrary later activity on any branch.

This sidesteps Orly's actual POV semantics, where writes in a child
POV propagate up to the parent / global immediately, making the
POV chain useless as a snapshot mechanism.

## Why this is interesting

### One pattern, many domains

The same fold-over-keyed-versions pattern works for any commutative
monoid:

| Domain | Fold op | Identity |
|---|---|---|
| Balances / counters | `+` | `0` |
| Set membership | `union` | `{}` |
| List append | `++` | `[]` |
| Min / max | `min` / `max` | `+∞` / `-∞` |
| Last-write-wins | take last | undefined |

Swap the fold operator and the identity; you get historical queries
on sets, lists, extrema, etc. for free.

### What this exploits about Orly specifically

- **Tuple-structured keys** as first-class indexes (`<['delta',
  branch, addr, h]>`). Most KV stores would force you to mangle a
  string.
- **Per-key immutability via uniqueness.** Different tuples never
  collide; nothing has to be "snapshotted" because nothing is ever
  overwritten.
- **Orlyscript recursion + short-circuit booleans** (`and_then`).
  Lets `delta_at` defer to its parent branch in a single 4-line
  function.

### Comparison to a typical graph DB

Modelling this in Cypher / Neo4j needs:

- A `:Block` node per height with `(:Block)-[:NEXT]->(:Block)` edges.
- A `:Branch {name}` node per timeline with a `[:HEAD]` edge.
- Each transaction as a `(:Tx)-[:IN]->(:Block)` with `[:CREDITS]` /
  `[:DEBITS]` rels to address nodes.
- A path-walking Cypher query per question: "balance for alice on
  fork at height 5" becomes a non-trivial chain traversal with
  aggregation, redesigned for every domain (sets need a different
  query than balances).

In Orly the branch and height are just key coordinates, and the
balance / set / list / min-max question is the choice of fold
operator. One small Orlyscript helper per type, four-line read,
domain-agnostic shape.

## Caveats

- **Write amplification.** Every state change costs a fresh key.
  For a real Bitcoin-scale chain (~1B txns × N branches) you'd add
  periodic snapshot keys (`<['snap', branch, addr, h_floor]>`
  every N blocks) and fold from the nearest snapshot.
- **Read amplification.** `balance_at(h)` folds over `[0..h]`.
  Same fix: snapshot keys, then fold from the nearest one.
- **Recursion depth.** A long fork chain (fork-of-fork-of-fork)
  costs one recursive call per ancestor at pre-fork heights. For
  shallow chains this is fine; for deep ones you'd flatten on
  fork by copying ancestors' keys into the new branch's namespace.
- **Not isomorphic to real Bitcoin.** This is account arithmetic,
  not UTXO output consumption. The pattern generalizes (UTXOs would
  be `<['utxo', branch, tx_id, idx]>` with a `spent` set keyed by
  `<['spent', branch, tx_id, idx]>`), but the demo uses balances
  for clarity.

## Files

| File | What |
|---|---|
| `bitcoin.orly` | The Orlyscript package: `delta_at`, `credit_at`, `balance_at`, `fork_from`, plus 13 inline tests |
| `demo.py` | Python WebSocket driver: applies the scenario, prints both chains, self-checks |
| `demo.go` + `go.mod` + `go.sum` | Go WebSocket driver, equivalent to the Python one; single dep on `github.com/gorilla/websocket` |
| `run.sh` | End-to-end wrapper: compiles, starts a fresh `orlyi`, runs `demo.py` |
| `run-go.sh` | Same wrapper, but runs `demo.go` instead |
