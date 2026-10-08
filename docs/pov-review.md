# Reviewing a POV's changes (#746)

A child POV is a cheap branch: fork one off a shared POV (or the global POV), write in it, and
Tetris promotes the writes to the parent. Paused, a POV keeps its writes to itself, so a review
workflow can look at them before they land, and either promote them or throw them away:

```
new fast private pov from {<parent>} <{.conflicts: "report"}>;   -> "<pov>"   the fork
pause {<pov>};                                                                hold the writes back
try {<pov>} ...;                                                              edit
diff_pov {<pov>};                                                             what changed
promote_pov {<pov>};        or        discard_pov {<pov>};                    keep it, or drop it
review_pov {<pov>};                                                           promotion progress, conflicts
```

The same calls exist on the binary protocol (`NewReviewPov`, `DiffPov`, `DiscardPov`,
`PromotePov`, `ReviewPov` in `orly/protocol.h`, `TClient` in `orly/client/client.h`) and in the
clients:

| | Python | Go | TypeScript |
|---|---|---|---|
| fork, tracking conflicts | `new_pov(conflicts="report")` | `NewPovWith(PovOptions{Conflicts: "report"})` | `newPov({conflicts: "report"})` |
| a page of the diff | `diff(pov, start=, stop=, after=, limit=)` | `Diff(pov, DiffOptions{...})` | `diff(pov, {start, stop, after, limit})` |
| every page | `diff_pages(pov, ...)` | `DiffAll(pov, ...)` | `diffPages(pov, ...)` |
| discard | `discard(pov)` | `Discard(pov)` | `discard(pov)` |
| promote and wait | `promote(pov, force=False)` | `Promote(pov, force, timeout)` | `promote(pov, {force})` |
| promote, return at once | `request_promotion(pov)` | `RequestPromotion(pov, force)` | `requestPromotion(pov)` |
| progress and conflicts | `review(pov, after=)` | `Review(pov, after)` | `review(pov, {after})` |

[`examples/grc20-pov/review.py`](../examples/grc20-pov/review.py) walks through all of it on the
GRC-20 model: a draft edit is diffed, its promotion is refused over a conflict, it is discarded and
redone, and it promotes cleanly (`./run-review.sh`).

## Diff

`diff_pov {<pov>} [<{.start: <[...]>, .stop: <[...]>, .after: <[...]>, .limit: n}>];` lists the
keys the POV changed relative to its parent, in key order:

```json
{"changes": [
   {"key": ["count", 1, 0], "kind": "delta",   "before": 25, "after": 28, "op": "add", "delta": 3},
   {"key": ["edge", 1, 3],  "kind": "changed", "before": 3000, "after": 333},
   {"key": ["edge", 1, 4],  "kind": "removed", "before": 40, "after": null},
   {"key": ["edge", 1, 100],"kind": "added",   "before": null, "after": 0}],
 "next": null, "next_literal": null, "updates": 10}
```

- **What it covers.** The POV's unpromoted writes: everything in its own repo, which for a paused
  POV is everything written since the fork (or since its last promotion). For a shared POV that
  includes what its children have promoted into it, but not their own unpromoted writes. The
  global POV has no parent and can't be diffed.
- **Kinds.** `added` (the parent didn't have the key), `changed`, `removed` (the POV deleted a key
  the parent has), and `delta`: the POV only applied commutative updates of one kind to the key
  (`+=`, `|=`, `*=`, ... ; `op` names it), and `delta` is what they add up to. A key the POV made
  and deleted again, or set back to the parent's value, isn't listed. Mixed operators on one key
  (`+=` then `*=`) come back as `changed`/`added` with the values.
