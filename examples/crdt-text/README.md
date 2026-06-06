# Collaborative text editing as a CRDT — the database *is* the merge

A Google-Docs-style collaborative text editor where Orly's commutative storage **is** the conflict-free merge. Two editors stream inserts and deletes into one shared POV with **no coordination** — no operational transform, no central reconcile step, no lock — and converge to the same document. The driver owns exactly one algorithm; everything else is the engine.

```
keystroke        →   one `|=` of  <{.pos, .ch, .site, .clock}>  into  <['ins', doc]>::({...})
delete           →   one `|=` of  <{.pos, .clock}>              into  <['del', doc]>::({...})
visible text     ←   sort the inserts by position, drop tombstoned, concatenate
time-travel as T ←   the same fold over edits with .clock ≤ T
```

## The trick: a sequence is a set with dense position ids

The only hard part of a text CRDT is ordering concurrent inserts. The classic answer (this is a **Logoot** CRDT) is to give every character a **dense, totally-ordered position id**, so the document is an unordered *set* of `(position, char)` entries and the text is just "sort by position." A position is a list of digits — `[42, 318]` — and to insert between two neighbors you mint a position strictly between their ids. Because the id space is dense, there is always room; because concurrent inserts in the same gap get *different* ids (and ties break on `(site, clock)`), they can't collide and nothing is lost.

Orly stores the position as a list of zero-padded digit strings (`[42, 318]` → `["00042", "00318"]`) so that its **native list ordering** compares positions exactly the way Logoot needs — digit by digit, shorter/prefix id first. The engine never computes a position; it only sorts and folds them.

The one piece of real CRDT logic — `between(p, q)`, the dense-order allocation — lives in the driver (~12 lines). That's the honest division: **the algorithm is small and client-side; the merge is the database.**

## In-engine reconstruction (with time-travel)

`render_as_of(doc, as_of)` rebuilds the visible text at a logical clock: keep inserts with `.clock ≤ as_of`, sort by `(pos, site, clock)`, drop any position tombstoned by a delete with `.clock ≤ as_of`, concatenate. `render` / `visible` are the live (`as_of: forever`) forms. The whole thing is one `sorted_by` + `reduce` fold — the same machinery the [GRC-20 demo](../grc20-pov/) uses for an event log, here over a *sequence*. Time-travel (Google-Docs version history) falls out for free: every edit is an immutable set member, so the past is structurally frozen.

## Run it

```sh
cd examples/crdt-text
./run.sh       # python driver
./run-go.sh    # go driver
```

Both wrappers require `make debug` to have built `orlyi` + `orlyc`. The Go wrapper additionally needs the `go` toolchain. The two drivers run the same scenario and self-check the same invariants. Compiling `crdt_text.orly` also runs its inline test suite.

## The four phases

Two editors, `alice` and `bob`, edit one shared document from independent WebSocket sessions:

| Phase | What | Shows |
|---|---|---|
| 1 | `alice` types a base sentence (sequential) | the baseline |
| 2 | `alice` inserts a word mid-sentence **while** `bob` appends at the end | concurrent **non-overlapping** edits both apply |
| 3 | `alice` and `bob` insert at the **same spot** at once | the no-conflict guarantee — **both land, neither is lost** |
| 4 | `alice` **deletes** a char **while** `bob` inserts one | tombstone + insert converge |

## What you'll see

```
[phase 1] alice types the base sentence
  after phase 1:                     'hello world'

[phase 2] alice inserts 'BIG ' before 'world'  ||  bob appends '!'
  after phase 2:                     'hello BIG world!'

[phase 3] alice inserts 'X'  ||  bob inserts 'Y'  -- both at the very start
  after phase 3:                     'YXhello BIG world!'

[phase 4] alice deletes the trailing '!'  ||  bob appends '?'
  after phase 4 (final):             'YXhello BIG world?'

[time-travel] the same document at past logical clocks
  as of end of phase 1 (clock 11):  'hello world'
  as of end of phase 2 (clock 17):  'hello BIG world!'
  live:                              'YXhello BIG world?'
```

