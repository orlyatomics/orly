# Durability: what an acknowledgment means

This is what `orlyi` promises about a write once it has answered `"status": "ok"`, and what a
crash can take back. It is read from the code, and `tests/kill_campaign.sh` checks it by killing
`orlyi` under write load (see [How it is checked](#how-it-is-checked)).

## In short

- **Safe POVs (the default) provide durable acknowledgments** (#755). An `ok` from a safe POV means
  the transaction is committed to the write-ahead log (WAL) on disk and durably synced before the
  acknowledgment is sent. Zero acknowledged safe writes are lost across a crash, SIGKILL, or power
  loss. Safe POVs and their unpromoted backlogs survive restart.
- **Fast POVs provide memory-speed acknowledgments.** An `ok` from a fast POV means the transaction
  committed in memory to that POV. It writes nothing to disk before acknowledging. A crash loses only
  the newest unpromoted writes, with the total loss bounded by the Update pool size
  (`--update_pool_size`). Fast POVs remain ephemeral (#439).
- **Durability levels:**
  - `local` (default for safe POVs on disk): group-committed to the WAL and synced to stable media.
    Guarantees that every acknowledged write survives a crash and restart of the node.
  - `memory` (fast POVs, or `--durable_acks=false`): committed in memory; durability deferred until
    Tetris promotes to the global POV and global flushes to a data file.
- **Group commit bounds latency.** Safe writes are batched into pipelined groups with dedicated OS
  syncer threads, sustaining tens of thousands of commits per second at a sub-millisecond tail on
  NVMe storage (see [Measured](#measured)).
- **Store format version 1:** Stores created in Orly 0.3+ use store format version 1 with a
  preallocated WAL ring. Existing format version 0 stores are automatically upgraded in place when
  opened with durable acknowledgments. Format downgrade back to version 0 is supported via
  `orly_dm --downgrade-format` when the WAL holds no uncheckpointed records.
- **Write receipts** (#750): A `try` with `<{.receipt: true}>` returns `{"status": "ok", "version": V,
  "durability": "durable"}` on a safe POV, or `"durability": "memory"` on a fast POV.
  `.wait_durable_ms: N` on a fast POV waits until promotion and disk merge persist the write.

## The path of a write

### Safe POV (durable)

1. **Commit.** The method runs and commits its transaction to the POV's repo (`TRepo::AppendUpdate`).
   `~TTransaction` serializes the transaction and appends a `Txn` record to the WAL ring (`TWal`).
2. **Durability wait.** The statement fiber parks waiting on `TWal::WaitForDurable`, releasing its
   fiber runner for other work. The WAL's background leader batches concurrent commits into a group,
   writes it with `O_DIRECT`, and syncer OS threads execute `fdatasync`/`fsync`.
3. **Acknowledgment.** Once the group is durably synced and sealed, the waiting fibers wake up and
   return `"status": "ok"` with `"durability": "durable"`.
4. **Promotion and flush.** Tetris (`orly/server/repo_tetris_manager.cc`) asynchronously moves the
   POV's backlog to the global POV, and the global POV flushes memory layers to immutable data files.
   Checkpointing moves the WAL head forward, truncating retired log records as writes reach data files.
5. **Restart & Recovery.** On restart, `orlyi` reads the superblock and newest checkpoint, scans the
   WAL, validates checksums and hash chains, restores safe POVs, and replays uncheckpointed
   transactions through the core apply path.

### Fast POV (in-memory)

1. **Commit.** `TRepo::AppendUpdate` inserts the update into the repo's memory layer. Nothing is
   written to the WAL and nothing waits for disk.
2. **Acknowledgment.** The reply `"status": "ok"` is returned immediately with `"durability": "memory"`.
3. **Promotion & Flush.** Tetris promotes updates to parent POVs and eventually to the global POV,
   which flushes to disk during scheduled memory merges.
4. **Restart.** Fast POVs are ephemeral (#439): any unpromoted writes in their backlogs are lost,
   and the POV id is refused after restart.

## Safe and fast POVs

| Property | Safe POV (default) | Fast POV |
|---|---|---|
| Acknowledgment | After WAL fsync | Immediately on memory commit |
| Receipt durability | `"durable"` | `"memory"` |
| Crash loss (SIGKILL) | **Zero** acknowledged writes lost | Newest writes, bounded by `--update_pool_size` |
| Lifecycle | Durable (survives restart) | Ephemeral (#439, refused after restart) |
| Latency overhead | ~1 disk sync (sub-millisecond on NVMe) | 0 (pure memory speed) |
| Use case | Financial data, ledgers, critical records | High-rate transient metrics, caches, scratch spaces |

## What a crash can lose

A SIGKILL, process crash, or power loss:

- **On safe POVs:** Loses **zero** acknowledged writes. Every write acknowledged as `ok` is recovered
  from the WAL during startup. In-flight writes that had not received an acknowledgment may either be
  recovered (if their group synced before the crash) or truncated cleanly.
- **On fast POVs:** Loses unpromoted writes in the POV's memory backlog and unmerged memory layers of
  the global POV.
  - **Order:** Prefix order is strictly preserved: for any POV, what survives is a contiguous prefix
    from write 1 to R with no holes. Transactions and batches (`call_batch`, `call_many`) survive whole
    or not at all.
  - **Size bound:** Loss is bounded by `--update_pool_size` transactions plus writes in flight.
  - **Counter integrity:** Each `+=` is applied exactly once, and shared totals match the sum of surviving keys.

## Store format and compatibility

- **Format version 0:** Legacy format prior to #755. All POVs treated as ephemeral.
- **Format version 1:** Current format with preallocated WAL ring and checkpoint slots in the superblock.
- **Auto-upgrade:** Opening a format version 0 store with durable acknowledgments enabled
  (`--durable_acks=local` or `--durable_acks=true`) automatically upgrades the store in place: allocates
  a contiguous block range from free space, zeros the checkpoint slots, sets FormatVersion = 1, and
  writes the superblock. If insufficient contiguous space is available, it refuses to start and prompts
  running with `--durable_acks=false`.
- **Downgrade:** `orly_dm --downgrade-format --dev=<device>` downgrades a format version 1 volume to
  format version 0 only when the WAL contains no uncheckpointed records (following a clean stop where
  all POVs were drained).
- **Version enforcement:** Stores with format version greater than 1 are refused with an explicit error.

## A graceful stop

SIGTERM or SIGINT (`TServer::Shutdown`) cleanly stops the server:

1. **No new writes.** New writes are refused with `insufficient_memory` (#769).
2. **Flush WAL.** With durable acknowledgments enabled, all active WAL groups are flushed and synced,
   and a final checkpoint is recorded. Safe POV backlogs remain safely in the log and reload on restart.
3. **Flush data files.** The global POV's memory layers are flushed to disk.

## How it is checked

`tests/kill_campaign.sh` runs `orlyi` on a loopback volume with concurrent writers across diverse POV
configurations (safe, fast, shared, private, nested, batches, conditionals, and receipts). It repeatedly
SIGKILLs `orlyi` under active write load (40 kills by default), restarts on the same volume, and verifies
every write against a client-side ledger:

| check | what fails it |
|---|---|
| prefix | a writer's keys that came back have a hole |
| floor | a write that an earlier restart gave back is missing |
| phantom | a key came back that was never sent, or holds an older write's value |
| atomic | a batch came back in part |
| counter | a writer's `+=` counter differs from its key count, or the shared total from the sum |
| durable | in durable mode (`--mode=durable`), any safe writer lost an acknowledged write |
| pov_survives | in durable mode, a safe POV fails to survive restart |
| bound | in bound mode, fast writers lost more transactions than the Update pool held |
| progress | over the whole campaign, none of a writer's writes ever came back |
| open | the restart fails, or the open check finds invalid block accounting |
| ephemeral | a fast POV from before the kill still accepts calls (#439) |

**Negative controls:**
- `NEGATIVE=rollback`: Restores an older volume snapshot before restart; floor check must catch it.
- `NEGATIVE=nosync`: Runs with `--wal_no_sync` and drops unsynced writes to simulate power loss; durable check must fail.
- `NEGATIVE=early_ack`: Runs with `--wal_early_ack` and drops unsealed tail; durable check must fail.

## Measured

End-to-end measurements on release `orlyi` comparing memory-only commit to the WAL group commit
(see §10 of `docs/design/durable-acks.md`):

- **One writer:** WAL adds ~0.25–0.5 ms at p50 and ~1.5–2.5 ms at p99 (roughly one disk sync on NVMe).
- **64 writers:** Group commit pipelines concurrent writes, achieving 70,000–119,000 commits/s on NVMe
  with two groups in flight.
- **Restart recovery:** Scanning and replaying a full WAL log completes in under 2 seconds.