- **Values.** `before` is the parent's value, read through the parent's own view of its chain
  when the page is read; `after` is the POV's: `before` with the POV's changes applied. While the
  parent is being written, `before` (and so `after`, for a delta) follow it, and so can
  `added` versus `changed` when the parent makes or deletes the key meanwhile; the keys listed and
  the deltas are the POV's alone. The POV's changes are read from its own repo in its
  own sequence order; sequence numbers are never compared across repos (each repo numbers its own
  updates, #791).
- **Range and paging.** `.start` (inclusive) and `.stop` (exclusive) restrict the keys, in key
  order: `<['edge', 1]>` to `<['edge', 2]>` is every key starting `'edge', 1`. (They aren't
  `.from`/`.to` because those are keywords of the statement grammar.) A page holds `.limit`
  changes (default 100, at most 10,000), plus any more of the last key in other value types.
  `next` is the page's last key when more follow; pass it back as `.after` (keyset paging, as
  #735). `next_literal` is the same key as an orlyscript literal, which round-trips exactly
  (JSON can't tell `1` from `1.0`); the clients' paging helpers use it.
- **Cost.** Each page walks the POV's whole backlog, which is in memory and capped (a POV holds at
  most 1/32 of the Update Entry pool), and reads the parent once per change in the page.

## Discard

`discard_pov {<pov>};` throws away the POV's unpromoted writes, as of the call, so it reads as its
parent again: `{"discarded_updates": 40, "discarded_entries": 4000}`. It pauses the POV while it
works (which waits for a promotion under way to land) and restores its status after, pops each
update without promoting it, and queues the memory merge that frees them; the update pools fall
back within moments. The pops replicate, so a slave drops the writes too. Only the session that
made a private POV may discard it (a shared POV holds other sessions' writes), and a failed POV
stays failed. A POV that tracks conflicts is re-forked: what its parent changed before the discard
is its new starting point.

## Conflicts

A POV made with `<{.conflicts: "report"}>` or `<{.conflicts: "refuse"}>` (binary:
`NewReviewPov`) tracks, from the moment it is made, every key its parent chain changes: the parent,
its parent, and so on up to the global POV.

- **A conflict** is an overwrite or delete, in one of the POV's updates, of a key the chain changed
  after the fork and before that update reached the parent. Its commutative updates never
  conflict: `+=` composes with whatever the parent did. A commutative write *in the parent* is
  still a change, so the POV overwriting that key does conflict.
- **What counts as a change.** A write straight into a shared POV of the chain, or a promotion into
  it from outside the chain (a sibling of the POV, say). Content moving up the chain (a shared
  parent promoting into the global POV) isn't: the POV could already see it. The check runs where
  the POV's update lands in its parent, under that repo's lock, so it is exact: a conflict is
  reported if and only if the chain changed the key in that window. After an update lands, its
  overwrites stand as the POV's own: only a later parent change makes the key conflict again.
- **Report** (`"report"`): the update promotes, and the conflict is recorded, numbered from 1.
- **Refuse** (`"refuse"`): Tetris tests each update before it promotes it, and holds the POV back
  (`review_pov` says `"blocked": true` and lists `blocked_on`) instead of promoting one that would
  conflict. That isn't an assertion failure, so it never fails the POV. `promote_pov` first tests
  the whole backlog and, if anything would conflict, leaves the POV as it was and answers
  `"status": "refused"` with the keys. To go ahead anyway, `promote_pov {<pov>} <{.force: true}>`
  lets everything unpromoted at that moment through (still reported); or `discard_pov` and redo the
  edits on top of the parent. Tetris's test reads the changes recorded so far, so a parent change
  that lands between the test and the promotion's commit is overwritten anyway; the landing check
  then reports it with `"raced": true`. It is the same window Tetris's own assertion tests have.
- **Promotion.** `promote_pov` unpauses the POV and answers at once: `"promoting"` (with `pending`,
  the unpromoted updates, and `mark`, the number of the last conflict so far) or `"refused"`.
  `review_pov {<pov>} <{.after: <mark>}>` then shows progress: `pending`, `status`, `blocked`, and
  the conflicts numbered after `mark`, which are this promotion's. The clients' `promote` helpers
  poll it until nothing is pending, or the POV is blocked, failed or paused.
- **Cost.** Nothing for a POV made without `.conflicts`. For one with: every update appended to a
  repo of its chain copies its keys, once each, into the POV's tracker, so a review POV under a busy
  parent grows with the distinct keys the parent changes. Past a million keys the tracker stops
  recording and reports `"overflowed": true`; from then on every overwrite or delete counts as a
  conflict, because it can no longer tell. The last 10,000 conflicts are kept.
- **Limits.** The tracker lives in the master's memory, like the POV itself (POVs don't survive a
  restart, #439); a slave doesn't keep one, so after a failover the POV promotes unwatched.
  Tracking needs no change to anything on disk.

## Save points

Named save points (a diff or a discard "since save point X" rather than against the parent) need
versioned reads, which are being designed in #745 (PR #803, `docs/design/versioned-reads.md`).
The hook is in place: `diff_pov` takes `.since: "<name>"` and refuses it until then, and the
conflict tracker (`orly/indy/fork_watch.h`) is where a POV's versions would be kept.

## Tests

- `clients/smoke/run-pov-review.sh` (CI): diffs of safe and fast, shared and private POVs under the
  global POV and under a paused shared POV, also while another connection writes the parent, by
  range and paged; report-mode conflicts on promotion; refusing mode refused, forced, blocked by
  Tetris and discarded; commutative updates never conflicting; a discard of 4,000 writes freeing the
  update pools; the error cases; then the Python and Go clients. Its `pov_review.mjs` also runs
  under ThreadSanitizer in `run-tsan-server.sh`.
- `orly/indy/fork_watch.test`: the tracker's rules, refusing and forcing, re-forking, overflow,
  and concurrent appends (in the TSan set).
- `orly/server/import_replication.test` `PovReviewOverBinaryProtocol`: the calls over the binary
  protocol on a master with a slave; the slave applies a discard to its paused copy, and after a
  failover that copy no longer holds the writes.