Phase 3 is the point: two editors insert at the very same position concurrently, and **both characters survive** (here `Y` then `X` — the order is whatever the two random ids resolve to, but it is the *same* on every replica). The self-check opens three independent reader sessions and asserts they render byte-identical text, that no concurrent edit was dropped, that the deleted character is gone, and that the time-travel snapshots are stable.

## Why this is conflict-free

| CRDT property | How it falls out |
|---|---|
| **Commutative** | the only write is `|=` (set union); order of arrival doesn't matter |
| **Convergent** | reads sort the same set by the same total order → every replica renders identically |
| **No lost updates** | concurrent inserts get distinct dense positions (ties → `(site, clock)`); the set keeps both |
| **Deletes** | a tombstone set; render drops tombstoned positions — commutative with inserts |
| **Causal/history** | each edit carries a logical clock; `as_of` replays any past state |

No editor ever reads-then-writes a shared value; it only `|=`s immutable records and reads the fold. The lost-update-free guarantee is the same commutative deferred-mutation field call as the other examples ([#49](https://github.com/orlyatomics/orly/issues/49) / PRs #50/#51/#52); storable record sets are [#90](https://github.com/orlyatomics/orly/issues/90).

## Schema (the entire engine side)

```orly
posid is [str];                                          -- a Logoot position id
ins_t is <{.pos: posid, .ch: str, .site: str, .clock: int}>;
del_t is <{.pos: posid, .clock: int}>;

<['ins', doc]>::({ins_t})    -- the set of inserted characters
<['del', doc]>::({del_t})    -- the set of delete tombstones
```

`insert` / `remove` are the two commutative writes; `render` / `visible` / `render_as_of` / `visible_as_of` / `char_count` are the in-engine reconstruction. The driver adds only `between(p, q)`.

## Caveats

This is a **convergence demo, not a product** — it shows the database giving you the CRDT substrate that a collaborative editor is built *on*, not a finished editor.

- **No UI, no rich text, no cursors/presence.** Plain character sequences.
- **Logoot interleaving.** Like all Logoot/LSEQ schemes, two editors *interleaving* characters into the *same* run concurrently can produce an interleaved (rather than cleanly grouped) result — correct and convergent, but not always what a human would pick. Stronger orderings (RGA, Fugue) avoid this at more complexity; Logoot is the clearest illustration of the pattern.
- **Position depth.** Repeatedly inserting at the exact same spot grows position ids (the Logoot tree deepens). The random-digit allocation keeps this shallow in practice (the property test stays at depth ~5 over 300k inserts), but a production system would rebalance/compact.
- **Driver-side logical clock.** A process-wide counter here; a real deployment would use a hybrid logical clock or per-site Lamport clock. The schema (`.clock` is just an `int`) doesn't change.

## Related demos

- [`examples/grc20-pov/`](../grc20-pov/) — the same `sorted_by` + `reduce` read-time fold, over an event log (latest-write-wins per key) instead of a sequence. This demo generalizes it from "resolve a key" to "order a sequence."
- [`examples/agent-swarm/`](../agent-swarm/) — multi-writer commutative `|=` / `+=` under contention, the same no-lost-update guarantee.
- [`examples/wikipedia-categories/`](../wikipedia-categories/) — fold-on-read with set-union values and a version axis in the key.

## Files

| File | What |
|---|---|
| `crdt_text.orly` | The Orlyscript package: `insert` / `remove`, in-engine `render` / `visible` / `*_as_of` / `char_count`, + inline tests |
| `demo.py` | Python WebSocket driver: `between()`, the 4 phases, time-travel, convergence self-check |
| `demo.go` + `go.mod` + `go.sum` | Go WebSocket driver, equivalent to Python |
| `run.sh` / `run-go.sh` | End-to-end wrappers: compile, start fresh orlyi, run the chosen driver |
