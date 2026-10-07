# Design: versioned reads with a per-POV retention policy (#745)

> Status: **proposal, awaiting a decision.** No engine code is part of this design. The
> implementation changes what the global POV keeps on disk and adds a public notion of a
> version, so it is a one-way door and needs explicit approval at the point marked in
> [§13](#13-staged-plan). Line numbers are omitted; search by type or function name.

## 1. Summary

**What #745 asks for.** Read a POV as it stood at an earlier version, consistently across keys;
compare two versions; page through a result while writes continue and get one snapshot; keep
versions per a retention policy (everything, the last N, or a time window) and refuse anything
outside it with a typed `version_unavailable`. Current reads must cost what they cost today, an
older point read at most logarithmically more, and an old range listing no more than the listing
plus the changes since.

**What the engine already has.** Most of the machinery exists and is unused:

- Every write to the global POV gets a **global sequence number**. It is durable, it is in every
  data file, and replication pushes it unchanged, so a slave has the same numbers and they
  survive a failover.
- Every read already goes through a pinned **view** with a sequence-number window
  (`TRepo::TView`), and the repo walker already drops entries outside it.
- Memory layers keep **every** version of every key. A data file stores each key's current entry
  in one region and its older versions in a separate, fixed-size **history region**, newest
  first. Current reads never touch the history region except to fold `+=` chains.
- Normal running throws history away in three places: #602's tail merge (keep current entries and
  little else), #55's fold (any file with a `+=` in it is rewritten to one entry per key), and the
  child-POV memory merge (promoted entries leave the child).

