# Design: durable acknowledgments for safe POVs (#755)

> Status: **proposal, awaiting a decision.** No engine code is part of this design. The
> implementation adds an on-disk log, changes what a restart restores and changes what an
> acknowledgment means, so it is a one-way door and needs explicit approval at the point marked
> in [§11](#11-staged-plan). It shares one store format change with versioned reads (#745,
> [`versioned-reads.md`](versioned-reads.md), PR #803). Line numbers are omitted; search by type
> or function name.

## 1. Summary

**Today** (`docs/durability.md`, #730): `"status": "ok"` means the transaction committed **in
memory** to the POV it named. A write reaches disk only after Tetris promotes it to the global POV
and the global POV's next memory merge writes a data file. Under load that is seconds after the
acknowledgment (the kill campaign lost up to 11,153 acknowledged writes in one kill, the oldest
acknowledged 3.9 s earlier). Safe and fast POVs behave the same, and POVs don't survive a restart
(#439).

**Goal.** An `ok` from a safe POV means the write survives a crash, at a measured small latency
cost. Fast POVs keep memory-speed acknowledgments with a stated loss bound.

**Recommendation: a write-ahead log with pipelined group commit, and durable safe POVs**
([§4.1](#41-option-a-recommended-wal--group-commit-with-durable-safe-povs)).

- Every transaction that touches a **safe** repo (a safe POV's write, a promotion into a safe
  parent or into the global POV, a pause, a POV's creation) is appended to one log, in commit
  order, as the record the replication stream already encodes for it. A safe POV's write is
  acknowledged when its group is synced and sealed. Fast POVs are not logged.
- One sync covers a whole group; while one group syncs the next one forms, and a group's seal
  rides on the next group's write, so under load each group costs one sync.
- Recovery replays the log from the last checkpoint through the slave's apply path, which already
  discards what the data files hold (`ensure_or_discard`). A safe POV comes back with its id,
  parent, sequence numbers and unpromoted backlog, so a receipt's version is still valid after a
  restart.
- The log is a preallocated ring on the volume, bounded by the same quantity that bounds today's
  loss (the Update pool), so restart time stays bounded.

**What I measured** ([§10](#10-measurements); every number names its hardware):

- A sync of one 4 KiB `O_DIRECT` write, the way `TPersistentDevice::Sync()` does it (`fsync` +
  `BLKFLSBUF`) on a loop volume: **p50 0.8–1.0 ms, p99 3.3–7.8 ms** in the OrbStack Linux VM on an
  M3 Max; **p50 0.24–0.27 ms, p99 0.31–0.41 ms** on a GitHub x86 runner. Both are floors, not
  durability claims: the M3 Max's own SSD takes 5.1 ms (p50) for a real cache flush
  (`F_FULLFSYNC`), far more than the VM's sync, and on the runner the sync of a preallocated file
  sent no flush to the device at all. With another writer streaming to the same disk, the sync rose
  to **20 ms p50, 126 ms p99** in the VM and to **9.4 ms p90** on the runner: tail latency comes
  from the device, not the design.
- An `orlyi` prototype that logs each write's record after commit and parks the writer's fiber until
  its group is synced (scratch branch, never pushed): one writer's commit went from **p50 0.40–0.51
  ms to 0.76–1.05 ms** (p99 3.2–3.5 → 4.8–5.6 ms), about one sync, in the VM, and from **p50
  0.30–0.31 ms to 0.55–0.57 ms** (p99 1.0–1.4 → 3.5 ms) on a CI x86 runner
  ([§10.3](#103-end-to-end-today-vs-the-prototype-in-orlyi)). The prototype has no seal; the seal
  adds about one more sync ([§5.3](#53-payload-then-seal)).
- With 8 and 64 writers today's throughput in the VM is not set by the commit path: writers outrun
  promotion and spend their time in the backlog cap and admission refusals (p99 170–440 ms at 64
  writers). The prototype's wait paced the writers and their throughput went **up**, from 550–1,000
  to 1,000–5,000 writes/s (on a CI runner, from 1,650–2,300 to 1,900–4,200). I don't count that as a
  win (it is the box, not the log), but it does mean the log is not the bottleneck: the standalone
  log sustained 28,000–35,000 commits/s at 64 writers in the VM, and 115,000–119,000 on the CI
  runner.

### The decisions to make

1. **Approach.** I recommend the WAL with **durable safe POVs** (option A). The alternative that
   avoids durable POV identity, logging only writes that go straight to the global POV (option B),
   would make "safe" mean nothing for the way applications actually write: through POVs.
2. **What a safe POV's default is.** I recommend that a safe POV's `ok` waits for the log by default
   (`local`, below): that is the meaning "safe" has always implied. A per-call override can lower it
   (`logged`: written to the log, not waited for, loss bound a few milliseconds) or ask for
   `memory`. Fast POVs stay at `memory` and can't be raised.
3. **Seal strictness.** I recommend the issue's rule as written: **ack only after the seal is on
   disk**, with the seal piggybacked on the next group so it costs no extra sync under load. It adds
   one sync's latency to every acknowledgment (measured +0.15 ms at p50 on the CI x86 runner,
   +0.7–0.9 ms in the VM); the log still sustains 70,000 sealed commits/s at 64 writers on the
   runner, far above what `orlyi` commits. The cheaper alternative (ack after the payload's sync,
   seal later) leaves a small window where a damaged last group is indistinguishable from a torn one
   and would be truncated silently ([§5.3](#53-payload-then-seal)).
4. **A write is readable before it is durable.** I recommend accepting this, as PostgreSQL does with
   asynchronous commit: for the few milliseconds of a group, another client can read a safe write
   that a crash then takes back. The log's order still guarantees that a surviving safe write never
   depends on a lost safe one
   ([§4.1](#41-option-a-recommended-wal--group-commit-with-durable-safe-povs)). Holding visibility
   back until the sync would touch every read path.
5. **A safe POV under a fast parent.** Its writes would stop being durable once promoted into the
   fast parent. I recommend refusing `local` and `logged` on such a POV (typed
   `durability_unavailable`) rather than silently downgrading.
6. **Replication in v1.** I recommend `local` only: an acknowledgment survives a crash and restart of
   the master, not a failover to a slave that hadn't received it yet. A `replica` level (wait for
   the slave) comes later ([§8.4](#84-replication)).
7. **Format.** One store format change, shared with #745 stage 3, and a 0.2.x patch release that
   refuses the new format before it ships ([§9](#9-format-and-compatibility)).
8. **A real NVMe number before stage 2.** The VM and CI disks are virtualised. I'd like approval to
   run the standalone benchmark and the prototype on a cloud machine with local NVMe (about an hour
   of machine time) before the format is fixed.

## 2. Requirements

From the issue and its review comments:

| # | Requirement | Where |
|---|---|---|
| R1 | Payload, then seal: a sealed record with a bad payload is corruption; an unsealed tail is truncated only after proving nothing sealed follows it | [§5.3](#53-payload-then-seal), [§6](#6-recovery) |
| R2 | Pipelined group commit: one sync per group, two groups in flight | [§5.4](#54-group-commit), measured [§10.2](#102-the-log-alone-group-commit-prototype) |
| R3 | Tiers per POV: safe POVs reply after their group is synced; fast POVs keep memory-speed replies with a stated loss bound | [§7](#7-api) |
| R4 | Receipts (#750) state "readable now at V" and "durable through D", D ≤ V | [§7.3](#73-receipts-750) |
| R5 | Exactly-once (#733): the operation id and saved outcome commit in the same log record as the write | [§8.2](#82-exactly-once-733) |
| R6 | An I/O error is not proof of non-write: never tell a client "not written" after an uncertain sync; fail and let recovery decide | [§5.7](#57-io-errors), [§6](#6-recovery) |
| R7 | Recovery from a checkpoint plus the log suffix; log truncated once global's data files hold the writes; bounded restart time | [§5.6](#56-checkpoints-truncation-and-recycling), [§6](#6-recovery), [§8.6](#86-restart-time) |
| R8 | Measure the sync's real share of commit latency, quiet and busy disk | [§10](#10-measurements) |
| R9 | Fault every edge (append, payload sync, seal write and sync, reply, "completed then errored"); kill campaign `durable` mode with zero acknowledged safe writes lost and a negative control | [§8.7](#87-the-fault-model), [§8.8](#88-proof-fault-harness-and-kill-campaign) |
| R10 | Where replayed writes go given ephemeral POVs (#439); replication contract; admission (#590/#607/#721); restart time; format bundled with #745 | [§4](#4-options), [§8](#8-interactions), [§9](#9-format-and-compatibility) |

**Headline targets** (from the issue's review; to be measured, not assumed):

| Target | Status |
|---|---|
| Safe-POV commit p99 within ~1–2 syncs of today's | met at 1 writer, without the seal: +1.5–2.2 ms at p99 in the VM (a VM sync's p99 is 3.3–7.8 ms) and +2.1–2.5 ms on a CI x86 runner whose disk's sync I didn't measure (its commit p90 rose to ~3 ms, about one sync) |
| Throughput at 64 writers within ~10% of today's | met in the VM and on CI: at 64 writers the prototype did 1,900–5,000 writes/s against today's 550–820 (VM) and 1,650–1,950 (CI); the log alone sustains 70,000–119,000 commits/s on a CI runner |
| Zero acknowledged safe writes lost, kill campaign and a simulated storage-fault campaign | the stage 2 gate ([§8.8](#88-proof-fault-harness-and-kill-campaign)) |
| Bounded restart time at any log size | bounded by design (ring size, checkpoint); replay rate is a stage 1 measurement ([§8.6](#86-restart-time)) |

## 3. The current path

### 3.1 A write

1. **Commit.** `TSession::Try` (and the batch path) runs the method, checks admission (disk #590,
   backlog #721, memory #607), builds the update and commits a `TTransaction`. The commit point is
   the transaction's destructor: `~TPusher` calls `TRepo::AppendUpdate`, which inserts the update
   into the POV repo's current memory layer and assigns it the repo's next sequence number. The
   session replies `ok`. Nothing waits for disk.
2. **Replication record.** In the same destructor, under the manager's replication queue lock,
   the transaction's `TReplica` (its pushes, pops and status changes, each with its repo id and
   sequence number) is enqueued for the replication stream. All user transactions are enqueued,
   fast POVs' included. So there is already **one place where every committed transaction passes
   in commit order, already serialised** (`TReplicationStreamer` encodes it for slaves).
3. **Promotion.** Tetris moves a POV's updates to its parent one transaction at a time: push into
   the parent, pop from the child, one transaction (and one replica).
4. **Flush.** Only the global POV writes data. Its memory merge (`TRepo::StepMergeMem`, every
   `--mem_interval` ms) writes sealed layers to a data file, syncs the file's blocks, then the file
   service appends the file to the file map and syncs that sector (`TDataFile`): two syncs per
   flush. Then it fires each update's `TPersistenceNotification`, which is copied along with the
   update through every promotion, so a transaction's `TTransactionCompletion` learns when its
   write reached a data file. No client is told (#750 would use this).
5. **Restart.** The global POV is rebuilt from the file map. POV records reload but are refused
   (`pov.cc`: "povs are ephemeral, #439"); their unpromoted writes are gone.

### 3.2 How Orly writes to disk

- `TPersistentDevice` opens the block device `O_RDWR | O_DIRECT` and submits I/O through libaio.
  There is no file system in between: Orly allocates blocks itself (`TVolumeManager`), and its file
  map lives in the file service's two alternating base images plus an append-log ring of sectors.
- `TPersistentDevice::Sync()` is `fsync(fd)` then `ioctl(BLKFLSBUF)`, on whatever thread calls it,
  if `--do_fsync` is on. With `O_DIRECT`, `BLKFLSBUF` (drop the block device's buffer cache) does
  nothing useful and measured 0.1–0.4 ms extra at p50 ([§10.1](#101-sync-latency)).
- After a failed sync the file service refuses every later file-map change until restart (#621):
  the fsyncgate rule, already applied.
- `--mem_sim` (the Docker image's default) keeps the "disk" in memory. Nothing is durable there,
  and durable acknowledgments must say so ([§7.1](#71-levels)).

### 3.3 What already exists that this design reuses

| Need | Exists |
|---|---|
| A total commit order across repos | the replication queue lock in `~TTransaction` |
| A record of a transaction's effects, with sequence numbers | `TTransaction::TReplica`, encoded by `TReplicationStreamer` |
| Idempotent re-application against what's on disk | the slave's `TManager::TSlave::ApplyCoreVectorTransactions` with `ensure_or_discard`; it already skips mutations on repos that aren't live |
| A POV's identity on disk | `TPov` is a durable object (id, parent, policy, audience, TTL) saved by the durable manager every 40 ms; #439 refuses it after restart |
| A repo's creation in the stream | `TRepoReplication` (id, safe, TTL, parent) |
| "This update reached a data file" | `TUpdate::TPersistenceNotification` |
| The fsyncgate rule | #621: refuse after a failed sync, never retry |
| Alternating checkpoint slots | the file service's two base images |
| Power-loss and torn-sector injection | the fault device and harness (#608, `fault_injection.test.cc`) |

## 4. Options

### 4.1 Option A (recommended): WAL + group commit with durable safe POVs

**What is logged.** Every committed transaction with at least one mutation on a **safe** repo:

- a safe POV's own write (a push into its repo);
- a promotion into a safe parent, including the global POV (push into the parent, pop from the
  child; when the child is fast its pop is not logged, and replay skips it as the slave does);
- pause, unpause, fail and expiry of a safe repo;
- a safe POV's creation (its `TRepoReplication` and `TPov` fields) and its close;
- the exactly-once operation record, inside the write's update metadata ([§8.2](#82-exactly-once-733)).

Fast POVs' own writes are not logged. A fast write becomes durable when its promotion into a safe
repo is logged and that group syncs, or when it reaches a data file.

**When a safe write is acknowledged.** The session commits as today, then (outside every lock)
parks its fiber on the log's durable-LSN until its record's group is synced **and** sealed
([§5.3](#53-payload-then-seal)), then replies. The log appends inside `~TTransaction` under the
replication queue lock that already serialises commit order, so the log's order is the commit
order.

**Visibility before durability.** The update is in the memory layer before its group syncs, so
another reader of the same POV, or of an ancestor after a promotion, can read it for those few
milliseconds. A crash can take back a write someone read. It cannot keep a safe write and lose a
safe write it was derived from: X became visible inside its transaction's commit, under the lock
that also appends X's record, so a write that read X commits and appends after it, and a crash keeps
a prefix of the log. (A fast POV's writes aren't logged, so this says nothing about them.) Only an
action outside Orly (an email sent because of a read) can depend on a write that is later lost; a
receipt's "durable through" lets a client check first ([§7.3](#73-receipts-750)). PostgreSQL with
`synchronous_commit = off` makes the same trade; with `on` it holds the transaction invisible until
its commit record is flushed. Doing that in Orly means a durability fence in every view
(`TRepo::TView` would cap `HighestSeqNum` at the durable point), in Tetris (which must not promote
what isn't durable) and in the read-your-writes path. I don't think it is worth it for v1; it can be
added as a fence later without a format change.

**The write-ahead rule.** A data file must never hold an update whose log records (its write and
the promotion that put it in the global POV) aren't durable yet. Otherwise a crash could leave a
promoted update in a data file while the log still holds its write in the child's backlog, and
replay would promote it a second time: a `+=` applied twice. So the global POV's memory merge
flushes only up to the highest global sequence number whose promotion record's LSN is ≤ the log's
durable LSN. Group commit lags by milliseconds and the merge runs every 40 ms, so in practice this
never delays a flush; it is the classic ARIES rule (log before data). Replay also checks each child
push's update id against the global POV's recent update ids, as a second line of defence, and
counts any match as an integrity event.

**Durable POV identity.** A safe POV's creation is logged before its first write can be
acknowledged (the same group, or an earlier one). After a restart the POV exists with the same id,
parent, policy, audience, sequence numbers and TTL remaining, and its unpromoted backlog is back in
its repo; Tetris resumes promoting it. Most of the hook exists: a `TPov` reloaded from the durable
manager already attaches to its repo if one is live (`TPov::GetRepo`'s `Reloaded` path, written for
a promoted slave, where replication built the repo) and refuses only when there is none. Replay
builds the repo, so the reloaded POV attaches. The `POV` record covers the one gap: the durable
manager saves objects every 40 ms, so a POV created and written in less than that has no saved
`TPov` yet, and recovery recreates it from the record. #439's refusal remains for fast POVs (and for
safe POVs on a store or level without the log). Sessions are durable objects already (the kill
campaign resumes them).

**Graceful stop gets simpler.** A safe POV's backlog no longer has to be promoted before the
server stops: it is in the log. #769 (PR #802) makes the stop wait for Tetris while it makes
progress, up to `--stop_promote_budget_s`; with durable POVs that wait is needed only for fast POVs'
backlogs, and a stop with only safe POVs takes the flush and a checkpoint.

**Costs.** One log record per safe transaction (encoded before the commit, linked into the group
under the lock that already serialises commits, [§5.5](#55-where-the-append-happens)); one sync per
group; a wait of one or two group intervals for safe writers; promotions into safe repos are logged
too, so a safe write's bytes are written to the log twice (once as the write, once as its promotion
into the global POV), then once more in the data file. That is the price of restoring the child POV
exactly; option A′ below avoids it at a cost.

**Variant A′: log promotions by reference.** Log a promotion as "pop child seq s, push it into the
parent as seq p" without the update body, and rebuild the parent's update from the child's logged
write at replay. It halves log bytes for safe POVs, but couples the two records' lifetimes (the
child's record can't be recycled while the parent's reference needs it) and makes the replay path
differ from the slave's. Not for v1; measure log bandwidth first.

### 4.2 Option B: log only writes that go straight to the global POV

Log only transactions that push into the global POV: writes made directly in the global POV and
Tetris's promotions into it. Acknowledge a write made **directly in the global POV** after its
group syncs. No durable POV identity is needed, because nothing below the global POV is restored.

- **For writes through a POV it changes nothing**: the write is durable only once promoted, which
  is the seconds-long backlog under load. Applications write through POVs (every client example and
  the kill campaign do), so "safe" would still promise nothing for them.
- It is much smaller to build: no POV records, no checkpointed POV table, no copy-forward
  ([§5.6](#56-checkpoints-truncation-and-recycling)), and replay only feeds the global POV.
- It is a subset of A: the record format, group commit, seal and recovery are the same. A could be
  built as B first if POV identity turned out harder than expected (stage 2a in
  [§11](#11-staged-plan)).

### 4.3 Option C: acknowledge after promotion and flush (#750's wait-for-flush)

Reply when the update's `TPersistenceNotification` fires: promoted to the global POV and written to
a data file. No log, no format change; the notification machinery exists.

- **Latency is the promotion backlog plus up to one merge interval plus two syncs.** At idle that is
  about half of `--mem_interval` (40 ms) plus the data file's two syncs: roughly 20–45 ms. Under
  load it is the backlog: the kill campaign's oldest lost write had been acknowledged 3.9 s before
  the kill, so a wait-for-flush reply at that load takes up to ~4 s.
- Throughput: the waiting writers hold connections, not pool space, so throughput is unchanged, but
  a writer that waits per write is limited to one write per flush interval.
- It is still the right **opt-in** for fast POVs that want "tell me when it's on disk" (#750), and it
  costs nothing to keep.

### 4.4 Option D: synchronous merge (write a data file per acknowledgment)

Make the global POV's memory merge synchronous with the commit: a safe write (after promotion) is
acknowledged when a data file containing it is written and synced, flushing on demand rather than
every 40 ms.

- Every flush is a data file (index, hash table, keys, history) plus a file-map sector: at least
  **two syncs**, and with no grouping across POVs, because child POVs don't write data files at all.
  The best case is the measured "two syncs in series" row: about 530–630 commits/s per stream in the
  VM ([§10.2](#102-the-log-alone-group-commit-prototype)), with promotion still in front of it.
- One small data file per flush multiplies disk merges (#602's tail merges, the fold), which
  [`versioned-reads.md`](versioned-reads.md) measured to be the expensive part of the storage engine.
- It still doesn't make a write durable before promotion, so it has option C's latency under load.

### 4.5 Against the requirements

| | A: WAL + durable safe POVs | B: WAL, global writes only | C: wait for flush | D: synchronous merge |
|---|---|---|---|---|
| Safe POV `ok` survives a crash | **yes** | only writes made directly in global | yes, after seconds | yes, after promotion |
| Latency added at low load | one or two syncs | one or two syncs (global writes) | ~20–45 ms | promotion + two syncs |
| Latency added under load | one or two syncs (group commit) | backlog for POV writes | the backlog: seconds | the backlog: seconds |
| Throughput at 64 writers | today's (log sustains 28k–35k/s in the VM) | today's | today's | ≤ ~600/s per stream |
| POVs survive restart | safe POVs | no | no | no |
| Exactly-once record durable with the write (#733) | yes, same record | only for global writes | at flush | at flush |
| Receipts "durable through D" (#750) | D ≈ V within ms | D far behind V for POV writes | D far behind V | D far behind V |
| Graceful stop | no Tetris drain for safe POVs | unchanged | unchanged | unchanged |
| New code | log, recovery, POV restore, checkpoint | log, recovery | none (notifications exist) | merge path rework |
| Format change | yes (one bump, with #745) | yes | no | no |

### 4.6 Not considered further

- **Per-write sync of the data files** (no grouping): option D without even the batching of a merge.
- **No log, LMDB-style.** LMDB has no log: a commit writes new copy-on-write B+tree pages, syncs them,
  then flips one of two meta pages and syncs again. That gives an atomic commit with two syncs and no
  recovery work. It suits a single B+tree. Orly's data lives in many POV repos, in memory layers that
  are merged into immutable files in the background; there is no single root to flip per commit, and
  making each commit write its own pages is option D. The useful idea is the two alternating meta
  pages, which the file service already uses for its base images and which the log's checkpoints use
  ([§5.6](#56-checkpoints-truncation-and-recycling)).

## 5. The log

### 5.1 Placement

- **On the volume, in a reserved contiguous block range,** allocated once when the store is created
  or upgraded and recorded in the device's system block (the slots after `MinDiscardBlocksPos` in
  `TDeviceUtil`), together with the store format version ([§9](#9-format-and-compatibility)) and the
  two checkpoint slots. No allocation happens at run time, so the log can't hit #590's out-of-space
  paths, and nothing about it lives in the file map (whose own changes are synced separately).
- **Written in place as a ring** of 4 KiB-aligned groups. Overwriting blocks that already exist
  means a sync has no allocation metadata to flush. On a raw device there is no file system at all;
  on a loop volume the backing file must be fully allocated, so `orly_dm --create-volume` (and the
  upgrade) write the log range once rather than leaving it sparse. RocksDB recycles preallocated WAL
  files for the same reason (`recycle_log_file_num`).
- **Size:** `--wal_mb`, default the larger of 256 MiB and twice the bytes the Update and Update
  Entry pools can hold, so the log never fills before memory admission refuses
  ([§8.3](#83-admission-590-607-721)).
- **A separate device later** (`--wal_device`), for isolation from merge traffic: the busy-disk
  numbers in [§10.1](#101-sync-latency) are the case for it. Not in v1.
- **Not in `--mem_sim`.** There is no disk to make anything durable on; safe POVs report `memory`
  ([§7.1](#71-levels)).

### 5.2 Format

All integers little-endian. A **group** is one write: a 64-byte header, the records, zero padding
to 4 KiB.

| Group header field | Bytes | Purpose |
|---|---:|---|
| magic `ORLYWAL1` | 8 | not a log group |
| format version | 2 | |
| flags | 2 | seal-only group, checkpoint marker |
| store id | 16 | the instance's UUID: a write misdirected from another store or ring is rejected |
| lap | 4 | incremented each time the ring wraps: a stale group from the previous lap is rejected (RocksDB's recyclable record type carries the log number for the same reason) |
| group number | 8 | consecutive across laps |
| first LSN, record count | 8 + 4 | |
| payload length | 4 | |
| sealed through | 8 | every record with LSN ≤ this was synced before this group was written ([§5.3](#53-payload-then-seal)) |
| chain | 8 | the previous group's checksum: a lost or misdirected group breaks the chain (TigerBeetle's parent checksum, SQLite's cumulative WAL checksum) |
| payload checksum | 8 | over the records |
| header checksum | 8 | over all of the above (last field) |

A **record**: length (4), type (1), flags (1), reserved (2), LSN (8), CRC32C of the body (4), body.
Types:

| Type | Body |
|---|---|
| `TXN` | the transaction's replica as `TReplicationStreamer` encodes it: per mutation, push/pop/fail/pause/unpause, repo id, sequence number, and for a push the update (its entries, its metadata key holding the `TMetaRecord`, its update id) |
| `POV` | create or close: id, parent, safe/fast, audience, TTL and its expiry time |
| `SEAL` | (in a seal-only group) nothing: the header's "sealed through" is the content |
| `META` | #745's persisted metadata changes: retention policy, horizon advance, save point create/drop ([§9.2](#92-bundled-with-745)) |

Checksums: CRC32C per record (hardware on x86 and arm64) and a 64-bit hash for the payload, header
and chain (XXH3-64 or two CRC32C lanes; the choice is a stage 1 detail). TigerBeetle uses
128-bit checksums throughout. A log that is recycled every few seconds to minutes can defend a
narrower one, but the width is cheap to change before the format is fixed and costly after.

### 5.3 Payload, then seal

The issue's rule: a record is written and synced, then a seal is written and synced after it. On
recovery a sealed record with a bad payload is corruption; an unsealed tail is truncated only after
proving nothing sealed follows it.

**Why the seal matters.** Checksums alone can't tell a torn write (never acknowledged, safe to drop)
from an acknowledged record that the device later lost or damaged (must not be dropped silently).
With up to two groups in flight, a crash can persist group k+1 while tearing group k, so "a valid
group follows" doesn't prove k was ever synced either. The seal is a statement written strictly
after a sync: "everything through LSN L was on the device before I was written." If a group fails
its checks and a later valid group seals it, it was acknowledged and is now damaged: recovery
refuses to start ([§6](#6-recovery)). If nothing seals it, it was never acknowledged: recovery
truncates it.

**The seal rides on the next group.** Each group's header carries "sealed through" = the highest
LSN whose sync had completed when the group was built. A record is acknowledged when a sync covering
a later group that seals it completes. Under load groups are back to back, so this costs no extra
sync, only up to one group interval of latency. When the log goes quiet with unsealed records, the
leader writes a seal-only group at once.

**Cost, measured** (standalone log, [§10.2](#102-the-log-alone-group-commit-prototype)). On the CI
x86 runner, sealing makes one writer's acknowledgment two syncs instead of one: p50 147–161 →
303–310 µs, p99 288–387 → 454–674 µs. Under load the seal adds no syncs, but every record now waits
for the sync after its own, so a writer's cycle is two syncs long: with the same number of closed-
loop writers, 37–47% fewer commits/s (17,700–18,700 against 30,800–33,200 at 8 writers;
69,700–75,200 against 115,600–119,100 at 64). That is still 25–30 times what `orlyi` committed end
to end on the same runner (2,400–3,000 writes/s,
[§10.3](#103-end-to-end-today-vs-the-prototype-in-orlyi)), so the seal's cost is latency, about one
sync, not throughput. In the VM the same comparison was p50 0.87–0.93 → 1.66–1.84 ms at one writer.
TigerBeetle reaches the same distinction with a second, redundant ring of headers next to its ring
of prepares; the seal in the next header is the same idea with one ring.

**The alternative** (decision 3): acknowledge after the payload's sync and let the seal follow. That
saves one sync at low load. The cost is a window: if the last one or two acknowledged groups are
damaged or lost by the device before the next group seals them, recovery can't tell them from a torn
tail and truncates them, losing acknowledged writes without a word. It needs a device fault in the
milliseconds before a crash, so it is rare, but it is exactly the silent loss the kill campaign's
`durable` mode exists to rule out.

### 5.4 Group commit

- **Leader.** One dedicated OS thread (not a fiber runner: a sync blocks the thread that calls it,
  and today's `TPersistentDevice::Sync()` is called from fiber runners only off the commit path)
  takes the current group buffer whenever it is non-empty and fewer than two groups are in flight,
  builds the header, and writes it with `O_DIRECT` at the ring position.
- **Syncers.** Two OS threads. A group's sync is issued after its own write and every earlier write
  completed, so when it returns every group up to it is on the device. Durable LSN advances to the
  group's sealed-through value; waiting fibers whose LSN is covered are woken (`TSingleSem::Push`
  from a non-runner thread, as the prototype does).
- **No timer.** Like RocksDB's leader-follower write group, a group is whatever accumulated while
  the previous sync ran, so there is no added delay at low load. PostgreSQL's `commit_delay` /
  `commit_siblings` add a deliberate wait when enough transactions are active; worth an option only
  if a measurement shows groups too small under moderate load.
- **Two in flight.** On the CI x86 runner a second group in flight gave 1.6–1.9× the commits/s at
  8 and 64 writers (30,800–33,200 against 16,100–19,800 at 8; 115,600–119,100 against
  66,400–78,600 at 64) at a lower p50, as the issue predicts. In the shared VM it didn't help and
  sometimes hurt (8 writers: 666–4,529 with two, 1,071–5,210 with one), which I put down to that
  box: the same configuration varied 5× between repetitions there. Two is the default; the depth
  is a setting, and the NVMe run confirms it.
- **Sync call.** `fdatasync` on the `O_DIRECT` block device fd, without `BLKFLSBUF`. On a raw device
  there is no metadata, so `fsync` and `fdatasync` are the same device flush.

### 5.5 Where the append happens

The replica is built in `TTransaction::Prepare`, before the commit and outside any shared lock;
encode the log record there (the replication stream's encoding, shared), except for the sequence
numbers, which are assigned at commit and patched in. Then in `~TTransaction`, under the replication
queue lock that already serialises commits, after the pushers have committed: take the next LSN and
link the encoded record into the current group. That fixes the log order as the commit order and
adds a counter increment and a list append to the lock, not an encoding. (PostgreSQL reserves WAL
space under a spinlock and copies records in under its WAL insertion locks, for the same reason.)
The session then waits outside every lock, and after it has dropped its views, its POV pointer and
its session pin: in the WebSocket path that is the statement runner (`TStmtQueue::Launch`), between
`work = nullptr` and the reply. The prototype first parked inside `TSession::Try`, still holding its
`TPov` pointer and session pin, and on a 4-runner topology (the CI runners' core count, or
`FAKE_NPROC=4` in the VM) the server wedged: the log was idle, the writers never got their replies,
and a durable manager runner spun at 100% CPU. Moving the wait to the statement runner made it
rarer, not impossible: in the VM with 4 runners, one of three runs still stalled at 64 writers
(after 1 and 8 writers on the same server). On the 4-vCPU CI runners, six servers with the log
(three per depth) went through 1, 8 and 64 writers each without a stall. I didn't find the cause. It
may be the #804 class (a fiber that parks while holding the durable manager's `std::mutex`, so the
next fiber on its runner blocks the thread; fixed by PR #806, not merged yet): parking more fibers
for longer makes that window likelier, and each round of writers creates new sessions and POVs.
Stage 2 must recheck on top of #806, on the 4-runner topology, before the wait ships. A transaction
whose mutations touch no safe repo isn't logged and gets no LSN.

### 5.6 Checkpoints, truncation and recycling

- **Retirement.** A record is retired when everything it did is in a data file or superseded:
  - a push into the global POV: when the global POV's flush covers its sequence number;
  - a push into a safe child: when the child's update has been popped (promoted) **and** that
    promotion's push into the parent is retired, recursively up to the global POV;
  - a pop or status change: when its repo's later state is in a checkpoint;
  - a `POV` record: when the POV is in a checkpoint.
  The log's **head** is the oldest unretired record. Space before the head can be reused.
- **Checkpoints** are written to one of two fixed slots, alternating, each with its own checksum
  and a checkpoint number (the newest valid one wins: the file service's base images and LMDB's
  meta pages do the same). A checkpoint holds: the head LSN; the global POV's flushed sequence
  number; every live safe POV (id, parent, policy, audience, TTL expiry, status, next and lowest
  sequence numbers, the LSN of its oldest unpromoted record); the exactly-once table (bounded,
  [§8.2](#82-exactly-once-733)); and #745's per-repo metadata ([§9.2](#92-bundled-with-745)). It is
  written after flushes advance the head and at least every `--wal_checkpoint_mb` of log (default
  64 MiB), synced, and only then is the head allowed to move past what it replaces.
- **Copy-forward.** A paused safe POV (or one Tetris can't promote) pins its oldest record, and so
  the head, while the rest of the ring fills. When the head's records are older than half the ring
  and belong to a still-unpromoted safe POV, the leader rewrites that POV's unpromoted backlog as
  fresh `TXN` records at the tail (flagged as copies, carrying the updates with their original
  sequence numbers, so replay applies them with `ensure_or_discard` like any other), then the next
  checkpoint moves the head past the originals. The cost per POV is bounded by the #721 backlog cap.
  Without it, one paused POV could stop the whole log.
- **Truncation is logical.** Nothing is erased; the head moves in a checkpoint, and the lap number
  rejects old groups when the ring wraps. SQLite's WAL resets with new salt values for the same
  purpose and bounds its size by checkpointing every `wal_autocheckpoint` pages while readers carry
  on; here the "checkpoint" that matters for size is the global POV's ordinary flush.

### 5.7 I/O errors

The rule (R6, fsyncgate): after a write or sync error on the log, nothing written since the last
good sync can be trusted to be on the device, and the next sync may report success anyway.
PostgreSQL has panicked on a failed fsync since 2019 (`data_sync_retry = off`) and recovers from
its WAL.

- The log enters **failed**: it never retries the sync or rewrites the group, and never reuses its
  ring space.
- Every fiber waiting on an LSN that isn't durable gets a typed **`durability_unknown`** reply:
  "the write may or may not survive a restart." Never "not written": the write did commit in memory,
  and the device may well have it. A client that wants certainty retries with its operation id after
  the restart ([§8.2](#82-exactly-once-733)).
- New writes to safe POVs at `local` or `logged` are refused with `durability_unknown` too (reads
  and fast POVs carry on), and the reporting port shows the failure. A restart runs recovery, which
  decides from what is on the device.
- The same applies to a write or sync that *reports* an error after its effect reached the media:
  recovery will find the record, valid and sealed or not, and act on that.

## 6. Recovery

On start, after the file service has loaded the file map and before any session is accepted:

1. **Read the system block.** Store format version; if it is newer than this binary knows, refuse
   to start. If the store has no log (format 0), start as today ([§9](#9-format-and-compatibility)).
2. **Load the global POV** from its data files as today. `G_disk` = the highest global sequence
   number on disk.
3. **Pick the checkpoint**: the valid slot with the higher checkpoint number. If neither is valid
   and the log isn't empty, refuse to start (a store with a log and no checkpoint is corrupt).
4. **Scan from the checkpoint's head**, group by group. A group is valid when its magic, store id,
   header checksum, payload checksum and record CRCs match, its lap and group number are the ones
   expected at that position, and its chain matches the previous group's checksum. Stop at the
   first invalid group k.
5. **Classify the stop.**
   - Read ahead over the rest of the ring's written part. If any valid group further on (same lap
     and chain from any valid predecessor, or a "sealed through" that covers k's position) seals an
     LSN at or after k's first LSN, **k was acknowledged and is damaged: refuse to start**, naming
     the LSN range, unless the operator passes `--wal_accept_loss_after=<lsn>`. The same if k's LSNs
     are at or below the checkpoint's recorded durable LSN.
   - Otherwise **k is a torn or never-synced tail**: the log ends at k-1. Nothing at or after k was
     acknowledged as durable.
6. **Restore safe POVs** from the checkpoint and the `POV` records after it: create each repo with
   its parent, policy and TTL, at its recorded sequence numbers.
7. **Replay** every `TXN` record from the head through k-1, in LSN order, through the slave's apply
   path (`ApplyCoreVectorTransactions`): pushes use `ensure_or_discard` with their logged sequence
   numbers, so a global push at or below `G_disk` is discarded and everything else lands at exactly
   its original number; pops and status changes are applied; mutations on repos that don't exist
   (fast POVs) are skipped. Exactly-once entries are rebuilt from the records' metadata. A child
   push whose update id is already in the global POV is an integrity event (the write-ahead rule
   failed): log it, skip it, count it on the reporting port.
8. **Write a checkpoint** (so the next crash replays less) and open for traffic. Tetris resumes
   promoting the restored backlogs.

Recovery never repairs from the middle of the log and never truncates anything sealed.

**Idempotence.** Recovery itself can crash at any step. Steps 1–7 write nothing; step 8 writes the
inactive checkpoint slot. Replaying the same log again gives the same state.

**Divergence after failover.** A master that lost its role and comes back must not replay its log
as authoritative: the new master may lack its last records. It rejoins as a slave (today's file copy)
and discards its log. Its system block records the replication epoch it last served, and recovery
refuses to replay a log from an older epoch while a newer master exists ([§8.4](#84-replication)).

## 7. API

### 7.1 Levels

| Level | Logged | `ok` means | Loss bound on a crash | Default for |
|---|---|---|---|---|
| `memory` | no | committed in memory (today) | today's: up to `--update_pool_size` transactions, no time bound | fast POVs |
| `logged` | yes | committed and in the log's next group, not waited for | the last one or two groups: a few ms of writes | — (per call or per POV) |
| `local` | yes | in a synced, sealed group on this server's disk | none, short of the device losing synced data (detected, [§6](#6-recovery)) | **safe POVs** |
| `replica` | yes | `local` plus received by the slave | survives failover too | — (later, [§8.4](#84-replication)) |

`logged` is PostgreSQL's `synchronous_commit = off`: memory-speed replies, durable within
milliseconds, and the whole log order and POV identity of `local`. It is the cheap middle that
"fast vs safe" lacks today.

On a `--mem_sim` store, or with `--durable_acks=false`, every level is `memory` and receipts say so.

### 7.2 Choosing a level

- **Per POV, at creation:** `new pov ... durability <level>`; default `local` for safe POVs,
  `memory` for fast. A fast POV can't be `logged` or `local` (it has no durable identity), and
  neither can a safe POV with a fast ancestor: both refuse with `durability_unavailable`.
- **Per session:** `durability <level>;` sets the default for this session's calls.
- **Per call:** `try {pov} wait <level> <pkg> <method> <args>;` (and the same for batches). A call
  may lower its POV's level; raising it above what the POV supports is `durability_unavailable`.
  #750's "wait for durable" is `wait local` on a safe POV, and on a fast POV it is option C's
  `wait flushed` (promotion plus a data file), which the persistence notifications already support.
- **Server:** `--durable_acks=true|false` (default true on a real device). `false` keeps today's
  behaviour for every POV and writes no new records; a store with unreplayed log records is still
  replayed at start.

The clients (TS, Python, Go, the MCP server) take `durability` on `newPov` and `wait` on `call`.
The wire bytes don't change when neither is used.

### 7.3 Receipts (#750)

Every successful write's reply gains:

```json
{"status": "ok", "result": ..., "version": V, "durable_through": D, "durability": "local"}
```

- `version` is #745's version: the global sequence number for a write made in the global POV; for
  any other POV, its version vector, whose first component is the POV's own sequence number
  (`versioned-reads.md` §4.2). With durable POVs, a safe POV's own sequence numbers survive a
  restart, so V stays valid across one.
- `durable_through` is in the same space as the POV's own component of V: the highest sequence
  number of this POV through which its writes are durable. For `local`, D ≥ V's own component when
  the reply is sent. For `logged`, D may be lower. For `memory`, D is the POV's durable point
  through promotion and flush (often far behind), or absent for a fast POV that has none.
- `durable version of {pov};` returns the POV's current D without writing (#750's third item).

The log's LSN is internal and never appears on the wire: clients compare versions of one POV,
nothing else.

## 8. Interactions

### 8.1 Versioned reads (#745)

The #745 design makes a version a global sequence number, and a child POV's version a vector valid
until its writes are promoted. The log records sequence numbers as assigned, and replay reassigns
the same ones, so: a global version survives a restart (it already did); a durable child POV's
vector survives a restart too, which #745 had ruled out because POVs were ephemeral. The vector
still expires at promotion, as #745 says. The log's checkpoint is the home #745 needed for its
per-repo metadata ([§9.2](#92-bundled-with-745)).

### 8.2 Exactly-once (#733)

- The operation id and the saved outcome (status and result, size-capped) go into the write's
  `TMetaRecord`, which is part of the update, so they are in the same log record as the write, and
  they travel with the update through promotion into the data file's update index.
- The exactly-once table maps (POV, operation id) → (sequence number, outcome). It is rebuilt at
  recovery from the checkpoint plus the replayed records, so a retry after a lost reply finds the
  outcome exactly when the write is durable, and never otherwise.
- A retry that arrives while the original waits for its group waits with it. A retry after
  `durability_unknown` and a restart finds the outcome if recovery kept the write, and runs fresh if
  it didn't: never twice.
- Retention is bounded (by count and age, per #733), kept in the checkpoint, so the table doesn't
  pin the log.

### 8.3 Admission (#590, #607, #721)

- **Disk (#590).** The log range is reserved at creation; disk admission's free space excludes it.
  The log never allocates at run time.
- **Log space (new).** Before commit, a safe write reserves its record's encoded size in the ring,
  next to the existing memory reservation; if the ring can't take it, the write is refused with
  `insufficient_storage` (retryable, nothing written), and admitted again once flushes retire
  records. Because an unretired record is a write that isn't in a data file yet, and every such
  write holds Update pool memory, sizing the ring to the pools' bytes means memory admission
  refuses first in practice; the log check is the backstop.
- **Memory (#607).** Unchanged. A write waiting for its group holds no pool reservation beyond its
  committed update, which it holds today anyway.
- **Backlog cap (#721).** Unchanged; it now also bounds how much log a POV's backlog can pin, and so
  the copy-forward cost ([§5.6](#56-checkpoints-truncation-and-recycling)).
- **Backpressure.** `ApplyWriteBackpressure` runs after the commit as today; the durability wait
  comes after it, so a writer waits for both, not one inside the other.

### 8.4 Replication

- Today the master ships each transaction's replica every `--replication_interval` (100 ms). The
  log records are the same encoding, so the slave can append what it applies to its own log and
  make its copy durable on its own disk with no new format.
- **Contract in v1 (`local`):** an acknowledged safe write survives a crash and restart of the
  master. It does not necessarily survive a **failover**: the slave may not have received the last
  replication interval's writes when it takes over. With replication on, that loss bound is the
  replication lag (at least one interval, 100 ms by default), and `docs/durability.md` must say so.
- **`replica` (later):** acknowledge after the slave has received the transaction (PostgreSQL's
  `remote_write`) or logged it (`on` with a synchronous standby). That costs a network round trip
  plus the slave's group, and it requires shipping the stream per group instead of per interval.
- After a failover the old master's log may hold records the new master never got; [§6](#6-recovery)
  says what happens to them.

### 8.5 Shutdown (#744, #758, #769)

A graceful stop refuses new writes (#769), waits for Tetris only as far as fast POVs need, flushes,
writes a checkpoint, and exits. Safe POVs' backlogs stay in the log and come back on start. A stop
under heavy load is then bounded by the flush, not by Tetris's promotion rate.

### 8.6 Restart time

- Today: **210–220 ms** from start to the WebSocket port after a SIGKILL, for a near-empty store and
  for one with 16–19 data files after 36 s of load (release `orlyi`, loop volume, the VM); **1.15 s**
  on a CI x86 (v7) runner with 11 data files, and **1.42 s** on CI x86 (v5) with 19–32 (three
  runs).
- Added: scanning the log from the checkpoint's head (bounded by the ring size: 256 MiB at
  sequential read speed is a fraction of a second on any SSD) and replaying it. The unretired part
  of the log is the writes that aren't in a data file yet, which is bounded by the Update pool, the
  same bound as today's loss; copy-forward keeps a paused POV from stretching it.
- **Not measured yet:** the replay rate of the slave apply path for small transactions. If it were
  as slow as 50,000 transactions/s, a full default pool (sized from RAM; 100,000 in the kill
  campaign) would replay in 2 s. Stage 1 measures it and sets `--wal_mb`'s default so a full log
  replays in under 5 s on the CI runner.
- A safe POV's restored backlog is readable immediately; promoting it is Tetris's ordinary work
  after the server is up.

### 8.7 The fault model

The design must survive, and the tests must inject:

| Fault | Where it comes from | Detected by | Outcome |
|---|---|---|---|
| Process crash | SIGKILL, abort | (page cache intact) | replay; zero acknowledged safe writes lost |
| Power loss | unsynced writes gone, any subset of them persisted, out of order | chain, lap, checksums | torn/unsynced tail truncated; nothing acknowledged lost |
| Torn write | a group partly on the device | payload checksum | as power loss |
| Misdirected write | a group lands at the wrong offset | store id, lap, group number, chain at both places | sealed damage refused; unsealed truncated |
| Lost write | device acknowledges write and flush, data never lands | chain break at the next group | if sealed later: refused |
| Latent sector error | a read fails at recovery | read error = invalid group | if sealed: refused; else truncated |
| Bit rot in a synced group | | checksum | if sealed: refused |
| Write or sync error | EIO from `pwrite`/`fdatasync` | the call's return | log failed, `durability_unknown`, refuse until restart ([§5.7](#57-io-errors)) |
| Error after effect | the write reached the media, then EIO | as above | recovery finds the record and keeps or truncates it by the seal rule |
| Lost reply | connection drops after commit | client | retry with operation id (#733) |

TigerBeetle states the same storage fault model (torn, misdirected and lost writes, latent sector
errors) and tests it in its deterministic simulator (the VOPR); its recovery distinguishes "never
written" from "written then damaged" the same way, using its redundant header ring and, in a
cluster, repair from replicas ("Protocol-Aware Recovery for Consensus-Based Storage", FAST '18). A
single Orly server can only detect and refuse; a slave is the repair.

**Create, rename, truncate.** ALICE ("All File Systems Are Not Created Equal", OSDI '14) found most
crash-consistency bugs in missing directory syncs after create or rename and in assumed write
ordering. Orly has no file system on the data path, so its list is short:

| Operation | Required order |
|---|---|
| Reserve the log range (create or upgrade) | write zeros over the range, sync; write the system block's format version, log range and empty checkpoints, sync; only then the first group |
| Append a group | write, sync; the seal is in a later group's header |
| Checkpoint | write the inactive slot, sync; the head moves only after that sync |
| Truncate | none (logical, in the checkpoint) |
| Wrap the ring | none (lap number in each header) |
| Flush a data file covering logged writes | only up to the log's durable LSN (the write-ahead rule); data blocks synced, then the file map's sector synced, as today |

A loop volume's backing file has file-system metadata of its own; a fully written, preallocated
image keeps that out of the sync path ([§5.1](#51-placement)).

### 8.8 Proof: fault harness and kill campaign

**Fault-injection harness (`orly/indy/fault_injection.test.cc`).** A new `Wal` case runs a
sequence of group commits with a checkpoint and a wrap, and recovers, under each existing mode
(Write, Read, Sync, Power, PowerTorn) at every N, plus new fault-device modes: **Misdirect** (the
Nth write lands at another offset), **LostWrite** (the Nth write reports success and is dropped),
**WriteErrAfter** / **SyncErrAfter** (the Nth write or sync reaches the media, then fails), and
**BitFlip** (one bit of a synced group flipped before recovery). Every edge the issue lists gets its
own N: each group write, each sync, the seal (the next group's write and sync), each checkpoint
write and sync, and the acknowledgment (the harness records which LSNs were acknowledged and checks
them after recovery). A run passes when:
- every acknowledged LSN is replayed, exactly once, and nothing unacknowledged appears out of prefix
  order;
- recovery refuses (never truncates) when an acknowledged group is damaged, and the refusal names
  it;
- after an injected write or sync error, nothing is acknowledged, no waiter is told "not written",
  and the log refuses until restart.

**Kill campaign, `durable` mode (`tests/kill_campaign.py --mode=durable`).**
- Writers keep their POVs across restarts (no fresh POVs per epoch); safe POVs must accept calls
  after each restart with their unpromoted backlogs intact (the `ephemeral` check becomes
  `pov_survives` for safe POVs and stays as it is for fast ones).
- **`durable` check:** for every safe writer at `local`, every acknowledged transaction is back
  after every kill. Zero, not a bound. Fast writers keep today's `bound` check; `logged` writers
  get a bound of the transactions acknowledged in the last two groups' time before the kill.
- In-flight writes are retried with their operation id after the restart; the `counter` check
  catches a double apply.
- **A power-loss layer.** SIGKILL leaves the kernel's page cache alone, and a loop volume's writes
  sit in the backing file's page cache until synced, so a server that skipped its syncs would pass a
  SIGKILL-only campaign. The durable mode therefore puts the volume on a `dm-log-writes` target
  (the device-mapper target xfstests uses for crash-consistency tests), which records every write
  and flush; after each kill it rebuilds the volume as of the last flush before the kill (and, with
  a seed, a random subset of the writes after it), and restarts on that. CrashMonkey (OSDI '18)
  builds crash states the same way. Whether `dm-log-writes` is available on the GitHub runners'
  kernel is to be checked in stage 0; the fallback is the same replay done by a small user-space
  block log in front of the loop device.
- **Negative controls, both required to fail:** `NEGATIVE=nosync` (a test-only flag that skips the
  log's sync but still acknowledges) and `NEGATIVE=early_ack` (acknowledge before the group's sync),
  each under the power-loss layer. The existing `NEGATIVE=rollback` stays.
- CI runs a short durable campaign (3 kills, both negative controls) on every push, and the full
  one on dispatch, like today's campaign.

**Simulation.** FoundationDB tests its whole commit path in deterministic simulation, which finds
interleavings no kill campaign reaches. Doing that for the whole server would mean making the
layers underneath deterministic (real OS threads per runner, libaio, epoll, real clocks). The log,
though, is a small single-purpose component with a narrow interface (append, wait, scan); a seeded
simulator of the log plus recovery, with the fault device's faults, is in reach and is listed as
optional in stage 1.

## 9. Format and compatibility

### 9.1 The change

- **A store format version** in the system block (the slots after `MinDiscardBlocksPos` in
  `TDeviceUtil`). 0.2.x stores read as version 0. This binary refuses any version it doesn't know.
- **The log range and the two checkpoint slots**, recorded next to it.
- **Records** as in [§5.2](#52-format).
- **#733's operation id and outcome** as optional `TMetaRecord` fields (absent when not used).

### 9.2 Bundled with #745

Stage 3 of #745 needs a store format version for persisted retention metadata, save points and
(time, version) samples, and a home for them that survives restart and reaches slaves. This design
supplies both: the format version is the same slot, and the metadata goes in the checkpoint, with
each change logged as a `META` record (and so replicated). One bump, one approval, one migration.
The #745 design's §11 already asked for exactly this bundling.

### 9.3 Opening old and new stores

| Binary | Store | Result |
|---|---|---|
| 0.3 | 0.2.x (version 0) | with `--durable_acks=true`: upgrade in place: reserve a contiguous log range from free space, zero it, write version 1. If no contiguous range is free, refuse and say so (start with `--durable_acks=false`, which opens version 0 stores as today) |
| 0.3 | version 1 | normal start with recovery |
| 0.2.x patch (0.2.N+1) | version 1 | **refuses** to start: "this store needs Orly 0.3 or later" |
| 0.2.0 – 0.2.N | version 1 | unknown until checked: these binaries don't read the slot. If they ignore it, they open the store and ignore the log: acknowledged safe writes that are still only in the log are lost, silently. |

The last row is why the release order matters: ship a 0.2.x patch that refuses a nonzero format
version first, and consider also changing the device magic number for version 1 stores so that even
older binaries don't recognise the device. That depends on what 0.2.x does with an unrecognised
device (skip it, or reformat it under `--create=true`?), which #745 also flagged and which stage 0
must check before choosing.

**Downgrade.** `orly_dm --downgrade-format` rewrites version 0 only when the log holds nothing
unretired (after a stop that drained every POV, `--stop_drain_all`), dropping the log range. Durable
POVs don't survive a downgrade (0.2.x treats every POV as ephemeral).

## 10. Measurements

All numbers are from a scratch branch and a scratch tool, never pushed. Each row names its machine.

**Machines.**
- **VM:** a shared Linux container under OrbStack (Linux 7.0.14) on a MacBook Pro, Apple M3
  Max, 64 GB, Apple SSD AP2048Z; 16 vCPUs and 15 GB in the VM. Volumes and the log are loop devices
  on files in the VM's overlay file system (virtio disk). **Busy:** other builds and test runs were
  using it the whole time (load average 8–16), so its numbers are noisy; each figure is
  a range over 2–3 repetitions.
- **CI x86:** GitHub `ubuntu-24.04` runners, 4 vCPUs, 16 GB, kernel 6.17 (azure), one runner per
  run with nothing else on it. They are not all the same machine, so each CI row says which:
  - **CI x86 (v7):** Azure `Standard_D4ads_v7`, AMD EPYC 9V45; the root file system (and the loop
    devices' backing files) on a 150 GB "MSFT NVMe Accelerator v1.0". Sync latency and the log
    alone (§10.1, §10.2), and one run of today's `orlyi`.
  - **CI x86 (v5):** Azure `Standard_D4ads_v5`, AMD EPYC 9V74; the root file system on a 150 GB
    "Virtual Disk" (`sda`). Today's `orlyi` against the prototype (§10.3). I didn't measure its
    sync latency.
- **CI arm64:** a GitHub `ubuntu-24.04-arm` runner; its run was still queued when this was
  written (§10.5).
- **Host SSD:** the same M3 Max, macOS 27, native.

### 10.1 Sync latency

One 4 KiB `O_DIRECT` write then the sync, 2,000 times, at consecutive offsets of a 512 MiB range.
`flsbuf` = `fsync` + `BLKFLSBUF`, as `TPersistentDevice::Sync()`. Microseconds, sync only:

| Machine | Target | Sync | p50 | p99 |
|---|---|---|---:|---:|
| VM | loop device | fsync + BLKFLSBUF | 793–956 | 3,316–7,829 |
| VM | loop device | fsync | 438–865 | 2,085–4,091 |
| VM | loop device | fdatasync | 863 | 4,003 |
| VM | preallocated file | fdatasync | 407–2,833 | 1,873–11,420 |
| VM | preallocated file | fsync | 522 | 3,529 |
| VM | appended file | fdatasync | 683 | 4,069 |
| VM, **busy disk** | loop device | fsync + BLKFLSBUF | 19,814 | 126,339 |
| CI x86 (v7) | loop device | fsync + BLKFLSBUF | 235–265 | 309–413 |
| CI x86 (v7) | loop device | fsync | 103–105 | 242–249 |
| CI x86 (v7) | loop device | fdatasync | 105–106 | 240–257 |
| CI x86 (v7) | preallocated file | fdatasync | 0 (no flush sent) | 0 |
| CI x86 (v7) | preallocated file | fsync | 0 | 118–120 |
| CI x86 (v7) | appended file | fdatasync | 111–113 | 143–173 |
| CI x86 (v7), **busy disk** | loop device | fsync + BLKFLSBUF | 434–437 (p90 9,357–9,378) | 9,555–9,618 |
| Host SSD (macOS) | file | `fsync` (no device flush on macOS) | 28 | 308 |
| Host SSD (macOS) | file | `F_BARRIERFSYNC` | 209 | 1,226 |
| Host SSD (macOS) | file | `F_FULLFSYNC` (device cache flush) | 5,071 | 7,239 |

"Busy disk": a second thread writing 1 MiB `O_DIRECT` blocks to another file on the same file
system with a sync every 64 MiB (2.5 GB/s in the VM), standing in for a disk merge.

- The VM's sync is much faster than the host SSD's real cache flush, so the VM doesn't flush to
  stable media on every sync. On the runner, `fdatasync` of a preallocated file returned in under a
  microsecond: the kernel sent no flush, which is what happens when a device reports no volatile
  write cache (Azure's disks acknowledge writes durably; I didn't check the runner's
  `queue/write_cache`). Its loop-device sync (0.1–0.26 ms) is the loop driver writing the backing
  file's page cache. So both machines bound the design's overhead from below. A data-center NVMe
  drive with power-loss protection can complete a flush in tens of microseconds; a consumer drive
  without it takes milliseconds, like the host SSD above. That is decision 8.
- With a concurrent bulk writer the sync is 20× slower at p50 and 30× at p99 in the VM; on the
  runner the p50 barely moves but p90 and p99 jump to 9.4–9.6 ms. That is the device and its
  queue, not the log's design; it is the argument for a separate log device later and for
  rate-limiting merges against the log.
- `BLKFLSBUF` costs 0.1–0.4 ms at p50 on top of `fsync` in the VM and 0.13–0.16 ms on the runner;
  the log should not call it.

### 10.2 The log alone: group commit prototype

`gcbench group`: W writer threads, each appending a 256-byte record (plus a 16-byte record header)
and waiting until it is durable, in a closed loop for 6–8 s; a leader thread writes each group with
`O_DIRECT` (4 KiB aligned, with a 64-byte header carrying a CRC32C and a chain hash); one or two
syncer threads. Loop device, fsync + BLKFLSBUF. Commits/s and commit latency (µs), range over 2–4
repetitions:

| Machine | Writers | Config | commits/s | p50 | p99 |
|---|---:|---|---:|---:|---:|
| VM | 1 | one in flight, no seal | 901–1,037 | 865–932 | 3,927–4,126 |
| VM | 1 | two in flight, no seal | 1,010–1,115 | 738–872 | 2,658–3,790 |
| VM | 1 | seal in a second sync | 526–629 | 1,182–1,784 | 5,395–5,606 |
| VM | 1 | seal on the next group | 527–553 | 1,663–1,836 | 4,562–4,920 |
| VM | 8 | one in flight, no seal | 1,071–5,210 | 1,120–5,276 | 4,845–42,542 |
| VM | 8 | two in flight, no seal | 666–4,529 | 1,337–8,567 | 4,814–99,177 |
| VM | 8 | seal in a second sync | 1,791–2,832 | 2,252–2,987 | 8,818–24,670 |
| VM | 8 | seal on the next group | 2,494–2,522 | 2,854–2,999 | 6,849–8,449 |
| VM | 64 | one in flight, no seal | 27,987–35,573 | 1,549–1,915 | 4,979–11,261 |
| VM | 64 | two in flight, no seal | 5,986–33,827 | 1,693–8,754 | 5,472–42,301 |
| VM | 64 | seal in a second sync | 11,430–19,466 | 3,295–3,774 | 7,458–31,065 |
| VM | 64 | seal on the next group | 5,669–17,479 | 3,526–5,492 | 7,135–51,602 |
| VM | 1 / 8 / 64 | no sync at all (ceiling) | 20,564 / 97,582 / 180,039 | 40 / 70 / 300 | 193 / 351 / 1,700 |
| VM, busy disk | 8 | one / two in flight | 376 / 303 | 21,129 / 24,248 | 52,202 / 60,821 |
| VM, busy disk | 64 | one / two in flight | 2,094 / 1,729 | 28,528 / 35,188 | 61,093 / 75,751 |
| CI x86 (v7) | 1 | one in flight, no seal | 3,211–5,673 | 149–313 | 294–452 |
| CI x86 (v7) | 1 | two in flight, no seal | 4,646–5,948 | 147–161 | 288–387 |
| CI x86 (v7) | 1 | seal in a second sync | 2,490–3,162 | 282–287 | 438–682 |
| CI x86 (v7) | 1 | seal on the next group | 2,594–2,966 | 303–310 | 454–674 |
| CI x86 (v7) | 8 | one in flight, no seal | 16,067–19,766 | 286–297 | 489–657 |
| CI x86 (v7) | 8 | two in flight, no seal | 30,774–33,236 | 213–233 | 427–487 |
| CI x86 (v7) | 8 | seal in a second sync | 15,876–16,450 | 447–485 | 773–818 |
| CI x86 (v7) | 8 | seal on the next group | 17,659–18,714 | 419–442 | 625–677 |
| CI x86 (v7) | 64 | one in flight, no seal | 66,352–78,648 | 576–624 | 773–908 |
| CI x86 (v7) | 64 | two in flight, no seal | 115,582–119,130 | 533–551 | 873–893 |
| CI x86 (v7) | 64 | seal in a second sync | 72,681–74,480 | 878–920 | 1,117–1,176 |
| CI x86 (v7) | 64 | seal on the next group | 69,704–75,234 | 842–880 | 1,234–1,774 |
| CI x86 (v7) | 1 / 8 / 64 | no sync at all (ceiling) | 22,262–23,308 / 117,004–120,492 / 227,630–240,961 | 42–44 / 65–67 / 241–271 | 54–57 / 114–123 / 514–600 |
| CI x86 (v7), busy disk | 8 | one / two in flight / seal on next | 909–942 / 1,324–1,390 / 702–716 | 8,862–9,384 / 9,343–9,399 / 10,070–10,076 | 10,191–10,212 / 11,099–19,108 / 19,851–19,985 |
| CI x86 (v7), busy disk | 64 | one / two in flight / seal on next | 6,597–7,466 / 8,847–8,911 / 5,639–5,721 | 9,810–9,940 / 9,540–9,588 / 10,069–10,088 | 19,272–19,841 / 19,277–19,308 / 19,870–19,903 |

- Group commit works as intended. On the runner, 64 writers sustained 115,000–119,000 commits/s
  with two groups in flight on syncs of about 0.25 ms; in the VM, 28,000–35,000 on syncs of about
  1 ms. Commit latency is about one sync plus the wait for the group in front.
- Two groups in flight beat one by 1.6–1.9× on the runner at 8 and 64 writers. The VM disagreed, but
  the same configuration varied up to 5× between repetitions there; the runner's repetitions agree
  within 10–20%.
- The seal costs one sync of latency at every load (p50 0.15 → 0.30 ms at one writer on the
  runner); with the same number of closed-loop writers that is 37–47% fewer commits/s, still
  70,000/s at 64 writers.
- Busy disk: commit latency follows the device (p50 ~9.5 ms, p99 ~19 ms on the runner) whatever the
  configuration.

### 10.3 End to end: today vs the prototype in `orlyi`

The prototype (scratch branch off master `3fc60840`, never pushed): after a write's transaction
commits, `TSession::Try` and the batch path append a record of `128 + 48 × entries` bytes (a size
stand-in, not the real encoding) to a group-commit log on a second loop device, and the statement's
fiber parks on a `TSingleSem` until its group is synced (in the VM runs below, inside `Try`; in the
CI runs, after the statement has dropped its pointers, [§5.5](#55-where-the-append-happens)); one
leader and `ORLY755_DEPTH` syncer OS threads, as in [§5.4](#54-group-commit), no seal. Release
`orlyi` on a loop volume with the kill campaign's flags (`--do_fsync`, 100,000-update pool), the
kill campaign's `put` (a key, a writer's `+=` counter and a shared `+=` total in one transaction),
each writer on its own connection, session and **safe private POV**, 2 s warm-up then 10 s measured;
a fresh server per configuration, running 1, 8 and 64 writers in turn. Writes/s and milliseconds:

| Machine | Writers | today | log, 1 in flight | log, 2 in flight | log, no sync |
|---|---:|---|---|---|---|
| VM | 1 | 1,547–1,839 /s; p50 0.40–0.51, p99 3.2–3.5 | 695–763; p50 1.01–1.05, p99 5.0–5.6 | 787–915; p50 0.76–0.97, p99 4.8–5.4 | 1,131–1,666; p50 0.20–0.51 |
| VM | 8 | 635–1,011; p50 7.5–9.1, p99 15.7–44.2 | 994–4,481; p50 1.2–6.7, p99 6.4–30.5 | 1,988–4,200; p50 1.5–2.7, p99 5.7–21.7 | 917–1,296; p50 5.5–8.1 |
| VM | 64 | 554–818; p50 1.1–1.4, p99 167–439; ~1,200 refused | 2,052–4,999; p99 27–302 | 3,223–4,634; p99 86–116 | 572–1,281; p99 122–418 |
| CI x86 (v5) | 1 | 2,890–2,930; p50 0.30–0.31, p99 1.0–1.4 | 987–1,136; p50 0.56–0.65, p99 3.5–3.8 | 1,060–1,303; p50 0.55–0.57, p99 3.5 | 2,220–2,273; p50 0.38 |
| CI x86 (v5) | 8 | 2,218–2,322; p50 2.8–3.0, p99 10.6–11.7 | 2,983–4,243; p50 1.5–2.2, p99 7.3–8.6 | 3,038–4,177; p50 1.6–2.2, p99 8.1–8.3 | 1,101–2,389; p50 2.8–6.2 |
| CI x86 (v5) | 64 | 1,656–1,953; p50 14.8–21.6, p99 78–94; 283–512 refused | 1,869–2,444; p99 65–92 | 1,886–3,196; p99 50–100 | 1,090–2,379; p99 76–167 |
| CI x86 (v7) | 1 / 8 / 64 | 2,959 / 2,486 / 2,401 (one run); p50 0.28 / 2.4 / 13.6 | — | — | — |

- **One writer:** the log adds 0.4–0.6 ms at p50 and 1.5–2.2 ms at p99, about one VM sync, which is
  the target ("within one or two syncs"). One writer's throughput halves, because each of its writes
  now includes a sync.
- **8 and 64 writers:** today's run is not limited by the commit. With a private POV each, the
  writers outrun Tetris, fill their backlogs, and wait in the backlog cap and memory admission
  (about 1,200 refusals and a p99 of 170–440 ms with 64 writers); one writer alone gets more done
  than eight. The log's wait slows each writer by a millisecond, which keeps the backlogs short, and
  throughput went **up** 2–9×. The "no sync" column (the same wait machinery, no sync) behaves like
  today, so the difference is the pacing, not the code path. I don't propose relying on this; it
  says the target "within 10% at 64 writers" is met here because the log isn't what limits
  throughput.
- **On the CI runners** the picture is the same, less extreme. One writer's commit gains 0.25 ms at
  p50 (0.30 → 0.55–0.57 ms) and 2.1–2.5 ms at p99, and its throughput falls 55–64%. Its p90 went
  from 0.37 ms to about 3 ms in most runs, so a sync on that runner's "Virtual Disk" is probably
  around 2.5–3 ms at the tail. At 8 writers the prototype did 3,000–4,200 writes/s against today's
  2,200–2,300, and at 64, 1,900–3,200 against 1,650–1,950: today's throughput is flat from 1 to 64
  writers (2,900 → 1,650–1,950) while p50 rises to 15–22 ms, so here too the commit isn't what
  limits it. The 4-vCPU runners never stalled with the wait moved to the statement runner (six
  servers with the log, each through 1, 8 and 64 writers).
- **One shared safe POV** (all writers in one POV; 2 repetitions): today 1,552–1,570/s at one writer,
  388–585/s at 8 (p99 162–200 ms) and 41–250/s at 64 (p50 95–895 ms); with the log, 2,669–3,598/s
  at 8 (p99 6–9 ms) and 154–3,485/s at 64. 64 writers in one POV are pathological with or without
  the log on this box; that is worth its own issue, separate from this design.

### 10.4 The sync's share (R8)

| Machine | Disk | today's commit p50 | safe commit p50 with the log | sync p50 | sync's share of the safe commit |
|---|---|---:|---:|---:|---:|
| VM | quiet | 0.40–0.51 ms | 0.76–1.05 ms | 0.44–0.96 ms | ~50–90% |
| VM | busy | (no sync) | ~20 ms (estimated: sync p50 + today's commit) | 19.8 ms | ~98% |
| CI x86 (v5) | quiet | 0.30–0.31 ms | 0.55–0.57 ms | not measured on this machine | ~45% (the added 0.25 ms) |
| CI x86 (v7) | quiet | 0.28 ms | (not run) | 0.24–0.27 ms (`flsbuf`), 0.10 ms (`fdatasync`) | |

On a quiet disk in the VM the sync is most of a safe commit; on the CI runner the added wait was
under half of it at p50, and most of it at p99. Either way group commit, not code, is what keeps
throughput up. On a busy disk the device's queue is everything: the design can't fix that, only
isolate the log from it.

### 10.5 Reproducing

The tool and scripts ran in the VM, and on CI through the `ab-bench` workflow's `command` input
(both arms built at master; only arm A ran the measurements). Run ids: `37734164894` (CI x86 v7:
sync latency and the log alone); `37741976751` (CI x86 v7: today's commit latency; its prototype
runs used the wait inside `Try` and wedged, [§5.5](#55-where-the-append-happens), and the run was
cancelled); `37750619523` (CI x86 v5: today against the prototype with the wait moved);
`37750623484` (CI arm64, everything; still queued behind other benchmarks when this was written).
`gcbench` (`lat` and `group`), the `orlyi` prototype patch, the commit benchmark client and the CI
driver will be attached to the stage 1 PR, where they belong next to the real implementation's
benchmark.

## 11. Staged plan

Each stage is its own PR with its own tests.

0. **Prerequisites (two-way).** #769 (PR #802), #804 (PR #806) and #796 merged. The fault device's
   new modes (Misdirect, LostWrite, WriteErrAfter, SyncErrAfter, BitFlip) with their own tests. The
   kill campaign's power-loss layer (`dm-log-writes` on the runners, or the user-space fallback) and
   its `nosync` / `early_ack` negative controls, proven against today's code: a power-loss campaign
   on today's master must lose acknowledged writes and say so. Check what 0.2.x does with an
   unrecognised device and with a nonzero format slot (decides magic number vs slot). The NVMe
   measurement (decision 8).

1. **The log as a library (two-way).** `TWal`: ring on a block range, leader and syncers on OS
   threads, groups, checksums, chain, piggybacked seal, scan/verify/classify, checkpoints,
   copy-forward. Fault-harness `Wal` case at every edge, a seeded log simulator (optional), and the
   standalone benchmark on CI and the NVMe machine. Measure replay rate. Not wired to the server; no
   store changes.
2. **Wired to the commit path, test stores only (two-way).** `--durable_acks=experimental` works
   only on stores it creates (refuses to upgrade an existing one): log records at commit, the
   durability wait, levels and receipts, durable safe POVs, recovery through the slave apply path,
   the write-ahead rule in the global flush, graceful stop without draining safe POVs. Gate: the
   kill campaign's `durable` mode with zero acknowledged safe writes lost over 40 kills under the
   power-loss layer, both negative controls failing, the fault harness green, ab-bench throughput
   and latency at 1/8/64 writers against master on x86 and arm64. (2a, if POV restore proves hard:
   ship option B's subset first.)

   **APPROVAL POINT.** Before stage 3 the maintainer approves, together with #745's stage 3: the
   store format version and how old binaries are kept out (§9), the log and checkpoint format
   (§5.2, §5.6), what the checkpoint carries for #745 and #733, the levels and their defaults (§7,
   decisions 2–6), and the replication contract (§8.4). Stage 3 is the first stage whose stores an
   older binary must refuse.

3. **The format change (one-way).** The 0.2.x patch release that refuses version 1 ships first.
   Then: format version, upgrade of version 0 stores, durable acks on by default for safe POVs,
   #745's metadata in checkpoints and `META` records, #733's operation fields in `TMetaRecord`.
   Tests: upgrade from a 0.2.x store with data; an older binary refusing the store; the durable
   campaign on an upgraded store; downgrade refused while the log holds anything.
4. **Exactly-once (#733)** on top: operation ids, the table, retries across a restart, clients.
5. **Replication levels** (`replica`), the slave's own log, the failover rule of §6.
6. **Docs.** `docs/durability.md` rewritten with the new contract and the measured loss bounds per
   level; `docs/PROTOCOL.md`; README; clients' docs.

## 12. Prior art

- **PostgreSQL WAL, `synchronous_commit`.** Durability per transaction (`off`, `local`,
  `remote_write`, `on`, `remote_apply`), not per server: the model for §7's levels and the per-call
  override. Group commit with `commit_delay` / `commit_siblings`. With `off`, the loss window is up
  to three times `wal_writer_delay`; `logged` is the same idea.
- **fsyncgate** (2018). After a failed fsync Linux may drop the dirty pages and report success on
  the next one, so a retry proves nothing. PostgreSQL now panics and recovers from the WAL
  (`data_sync_retry = off`). §5.7, and #621 already, follow it.
- **RocksDB.** WAL with leader-follower group commit; `enable_pipelined_write` separates the WAL
  write from the memtable insert so the next group forms meanwhile; recycled, preallocated WAL files
  (`recycle_log_file_num`) whose records carry the log number so a stale record from the file's
  previous use is rejected (§5.2's lap).
- **TigerBeetle.** Checksums on every header and body, a hash chain through the log, a redundant
  ring of headers next to the ring of prepares, an explicit storage fault model (torn, misdirected,
  lost writes, latent sector errors), direct I/O, and a deterministic simulator that injects those
  faults; recovery that tells "never written" from "written then damaged". §5.2–5.3 and §8.7 follow
  it on a single node.
- **FoundationDB.** Deterministic simulation of the whole commit path. §8.8 scopes it to the log.
- **ALICE** (Pillai et al., OSDI '14). Crash-consistency bugs come from missing directory syncs after
  create and rename and from assumed ordering. §8.7 lists every ordering the log needs.
- **CrashMonkey** (Mohan et al., OSDI '18) and xfstests' `dm-log-writes`: recording writes and flushes
  below the file system to build crash states. §8.8's power-loss layer.
- **SQLite WAL.** Checkpointing into the main store while readers continue, a cumulative checksum
  over frames, salts that change when the WAL restarts, and a log bounded by checkpoint frequency
  (`wal_autocheckpoint`). §5.6.
- **LMDB** (contrast). No log: copy-on-write pages and two alternating meta pages give an atomic
  commit with two syncs. Not a fit for layered, background-merged storage (§4.6); its alternating
  meta pages are the checkpoint slots.
- **ARIES** (Mohan et al., 1992). The write-ahead rule: a data page may reach disk only after the log
  records describing it. §4.1's rule for the global flush.

## 13. Open questions

- Should `logged` exist in v1, or only `memory` and `local`? It is cheap once the log exists, and it
  is the level most applications that now use fast POVs for speed would want.
- The checkpoint carries every live safe POV. With many thousands of POVs it is large; should it be
  incremental (only POVs changed since the last one), at the cost of a longer chain at recovery?
- Is two groups in flight worth keeping as the default? The VM says no; the NVMe run decides.
- Should the log's record encoding be the replication stream's exactly, or a versioned envelope
  around it, so the two can evolve separately? (I lean to an envelope with the stream's encoding
  inside.)