**What I measured** (a scratch prototype, never pushed: release `orlyi`, native aarch64, keys
overwritten 32 or 256 times, reads in one WebSocket call each; details in [§7](#7-measurements)
and [§8](#8-write-amplification-and-disk-growth-by-retention-policy)):

- **Current reads don't pay for retained history.** With 32 versions per key kept on disk, point
  reads and range walks cost the same as with #602's pruning (+0.6%, +0%); at 256 versions, +5% and
  +16% (one server each, not yet explained).
- **A read at an old version costs a small, flat amount more.** Binary search of the key's history
  run: +12–16% per point read with 32 versions kept, +10–23% with 256, no more for 256 versions back
  than for 64. A linear scan grows with age (+93% at 256 back). Range walks at an old version where every key has changed
  since: +55–74%.
- **A retained version costs 64–73 bytes of disk** (more for values that don't fit inline).
- **But today's merge policy rewrites retained history over and over.** Files are paired by key
  count, and an overwrite workload never adds keys, so the file holding every key is merged again
  every two or three flushes, history and all. Keeping every version made the merges write 3.5×
  the bytes at 32 versions per key and 6.8× at 256, growing with the square of the history.
  Sizing merge generations by bytes cut that to 1.7× and 2.3×, but left a third disk layer, which
  made current point reads 31% and range walks 81% slower.

**Recommendation.** Build the version core first, the same whichever storage option follows: a
version is a **global sequence number**; a read at version V finds each key's newest entry at or
before V (galloping search in the history run); leases and continuations pin a version; a child
POV's version is a vector of per-repo numbers, readable while its own part is unpromoted. For
retention, use **Option A** (keep history in the existing files, with a retention-aware merge and
keyframed `+=` chains) **for bounded windows only**: the last N versions, a time window, save
points, open leases. Under A the extra merge cost is bounded by the window. **Unbounded "keep
everything"** on data that is overwritten needs history out of the main files (**Option B**, a
separate history index), because A's merges would grow without bound or its reads would slow down.
Defer B until a workload needs it; the version core and API don't change when it comes. See
[§12](#12-recommendation).

### The decision to make

1. **Does v1 need unbounded retention ("keep everything") on overwritten data?** The issue calls
   it the default for a source of record. I recommend **no for v1**: ship A with bounded windows
   (cheap, no new file kind, a 0.2.x binary can still read the store) and build B when a workload
   needs unbounded history. If the answer is yes, B has to be built before retention ships: a new
   file kind, a second output from the merge, and a hard one-way format change in the part of the
   engine with most of 2026's integrity bugs. C (checkpoints plus a delta log) can't meet the cost
   targets either way.
2. **What a version means, publicly.** I recommend: an opaque 64-bit number that is the global POV's
   sequence number, the same on every replica, increasing with commit order. Clients will store these
   numbers (in continuations, save points, their own tables), so this contract can't change later
   without breaking them. For a non-global POV, a version is a vector that stays readable only until
   the POV's own writes are promoted (then `version_unavailable`), or while the POV is paused.
   Durable child versions that survive promotion would need a translation index written on every
   promotion; I recommend against them ([§4](#4-what-a-version-is)).
3. **The default retention.** I recommend **no change**: the default stays #602's pruning, and
   retention is an explicit per-POV setting, so an upgrade never changes a store's disk growth or
   merge load by surprise.
4. **Sequencing with #755.** #755's write-ahead log is a new on-disk file kind and a hard one-way
   change; A adds no file kind. Both need a store format version, which Orly doesn't have. I
   recommend one format bump carrying the version stamp, #755's log and A's retention metadata
   ([§11](#11-format-change-and-migration)), behind one approval.

## 2. Requirements and how each option meets them

| Requirement (#745) | A: chains in layers | B: separate history index | C: checkpoints + delta log |
|---|---|---|---|
| Read at a version, consistent across keys | yes: one view, sequence filter | yes | yes, but only near a checkpoint cheaply |
| Retention: everything / last N / time window | last N and time windows; **not** everything (§8) | all three; drop whole history partitions | all three; drop checkpoints and log segments |
| Outside retention → `version_unavailable` | horizon check before the read | same | same |
| Opened version stays readable until released | lease holds the horizon | same | lease pins a checkpoint |
| Current read ≈ today | yes with bounded history (measured, [§7](#7-measurements)); unbounded history slows merges or reads ([§8](#8-write-amplification-and-disk-growth-by-retention-policy)) | yes, main files stay live-sized | yes |
| Older point read ≤ log(age) more | galloping search in the key's history run (measured flat) | probe O(log) partitions | **no**: replay from the checkpoint, O(changes since it) |
| Old range = listing + changes since | yes, with a log factor per changed key | yes, exactly | **no**: replays the whole log since the checkpoint, unindexed by key |
| `+=` defined at every version | keyframed delta chains | deltas in history, keyframes | replay |
| Pruning respects the horizon (#592) | tail rule gains a horizon | main files prune as today | as today |
| Snapshot paging (#735/#793) | continuation carries the version | same | same |
| Diff between versions | key walk or the update index | history partitions are the diff | the log is the diff |
| Format change | none for the files; metadata only | new file kind, new merge job | new checkpoint and log kinds |

## 3. The current data path: where versions are created and dropped

### 3.1 Created

- **Commit.** `TRepo::AppendUpdate` gives each transaction the repo's next sequence number
  (`NextUpdate`) and inserts its entries into the current memory layer. That is the POV's own
  repo: a private or shared POV's numbers are its own.
- **Promotion.** Tetris pushes the update into the parent and pops it from the child in one
  transaction; the parent assigns **its own** next number (`TTransaction::Push`). So one logical
  update has a different sequence number in every repo it passes through, and only the number in
  the global POV is permanent. The update keeps its `UpdateId` (a UUID) throughout, which is what
  the read-time `+=` dedup in `context.cc` uses during the promotion window.
- **Replication.** The master ships each transaction's replica; the slave pushes it with
  `ensure_or_discard` set to the master's sequence number (`transaction_base.cc`), so a slave's
  numbers equal the master's for every repo it holds. A slave join copies whole data files
  (`SyncFile`) and pulls the gaps by sequence range. **Global sequence numbers survive failover.**
- **Time.** Each update's metadata (`TMetaRecord`) records `RunTimestamp`, the time it committed
  in the POV it was written to, and the data file's update index keeps that metadata per update.
  That is not the time it reached the global POV: promotion comes later, and in a different order
  across POVs, so these timestamps are not monotonic in global sequence order.

### 3.2 Kept

- **Memory layers keep everything.** A layer's entry list is ordered by (index, key, newest
  first) and holds every entry pushed to it. The global POV's memory merge (`StepMergeMem`)
  combines layers without dropping entries (the root "is the durable home and must keep
  everything").
- **The first data file keeps everything.** `TDataFile` writes the layer: for each key the newest
  entry goes to the **key region** (`TKeyItem`: sequence number, key, value, history count and
  offset, mutator; 80 bytes) and every older one to that key's run in the **history region**
  (`THistoryKeyItem`: sequence number, key, value, mutator; 64 bytes), newest first. Keys and
  values that don't fit in a 24-byte `TCore` live in the file's arena. The file also has an
  **update index**: per update its sequence number, metadata and id, plus a pointer to each of its
  entries.
- **Reads.** `TContext` takes one `TRepo::TView` per repo in the chain, child first. A view pins
  the mapping (the layer list), the current memory layer, and the window `[LowestSeqNum,
  HighestSeqNum]`. `TRepo::TPresentWalker` merges one walker per layer and drops entries outside
  the window. A disk layer's walker (`TPresentWalkFile`) hashes to the key in the key region and
  reads the history region only when the current entry is a commutative delta (`ArmHistory`), to
  replay the `+=` chain into the fold. `TContext::TPresentWalker` merges the repos and folds
  commutative runs. Since #796 (open), entries of one key are ordered by repo first (nearest the
  POV first) and by sequence number only within a repo, because sequence numbers of different
  repos are not comparable.

### 3.3 Dropped

| Where | What it drops | Code |
|---|---|---|
| Child memory merge | entries already promoted to the parent | `StepMergeMem` (`lower_seq_bound = ReleasedUpTo`) |
| Root disk merge (tail, #602) | every history entry except: the older input's current entry, entries of updates that still hold a current value, and `+=` chains; plus the update-index rows of updates that hold nothing current | `TSafeRepo::StepMergeDisk` → `MergeFiles(can_tail = IsMergePruningAllowed())`, `TMergeDataFileImpl<true>` |
| The fold (#55) | **all** history of **every** key in a merged file that contains any commutative entry; rewrites each surviving entry as its own update with a fresh id and empty metadata (so commit times are lost too) | `TFoldDataFile`, called from `MergeFiles` |
| `tail` statement / `TailGlobalPov` | the oldest file's history, tombstones included | `TSafeRepo::StepTail` |
| Restart | every non-global POV and its unpromoted writes (#439) | — |

`--prune_merge_history=false` turns off the tail rule only; the fold still drops history for any
file holding a `+=`.

### 3.4 What a "version" means today

Nothing outside the engine. Inside, a version is a per-repo sequence number, meaningful only within
that repo (`sequence_number.h`). A read through a child POV is a set of views, one per repo,
each taken at a slightly different moment (#143), and #796 exists precisely because cross-repo
sequence numbers had been compared as if they were one clock.

## 4. What a version is

### 4.1 The global POV: a global sequence number

A version of the global POV is its sequence number V. "The state at V" is every key's newest
entry with sequence number ≤ V, with commutative chains folded. It is:

- **total and consistent:** one number orders every committed transaction; a read at V sees each
  transaction wholly or not at all, because a transaction's entries share its number;
- **the same on every replica** (`ensure_or_discard`), so a version handed out by a master is valid
  on the slave that replaces it;
- **cheap to name:** the current version is `HighestSeqNum`, already in every view.

A version is exposed as an **opaque, increasing 64-bit number**. Clients may compare two versions
of the same POV and nothing else (no arithmetic: a version need not have a successor one higher,
because merges drop whole updates and the update walkers already accept gaps).

**Time → version.** `at time T` resolves to the global version that was current at T on the
master's clock. Update metadata can't answer that (§3.1: its timestamps are when each write
committed in its own POV), so the global POV records a (time, version) sample at each memory merge
and keeps the samples with the retention metadata; that is a 40 ms resolution. The resolved version,
not the time, is what a continuation or a save point stores.

### 4.2 Non-global POVs: version vectors, valid until promotion

A private or shared POV reads through a chain of repos `[pov, parent, ..., global]`. Its version is
the vector of each repo's sequence number at the read, `(s_pov, s_parent, ..., s_global)`. Reading
at that vector filters each repo to its own component, which `TView` already does.

The problem is promotion. When the POV's writes up to `s_pov` are promoted, they leave the POV's repo
and reappear in the parent under **new** numbers, interleaved with siblings' writes that the
vector's `s_parent` was taken before. The vector then can't be answered from the repos. Two ways out:

- **(Recommended) A child component is valid only while unpromoted.** Once any of the POV's writes
  up to `s_pov` has been promoted (the repo's `LowestSeqNum` has passed it), the version is
  `version_unavailable`. A **paused** POV never promotes, so its versions stay valid for as long as
  it is paused, which is exactly the review case (#746): pause, save a point, look, compare,
  then discard or unpause. A shared POV with a live backlog has versions valid for seconds.
- **(Rejected) Translate on promotion.** Record, per promotion, `(child seq → parent seq)`, and
  answer the old vector as "parent at `s_parent` plus these translated entries". That needs a
  durable translation index written in the Tetris commit path (the throughput bottleneck, see
  `concurrent-merge-throughput.md`), a new filter shape in every walker (a seq window plus an
  include-set), and it still dies at restart because child POVs are ephemeral (#439). Nobody has
  asked for it.

A session that needs one consistent snapshot of a child POV for a short while (paging, a report)
takes a **snapshot handle** instead ([§4.4](#44-leases-and-snapshot-handles)).

### 4.3 Save points

A save point is a name for a version: `(pov, name) → version vector`, plus a lease on it.

- On the **global POV** it is durable: stored with the retention metadata (§11), replicated as a
  record in the stream, and it holds the horizon at its version until it is deleted. Retention
  "last N" or "7 days" never drops a version a save point names.
- On a **child POV** it lives as long as the POV (or until its component is promoted, §4.2). This
  is the hook #746 asks for: "diff this POV against its save point" is a read of the POV's own
  repo between two sequence numbers.

### 4.4 Leases and snapshot handles

"A reader that has opened a version keeps it readable until it releases it" needs two mechanisms,
because there are two kinds of reader:

- **A lease** (global POV): a registered `(version, expiry)` in the repo. The retention horizon is
  `min(policy horizon, oldest unexpired lease, oldest save point)`. A lease pins no files and no
  memory: the merge simply keeps the versions it covers. Each page of a paged read opens a fresh
  view and reads at the leased version. A lease has a TTL (default 10 minutes, renewed by use) so
  an abandoned continuation can't hold history forever; the server logs and reports the oldest
  lease. Leases are in memory on the master only: after a failover, a version still inside the
  policy horizon stays readable, and one held only by a lease becomes `version_unavailable`.
- **A snapshot handle** (any POV): the `TContext`'s views themselves, kept open between
  statements. This is the only way to hold a child POV's unpromoted state still, and it is
  expensive: views pin the memory layers and the disk files they saw, so merges can't free what
  they replace. Handles are counted against the memory budget like pinned memory (refused with
  `insufficient_memory` when it is short) and expire with a short TTL (default 60 s).

## 5. The options

### 5.1 Option A: version chains in the existing layers, retention-aware merge

**Layout.** Unchanged. A data file already has a current entry per key and a newest-first history
run per key. A keeps more of the history run.

**Merge (the retention rule).** For a root repo with horizon H, a merge keeps, for each key:
1. every entry with sequence number > H (the retained window),
2. the newest entry with sequence number ≤ H (the key's value at the horizon), unless it is a
   tombstone and the merged files are the oldest (then nothing older can be hidden),
3. the update-index rows of every update with sequence number > H, plus those of updates that
   still own an entry kept by (2).

This is the snapshot-aware compaction rule of RocksDB and of most MVCC stores. #602's rule is the
special case H = "now". `release_up_to` is already passed down to the file writers and ignored
(#592); A gives it a meaning.

**The fold, with keyframes.** Today a merged file containing any `+=` is rewritten with one entry
per key. Under A the fold:
- folds everything at or below H into one `Assign` at the newest sequence number ≤ H (as today);
- keeps entries above H as they are, deltas included, **except** that every `K`-th delta of a key
  (default K = 64) is written as an `Assign` of the folded value at that entry's sequence number;
- keeps every update's id and metadata (its original commit time).

An `Assign` of the folded value at sequence s is, by definition, the key's value at s, so it is a
correct version, and every reader, including a 0.2.x binary, stops its fold at the first `Assign`
it meets going newest first (`ApplyDeferredFold`). So a `+=` key has a defined value at every
retained version, and any read folds at most K deltas per file.

One reader change goes with it. The disk walker replays a delta key's **whole** history run
(`ArmHistory` sets `HistRemaining = num_hist_keys`), and the context fold then discards everything
below the base. With retained history that would read every retained version of a hot counter on
every current read. Under A the walker stops replaying after it yields an `Assign` (or a
tombstone). A 0.2.x binary reading a 0.3 store still gets the right value, just more slowly for
`+=` keys with long retained chains.

**Reading at V.**
- Files whose lowest sequence number is > V are skipped entirely (the file map records each file's
  range).
- In a file, the walker hashes to the key as today. If the current entry is ≤ V it is the answer
  (no change from today). Otherwise it **searches the key's history run** (fixed-size items,
  sorted by sequence number, newest first) for the newest entry ≤ V, galloping back 1, 2, 4, ...
  entries and then bisecting: about `2·log2(d)` item reads for a version `d` entries back, in one
  stream, almost always within one or two 4 KiB pages.
- Memory layers: the entry list already holds every version; the walker skips entries > V, which
  the repo walker's window filter does today.
- A range read at V walks the key region as today. Keys whose current entry is ≤ V cost what they
  cost today; keys changed since V pay the search; keys created after V are skipped; keys
  deleted after V are found through their tombstone's history.

**Diff (v1, v2] over a range.** Two plans, chosen by a cost estimate:
- *walk the range*, emitting keys whose newest entry ≤ v2 differs from their newest ≤ v1 (a key whose
  current entry is ≤ v1 is unchanged without touching history);
- *walk the update index* for updates in (v1, v2] and filter their keys by the range: cost
  proportional to the writes in the window, independent of the range's size.

**Costs.** Current reads: unchanged code path, and the key region does not grow ([§7](#7-measurements)).
Merges: every merge rewrites its inputs whole, history runs included, and today's merge pairing
(by key count) rewrites the file holding every key every few flushes, so retained history is
rewritten over and over. With bounded retention that costs up to about today's merge bytes × (1 +
retained/live); with unbounded retention on overwritten keys it grows with the square of the history
([§8](#8-write-amplification-and-disk-growth-by-retention-policy)). Pairing by bytes instead fixes the
writes but leaves more layers for current reads to visit.

**Risks.** Merge inputs grow with retention, and #692 skips pairs that don't fit the free space;
the fold materialises a whole file in the update pools (#627) and would fail more often with
retained deltas, so the fold must become streaming before retention can be large ([§9.6](#96-the-fold-55-627)).

### 5.2 Option B: a separate history index, keyed by (key, sequence)

**Layout.** The main data files keep only current entries (as #602 does today). When a merge drops a
superseded entry that is inside retention, it writes it instead to a **history file**, a new file
kind holding `(key, valid-from seq, valid-until seq, value, mutator)` sorted by key then
valid-from descending. History files are **partitioned by valid-until**: each partition covers a
range of supersession sequence numbers.

**Retention** is O(1): a partition whose valid-until range is entirely ≤ H is deleted whole. The
newest version ≤ H of a key that has since changed is in the partition of its supersession; for
it to stay, partitions straddling H are kept, and a background job rewrites the oldest partition
to drop what is entirely below H.

**Reading at V.** Hash into the main files: if the current entry is ≤ V, done. Otherwise probe the
history partitions whose valid-until range includes values > V, newest first, until one holds a
version valid at V. With partitions merged geometrically, that is O(log(age)) partitions, each a
hash probe. A range at V merges the main range walk with the history partitions' range walks
over valid-until > V: the history entries visited are exactly the changes since V, which matches
the target precisely.

**Costs.** A superseded version is written once to history and then only when partitions merge;
main-file merges stay the size of live data. Current reads unchanged. Old reads probe more files.
New code: a file kind, a writer, a walker, a partition manager, the file map and reload
(`ReConstructFromDisk`) must know the new kind, slave join must ship it, and the fault-injection
harness must cover it.

**Risks.** It is a new subsystem in the storage engine that has had most of 2026's integrity bugs
(#590, #618, #620, #666, #671). A 0.2.x binary meeting a history file in the file map would not
know what it is: this is a hard one-way format change.

### 5.3 Option C: checkpoints plus a delta log

**Layout.** A checkpoint is a pinned mapping: the set of data files at a version, kept from being
removed by a reference held in the retention metadata. The delta log is the sequence of updates
since each checkpoint: either the update indexes of the files, or (with #755) the write-ahead log
kept beyond its truncation point.

**Reading at V.** Read the nearest checkpoint ≤ V and apply the log's updates in (checkpoint, V].
For a point read that is a scan of the log for one key, O(updates since the checkpoint), unless
the log is indexed by key, which turns it into B. A range read at V merges a checkpoint range walk
with every logged change in the range.

**Costs.** Disk: each checkpoint keeps the files of its moment, so with merges rewriting files,
up to one full copy of the live data per checkpoint. Version granularity is coarse unless every
read pays the log replay.

**Verdict.** C fails the point-read and range targets for any version far from a checkpoint. It is
a good implementation of **coarse save points** (a nightly checkpoint that can be read, diffed, or
restored) and of point-in-time recovery, and it composes with #755's log. It is not a versioned
read path.

### 5.4 Options considered and dropped

- **Pinned views as the only mechanism** (no merge change; a reader holds a `TView` for as long as
  it wants its version). It meets paging within a session today, but pins memory layers and every
  file a merge replaces, so its cost grows with write rate × hold time, and it cannot answer a
  version nobody held. Kept only as the snapshot handle (§4.4) for child POVs.
- **User-space history** (the `grc20-pov` example's event sets folded on read). O(all events) per
  read; the issue's starting point.

## 6. Cost model

Notation: `N` keys in the range, `d` versions of a key between V and now in one file, `L` disk layers, `c` keys
changed since V in the range, `K` keyframe interval.

| Read | Today | A | B | C |
|---|---|---|---|---|
| Current point | L hash probes | L hash probes (same code) | L hash probes | L hash probes |
| Point at V | n/a | L probes + about `2·log2(d)` history items per file holding a newer entry | L probes + O(log age) partition probes | checkpoint probe + a scan of the log since the checkpoint |
| Current range | N key items | N key items | N key items | N key items |
| Range at V | n/a | N key items + about `c·2·log2(d)` history items | N key items + c history items | checkpoint range + O(all changes since checkpoint) |
| `+=` at V | n/a | ≤ K deltas per file | ≤ K deltas per file | replay |
| Diff (v1,v2] | n/a | min(N, writes in window) | c over v1's partitions | log in window |

Entry sizes ([§3.2](#32-kept)): a retained version under A costs one 64-byte history item, the
value's arena bytes if it is not stored inline, and its share of the update index (72 bytes per
update plus 8 per entry). Measured: 64–73 bytes per retained overwrite on disk.

## 7. Measurements

**Setup.** A scratch branch (never pushed) adds two things to master `3fc60840`:
- a switch that makes every present read answer as of a sequence number V: `TRepo::TPresentWalker`
  narrows its window to V, and `TPresentWalkFile`, given a current entry newer than V, finds the
  key's newest history entry at or before V, by binary search or by a linear scan newest first;
- byte counters on the global POV's flushes (`WriteFile`) and disk merges (`MergeFiles`).

Release build, native aarch64 (Docker on Apple silicon, a shared box), `--mem_sim` volumes, one
`orlyi` per configuration. A package writes `new <['v', n]> <- x` and reads in one call either
2,000 random keys (`sum_keys`, a point read each) or every key (`count_all`, a range walk). A load
writes R+1 rounds; round r sets every key to r, in batches of 500 keys per transaction. After the
disk merges settle (two disk layers and no memory layers in every run, except the byte-generation
runs in §8), reads go through a fresh
POV; each figure is the median of 9 calls, and each configuration's median of several repetitions,
alternating current and old reads in the same server. Every read at a version was checked: the
mean value read equals the round of that version. `--prune_merge_history=false` keeps every
`Assign` version, which is what A's retention rule does with H = 0; `true` is #602's behaviour.

Most of each figure is the method's own per-key work in orlyscript and the call's share of the
round trip, which every configuration pays alike, so compare the differences, not the totals.

**Current reads do not pay for retained history.** 20,000 keys, 32 overwrites each, 10 repetitions
over two servers per row:

| Global POV keeps | disk used | point read, ns/key | range walk, ns/row |
|---|---:|---:|---:|
| #602's pruning (`true`) | 8.9–9.2 MB | 1,753 | 220 |
| every version (`false`): 33 per key | 55.6–56.1 MB | 1,764 (+0.6%) | 220 (+0%) |

The key region is the same size either way, and current reads never open the history region of an
`Assign` key.

**Reads at an old version cost a bounded, small amount more.** Same 32-version store, reads at the
end of round r:

| Read at | point, binary | point, linear | range, binary | range, linear |
|---|---:|---:|---:|---:|
| current | 1,764 | – | 220 | – |
| round 31 (1 version back) | 1,974 (+12%) | 1,825 (+3%) | 342 (+55%) | 209 (−5%) |
| round 16 (16 back) | 2,040 (+16%) | 1,962 (+11%) | 383 (+74%) | 295 (+34%) |
| round 0 (32 back) | 2,013 (+14%) | 2,082 (+18%) | 366 (+66%) | 393 (+79%) |

- Binary search is **flat with age**: about +250 ns per point read and +120–160 ns per range row,
  whether the version is 1 or 32 back. Linear search grows with age, as expected.
- In the range rows every one of the 20,000 keys has changed since V, so each row pays a search:
  this is the worst case of "listing plus changes since". A key unchanged since V costs what it costs
  today.
- The prototype's binary search opens a new stream per probe, which is why it loses to the linear
  scan for very recent versions. The implementation should **gallop**: step back 1, 2, 4, ... entries
  from the newest, then binary-search the last interval, all in one stream. That costs
  `O(log d)` for a version `d` entries back, which is the issue's target exactly ("logarithmically
  more the older it is"), and it is no worse than the linear scan at d = 1.

**Deeper history: 256 versions per key.** 5,000 keys overwritten 256 times (3 repetitions, one
server per row; with fewer keys the per-call overhead weighs more per row, so compare within the
table):

| Read at | point, binary | point, linear | range, binary | range, linear |
|---|---:|---:|---:|---:|
| current, #602's pruning | 1,802 | – | 303 | – |
| current, every version kept | 1,890 (+5%) | – | 352 (+16%) | – |
| round 255 (1 back) | 2,082 | 2,001 | 488 | 422 |
| round 192 (64 back) | 2,322 | 2,378 | 576 | 626 |
| round 128 (128 back) | 2,328 | 2,820 | 552 | 989 |
| round 64 (192 back) | 2,313 | 3,207 | 549 | 1,356 |
| round 0 (256 back) | 2,305 | 3,641 | 563 | 1,761 |

Binary search stays flat from 64 versions back to 256 (about +430 ns per point read and +200–225 ns
per changed row, for eight probes). A linear scan at the oldest version costs 1.9× a current point
read and 5× a current range row. The current range walk was 16%
slower with 256 versions kept, on one server each; at 32 versions, with two servers each, there
was no difference. I did not track this down; stage 1 should measure it on a quiet machine before
retention ships.

**Disk per retained version.** 20,000 keys: 8 overwrites each kept, 19.3 MB against about 9.0 MB
pruned; 32 kept, 55.8 MB against 9.1 MB. 5,000 keys with 256 kept: 99.0 MB against 6.2 MB. That is
**64–73 bytes per retained version**: a 64-byte
history item, an 8-byte update-index pointer, and the update's 72-byte index row shared by its 500
entries. A value or key too big to sit inline in its 24-byte `TCore` adds its arena bytes; #592's
workload (one key per update, larger keys) measured about 184 bytes per overwrite.

## 8. Write amplification and disk growth by retention policy

**Measured write volume.** Bytes written by the global POV's flushes and disk merges (the scratch
counters), per logical write:

| Load | Global POV keeps | flushes | disk merges | total per write | merge bytes vs pruned |
|---|---|---:|---:|---:|---:|
| 20,000 keys × 33 rounds | #602's pruning | 158–160 MB | 509–519 MB | 1.0 KB | 1× |
| 20,000 keys × 33 rounds | every version | 155–157 MB | 1,725–1,898 MB | 2.8–3.1 KB | 3.4–3.7× |
| 5,000 keys × 257 rounds | #602's pruning | 362 MB | 709 MB | 0.8 KB | 1× |
| 5,000 keys × 257 rounds | every version | 349 MB | 4,791 MB | 4.0 KB | 6.8× |

**The merge policy, not the history itself, is the problem.** The per-merge log shows why. The disk
merge pairs adjacent files whose *key counts* fall in the same or a rising generation
(`SuggestGeneration(GetSize())`). An overwrite workload never adds keys, so the file holding every
key never moves up a generation, however much history it holds, and each new flush file of a few
thousand keys is in the same generation. So the big file is merged again every two or three
merges: in the 32-version run, 76 of the 150 merges wrote a file holding every key, most of them
the big file growing from 4 MB to 50 MB; in the 256-version run 444 of 608 did, and the merges
wrote 52 times the final retained history. Write volume per flush grows with the history retained,
so total merge writes grow with its square. Under #602's pruning the same file stays a few MB and the
pattern is cheap, which is why nobody noticed.

**Sizing generations by bytes fixes the writes and costs the reads.** A second scratch switch made
the merge (and its candidate check) size generations by file bytes / 80, about one key entry, so
history counts:

| Load | Keeps | generations by | merge bytes | vs pruned | disk layers at rest | current point | current range |
|---|---|---|---:|---:|---:|---:|---:|
| 20,000 × 33 | pruned | bytes | 476 MB | 1× | 2 | 1,744 | 208 |
| 20,000 × 33 | everything | keys (today) | 1,725–1,898 MB | 3.5× | 2 | 1,764 | 220 |
| 20,000 × 33 | everything | bytes | 807 MB | 1.7× | 3 | 2,315 (+31%) | 399 (+81%) |
| 5,000 × 257 | everything | keys (today) | 4,791 MB | 6.8× | 2 | 1,890 | 352 |
| 5,000 × 257 | everything | bytes | 1,599 MB | 2.3× | 3 | 2,325 (+23%) | 499 (+42%) |

With byte-sized generations, the history-heavy file sits a generation above the newer files, so the
newest round of overwrites stays in its own layer until enough accumulates to climb to it. Every
current read then visits three layers instead of two. That is the classic size-tiered trade, and under A there is no escaping it: the history lives in the same
file as the current entries, so either the merge rewrites it often or the read visits more files.

So A's cost depends on how much history is retained **relative to the live data**: with today's
policy, the merge bytes grew to about 3.5× at 5× retained/live and 6.8× at 15×. A bounded window
keeps that ratio bounded; "everything" on overwritten keys doesn't.

(The 256-version ratio is against the pruned run with today's generations; I did not run a pruned
256-version load with byte generations. The flush figures are larger per write than the final file is per entry because each 40 ms flush
file carries its own index and hash tables; that is the same in every row.)

**By retention policy** (`b` ≈ 64–73 B per retained version on top of live data, more for values
that are not inline; `W` = overwrites per second; `D` = live data):

| Policy | Disk held for history | Merge writes (A, today's tiering) | Offer under A? |
|---|---|---|---|
| Default (#602) | ≤ one older version per key per merged pair | today's | yes (default) |
| Last N versions (global transactions) | `b` × entries written by the last N transactions, plus one base version per changed key | ≈ today's × (1 + history/`D`), bounded | yes |
| Time window T | `b × W × T` | same, bounded by `T` | yes |
| Save points, leases | the versions they name, until released | same, bounded by the oldest | yes |
| Everything | grows by `b` per overwrite forever (1,000 overwrites/s × 73 B ≈ 6.3 GB/day; at #592's 184 B, 16 GB/day) | grows with the square of the history | **no; needs B** |

For data that is mostly added, not overwritten, "everything" retains little and A would handle it.
But the server can't know in advance which workload it will get, and it can't take the setting back
later without dropping history the user asked it to keep. So I recommend not offering it under A.

"Last N" counts global versions, not versions per key. A per-key limit can't give a consistent read
at a version: one key would still have its value at V while a hotter key had already dropped it, and
the issue rules out answering from partial state.

**Under B** (an estimate built from measured parts: B's main files hold what #602 keeps, so they merge
like the pruned rows; each retained version is written once to a history file, about `b`, plus
whatever merging of history files keeps their probe count down, say up to 3×): 32 versions, about
510 MB + 47–140 MB ≈ 0.56–0.66 GB of merge writes against A's 1.7–1.9 GB; 256 versions, about
709 MB + 93–280 MB ≈ 0.8–1.0 GB against A's 4.8 GB. Current reads stay at the pruned rows' two
layers. That is B's case for unbounded retention. Its costs are code and risk ([§5.2](#52-option-b-a-separate-history-index-keyed-by-key-sequence)).

## 9. Interactions

### 9.1 Tetris promotion

Unchanged. Promotion assigns global versions; a version is born when Tetris commits into the global
POV. Tetris's own reads (`TestAssertions` against the parent) are current reads. The only new cost
is in the global memory merge and disk merges, which are off the promotion path, except that larger
merges hold the merge threads longer; [`concurrent-merge-throughput.md`](concurrent-merge-throughput.md)
found the root merge thread is not the binding constraint today.

### 9.2 The POV hierarchy

See §4.2. Versions of the global POV are durable and replicated; versions of other POVs are vectors,
valid while unpromoted or while the POV is paused. Retention is set **per POV**, but it only has an
effect on the global POV (and on any other parentless safe repo): a child's retention is its backlog.
A read through a child at a vector filters each repo to its component, and #796's repo-first ordering
is what makes that read correct: it never compares two repos' numbers.

### 9.3 Replication and failover

- Versions are equal on master and slave (§3.1).
- **The horizon must be equal too**, or a version could be readable on the master and gone on the
  promoted slave. A time-based horizon depends on clocks, so the master decides: it ships a
  "horizon advanced to H" record in the replication stream, and slaves prune to the shipped H, never
  further. Save points travel the same way.
- A slave join copies files, which carry their retained history, plus the retention metadata.
- Leases are not replicated (§4.4).

### 9.4 The durable manager

Save points and the retention policy are small records that belong with the repo's metadata. The
durable manager already persists TTL'd objects; save points on the global POV fit as durable objects
with no TTL, and child save points as objects tied to their POV's session. No change to the durable
manager's format.

### 9.5 Admission (#590, #607, #692, #721, #765)

- **Disk.** Retention is live data. Write admission's reserve (`--disk_reserve_*`) and the merge's
  claim of its input size both already account for it, so a full disk refuses writes with
  `insufficient_storage` as it does today. What changes is how fast a store gets there: within a
  window, a store holds the retained bytes per write for as long as the window lasts ([§8](#8-write-amplification-and-disk-growth-by-retention-policy)).
  The server should report the retained bytes and the horizon, and refuse a policy change that the
  free space can't cover.
- **Merges that don't fit.** #692 skips a pair whose inputs exceed the free room. Retained history
  makes inputs larger, so on a nearly full disk the merge stalls sooner and files accumulate (read
  amplification). Under A the mitigation is to let the merge advance the horizon only on explicit
  policy, never silently; under B it doesn't arise because main files stay the size of live data.
- **Memory.** Memory layers already hold every version until they are flushed; nothing changes. A
  snapshot handle pins memory and must be counted (§4.4).

### 9.6 The fold (#55, #627)

The fold reads a whole merged file into a memory layer in the update pools; when the pools can't
take it, `MergeFiles` keeps the unfolded file (#627) and reads fold longer chains. Under A with
retention, the fold keeps more entries, so it needs more pool space and will miss more often. The
fold should become a streaming pass (key by key, as `TMergeDataFileImpl` already is) before
retention is allowed to grow large. This is independent of versioning and worth doing anyway.

### 9.7 The read budget (#694, #729)

A read at V charges rows as a current read does. A key's history probe counts with the key (one row),
as a folded history does today. A diff charges one row per key it examines on the key-walk plan and
one per update entry on the update-index plan. A snapshot handle's pinned memory is charged to the
session's budget, not the read's.

### 9.8 Keyset paging (#735, #793)

#793 (open) makes a continuation a key: `keys ... after <last key>`. A versioned continuation is
`(version, last key)`. The client's `pages()` helper takes the version from the first page's reply
and sends it with every later page, with a lease ([§10](#10-api-surface)); on a non-global POV it uses
a snapshot handle. Without a version, paging keeps today's semantics (each page reads current).

### 9.9 POV review (#746)

- "Diff a POV against its parent" is a read of the POV's own repo (its unpromoted entries), which
  needs no versioning.
- "Diff a POV between two save points" is a diff over the POV's repo between two of its own sequence
  numbers, valid while the POV is paused (§4.2).
- "Diff the global POV between two versions" is §5.1's diff.
- #746's "leave a hook for save points" is §4.3.

### 9.10 Exactly-once writes (#733) and durable acks (#755)

- #733's operation record is committed with the write, so it carries the write's version; a retried
  write's saved outcome can include "committed at version V", which a client can then read at.
- #755's receipt states "readable now at version V" and "durable through version D"; both are global
  versions in this design's sense once the write is promoted. A safe POV's write is acknowledged
  before promotion, so its receipt's V is a vector (§4.2) until promotion.
- **Format.** #755 adds a log file kind and recovery; A adds retention metadata and save points. Both
  need a store format version. Ship them under one bump (§11).

### 9.11 #592 / #602 pruning

The tail rule becomes the retention rule with H = now when retention is off, so the default behaviour
is #602's, byte for byte. `--prune_merge_history=false` (keep every `Assign` version) is what §8
measured as unbounded retention under A; it should stay a diagnostic flag, not a policy. The fold
change (§5.1) closes the hole where one `+=` in a merged file dropped the history of every key in it.

### 9.12 #796 cross-repo ordering

#796 orders one key's entries by repo, nearest the POV first, then by sequence number within a repo.
A version vector filters each repo separately, so it composes with that ordering without ever
comparing two repos' numbers. #796 should merge before any of this is implemented.

## 10. API surface

### 10.1 Protocol (WebSocket and binary)

| Statement | Result |
|---|---|
| `try {pov} at <version> <pkg> <method> <args>;` | the method's result, every read as of the version; a method with effects is refused (`version_read_only`) |
| `try {pov} at save "<name>" ...;` / `at time "<RFC 3339>" ...;` | as above, resolving the name or time first |
| `version of {pov};` | the POV's current version: a number for the global POV, a vector string otherwise |
| `hold version {pov} <version> for <seconds>;` | a lease id; `release <lease-id>;` drops it |
| `save point {pov} "<name>" [at <version>];` / `drop save point {pov} "<name>";` | the version saved |
| `retention {pov} (last <N> versions \| <seconds> seconds \| default);` | the resulting horizon (`everything` arrives with B) |

Every `try` reply gains `"version": V`, the version the call read at (and for a write, the version it
committed at in its POV). It is additive JSON and costs nothing to ignore.

A read outside retention replies `"status": "version_unavailable"` with
`{"requested": V, "oldest": H}` in `result`. It is not retryable as sent. Over the binary protocol the
message starts with `version unavailable`.

### 10.2 Orlyscript

Versions are values: a method takes one as an ordinary `given::(int)` argument. Two additions:
- `keys (T) @ <[pattern]> changed between (v1, v2)`: the keys in the pattern whose value differs
  between two versions, as a sequence, with the same `after`/`from` bounds as #793.
- `*<[key]>::(T) at v`: a single read of one key at a version inside a method that otherwise reads
  current, for "what changed" methods that show the old and new values side by side.

The whole-call `at` in the protocol is the main surface; the in-language forms are for diffs.

### 10.3 Clients

TS/Python/Go: `call(pov, pkg, method, args, { at })`, `versionOf(pov)`, `hold(pov, version, secs)`,
`savePoint(pov, name)`, and `pages()` pinning the first page's version. The MCP server exposes `at` on
its read tool.

## 11. Format change and migration

**There is no store format version today.** A device has a magic number (`TDeviceUtil::OrlyFSMagicNumber`)
and nothing else identifies the layout, so a 0.2.x binary would open any later store and interpret it
as 0.2.x.

**A, by itself, changes no file layout.** Retained history is history-region entries, which 0.2.x
writes today under `--prune_merge_history=false`; keyframes are ordinary `Assign` entries. So:
- a 0.3 store opened by 0.2.x reads current values correctly (the `kept` runs in §7 read their
  current values through today's read path), and 0.2.x's merges drop the retained history as they go:
  a downgrade loses history, not data;
- a 0.2.x store opened by 0.3 has a horizon of "now" everywhere: versions start at the upgrade, older
  versions are `version_unavailable`, and nothing needs rewriting.

What A adds is **metadata**: the policy, the horizon, save points and the (time, version) samples, per
repo. It needs a home that survives restart and is shipped to slaves; the durable manager is one, a
small metadata file per repo is another.

**#755 is different:** a log file kind that recovery must replay. A 0.2.x binary opening a 0.3 store
with an unreplayed log would silently lose acknowledged writes. So the format bump must:
1. add a **store format version** in the device's system block (`TDeviceUtil` has unused slots after
   `MinDiscardBlocksPos`); 0.2.x stores read as version 0;
2. make 0.3 refuse a store whose version is newer than it knows;
3. ship a 0.2.x patch release that refuses a nonzero format version, before 0.3, so a downgrade fails
   loudly instead of losing writes. (How 0.2.x treats an unrecognised device today, and whether
   `--create=true` could reformat it, must be checked before choosing between a new slot and a new magic
   number; I have not verified it.)

Bundle A's metadata, the format version and #755's log into that one change.

## 12. Recommendation

**The version core, whichever storage follows:**
- a version is the global POV's sequence number, exposed as an opaque increasing number;
- non-global POVs get version vectors, valid until promotion or while paused, and snapshot handles
  for short consistent reads;
- a read at V finds each key's newest entry at or before V by a galloping search of its history run,
  and skips files entirely newer than V;
- leases and continuations pin a version; save points name one;
- `try ... at`, `version of`, `hold version`, `save point`, `changed between`, `version_unavailable`.

**Retention storage: Option A, for bounded retention.**
- The merge keeps everything above the horizon plus each key's newest version at or below it.
- The fold keeps deltas above the horizon, with a keyframe every 64 deltas, and keeps update
  metadata. The disk walker stops replaying a `+=` chain at its first `Assign`.
- Policies: the last N versions, a time window, save points and leases. The default stays #602's.
  No "everything" under A.
- The master decides the horizon and replicates it.
- One format bump, shared with #755.

**Option B, later, for unbounded retention.** Do it when a workload needs "keep everything" on
overwritten data, as its own design and approval. The core and the API don't change. A store that
used A's windows moves to B by letting the merge send retained history to history files instead of
keeping it.

**Why not A for everything:** measured, either its merges rewrite retained history over and over (3.5×
the merge bytes at 32 versions per key, 6.8× at 256, growing with the square of the history) or,
with generations sized by bytes, current reads visit more files (+31% point, +81% range). The
issue's first cost target is that current reads cost what they do today.

**Why not B now:** it is the largest and riskiest piece (a new file kind, a second merge output, a
partition manager, reload and slave-join support, fault-injection coverage), and it is a hard one-way
format change. Bounded windows cover paging, diffs, review save points and "what did this say last
week". Unbounded history can follow once someone needs it.

## 13. Staged plan

Each stage is a separate PR with its own tests. Stages 0–2 add no file kind and no persisted
metadata: an older binary opens the store, reads the right current values, and at worst drops
retained history.

0. **Prerequisites.** #796 (cross-repo ordering) and #793 (keyset paging), both open. Make the fold
   streaming (§9.6), which is worth doing anyway.
1. **Read at a version (two-way).** `TPresentWalkFile` learns "newest entry at or before V" with a
   galloping search; files entirely newer than V are skipped; `try ... at V` reads through the global
   POV. Retention is still #602's, so the server tracks the highest sequence number any pruning merge
   has covered and refuses older versions with `version_unavailable`: in practice, the last seconds of
   writes plus whatever is still in memory. Snapshot handles (pinned views, §4.4) give paging a
   consistent snapshot on any POV. Tests: a differential test against a model that records every
   write, reading every version of a randomized workload, point and range, current and old, across
   merges; a negative control that reads below the horizon and must be refused, never answered.
2. **Bounded retention in the merge (two-way on disk).** The retention rule in `TMergeDataFileImpl`
   (H = min(policy, oldest lease)), keyframes and kept metadata in the fold, the walker that stops a
   `+=` replay at its first `Assign`, in-memory leases for continuations, and the policy as server
   flags (`--retention_versions`, `--retention_seconds`) that master and slaves must share. Tests: the
   merge differential test with a horizon; `+=` at every retained version across merges and folds;
   power loss mid-merge with retained history (fault-injection harness); the kill campaign with
   retention on; a 0.2.x binary reading the store's current values; write volume and layer count
   against #602 on the ab-bench smoke, with the window sized to the live data.

   **APPROVAL POINT.** Before stage 3, the maintainer approves: the public version contract (§4),
   the default and the policies (§1, decisions 1 and 3), the store format version (§11) and its
   bundling with #755. Stage 3 is the first stage whose stores an older binary must refuse.

3. **Persisted retention, save points, format version (one-way).** Per-repo retention metadata (policy,
   horizon, save points, (time, version) samples), the horizon shipped in the replication stream, the
   store format version, and #755's log in the same bump. Tests: failover keeps every version inside
   the horizon; a restart keeps save points; an older binary refuses the store.
4. **Diff and time (two-way on top of 3).** `changed between`, `at time`, `version of`, #746's
   save-point hook.
5. **Clients and docs.** TS/Python/Go/MCP, `docs/PROTOCOL.md`, a rewritten time-travel section in
   `docs/architecture.md`, and a walkthrough on the grc20-pov model.
6. **Option B, only when needed** (its own design and approval): history files for unbounded
   retention.

## 14. Open questions

- Is a keyframe interval of 64 right? It bounds a `+=` read at any version to 64 deltas per file; a
  hot counter written 10,000 times inside retention costs 157 keyframes, each the size of the value.
  For set unions (`|=`) a keyframe is the whole set; the interval should probably be in bytes, not
  deltas.
- Should retention be allowed on a non-global safe POV that is a root (no parent)? Only the global POV
  is a root today.
- Should `at` be allowed on writes ("write as if at V")? No use case; refuse.
