# Durability: what an acknowledgment means

This is what `orlyi` promises about a write once it has answered `"status": "ok"`, and what a
crash can take back. It is read from the code, and `tests/kill_campaign.sh` checks it by killing
`orlyi` under write load (see [How it is checked](#how-it-is-checked)).

## In short

- **An acknowledgment is not a durability promise, on any kind of POV.** `ok` means the
  transaction committed, in memory, to the POV you sent it to. Safe and fast POVs, private and
  shared, single writes and batches: all the same.
- **A write is durable once it has reached a data file of the global POV.** Getting there takes
  two steps after the acknowledgment: Tetris promotes the write up the POV tree to the global
  POV, then the global POV's next memory merge writes it to disk.
- **A crash loses only the newest writes.** For each POV, what survives a crash is a prefix of
  the order its writes committed in: no holes, every transaction whole or not at all, every `+=`
  applied exactly once, and nothing that was never sent.
- **How much a crash can lose is bounded by memory, not time.** Every acknowledged write that
  isn't on disk yet holds an update in the Update pool, so a crash loses at most
  `--update_pool_size` transactions. There is no time bound: under sustained load the oldest
  write a kill took back was acknowledged up to a few seconds earlier (see
  [Measured](#measured)).
- **A client can see when a write becomes durable** (#750): a `try` with `<{.receipt: true}>`
  returns the version it committed at, and `durable_version {pov};` returns the highest version
  of that POV known to be on disk, once Tetris has promoted the write to the global POV and the
  global POV's flush has synced it. A write that a restarted server gives back is on disk, and
  stays there; the query itself restarts at `null` for a POV until it next promotes. `.wait_durable_ms: N` on the `try` waits for that, up to N ms.

## The path of a write

1. **Commit (the acknowledgment).** A `try` runs the method and commits its transaction to the
   POV's repo: `TRepo::AppendUpdate` (`orly/indy/repo.cc`) inserts the update into the repo's
   current memory layer. That is all that happens before the reply. Nothing waits for disk.
2. **Promotion.** Tetris (`orly/server/repo_tetris_manager.cc`) moves a POV's updates to its
   parent, oldest first, one transaction at a time; the push into the parent and the pop from the
   child are one transaction (see [architecture.md](architecture.md) §3). A POV made `from`
   another POV is promoted twice before its writes reach the global POV.
3. **Flush.** Only the global POV writes data to disk. Its memory merge (`TRepo::StepMergeMem`,
   at most every `--mem_interval` ms, 40 by default) writes its sealed memory layers to a new data
   file and puts the file in the file map. A child POV never writes its own data to disk, safe or
   fast: its data stays in memory until it is promoted (the `#227` comments in `StepMergeMem`).
4. **Restart.** Only the global POV's repo (and the system repo) is reloaded. POVs are ephemeral
   (#439): a POV's unpromoted writes are gone, and its id is refused after a restart.

So a write is durable exactly when step 3 has written it. Between the acknowledgment and that
point it lives only in memory: first in its POV's backlog, then in the global POV's memory
layers.

## Safe and fast POVs

A safe POV is backed by a `TSafeRepo` and a fast one by a `TFastRepo` (`TPov::GetRepo`,
`orly/server/pov.cc`). For durability that makes no difference: a `TSafeRepo` writes to disk
only when it is the global POV's repo, so a safe child POV keeps its writes in memory exactly as a
fast one does, and both lose their unpromoted writes in a crash or restart. Neither kind writes
anything to disk before acknowledging. The campaign runs both and finds the same behaviour.

## What a crash can lose

A SIGKILL (or any crash of the process) loses the acknowledged writes that step 3 hadn't written
yet:

- writes still in a POV's backlog, not yet promoted;
- writes promoted to the global POV but still in its memory layers.

**Order.** Within a POV, writes promote in commit order and the global POV writes its memory
layers to disk oldest first, so what survives is a prefix: if a POV's write survives, every write
committed to that POV before it survives too. A batch (`call_batch`, `call_many`) is one
transaction and survives whole or not at all, and a `+=` that survives is counted once.

**Size.** Each acknowledged transaction that isn't on disk holds at least one update in the
Update pool, so one crash loses at most `--update_pool_size` transactions (the pool size is in the
`memory plan sizes:` line `orlyi` logs at startup). The unpromoted part of it is also capped per
POV by the writer backlog cap (#721): at most `min(--tetris_backpressure_threshold,
update pool / 32)` updates and `update entry pool / 32` entries, except for a POV whose Tetris
join was deferred under memory pressure (#250).

**Time.** The code gives no time bound. The global POV's flush runs every 40 ms, but promotion
runs behind the writers whenever they write faster than Tetris promotes, and a write waits its
turn in that backlog.

### What a client can see

- Over WebSocket, nothing about promotion or durability: a WebSocket session queues no
  notifications (#591). The binary protocol sends `UpdateAccepted` each time a write is promoted
  one level, but nothing when it reaches disk; the durable notifications are never sent.
- Reading a write back, through any POV, shows that it committed, not that it is on disk: reads
  include the global POV's memory layers.
- After a restart, everything the server gives back is on disk, and a later crash won't take it
  back. The campaign checks this on every kill.

## A graceful stop

SIGTERM or SIGINT (`TServer::Shutdown`) works in this order:

1. **No new writes.** From the moment the stop begins `orlyi` commits no new write: a write
   that hasn't committed yet is refused with `insufficient_memory` (over the binary protocol, an
   error starting `insufficient memory`), which says the server is stopping. Nothing of it was
   written, so a client retries it once the server is back. Then every client connection is
   closed. So every write the server acknowledged is one the stop knows about (#769).
2. **Promote.** It waits for Tetris to promote every POV's backlog to the global POV, for as
   long as Tetris keeps promoting, up to `--stop_promote_budget_s` (default 300 s). Every 5 s it
   logs how much is left (`Tetris still promoting into [N] pov(s) after Ns: [U] updates ([E]
   entries) left to promote`). The wait ends early if Tetris promotes nothing for 30 s (or for
   the whole budget, if that is shorter), so a promotion that can't make progress doesn't hold
   the stop.
3. **Settle and flush.** It lets the release machinery settle for three periods of the slowest
   of `--replication_interval`, `--mem_interval` and `--durable_write_interval` (at least 100 ms
   each, so 300 ms by default), then writes every memory layer of the global POV to disk (#440,
   #744).

So a graceful stop loses no acknowledged write, unless:

- the promotion wait ran out (the budget, or 30 s without a promotion). `orlyi` then logs, at
  `LOG_ERR`, `stopped waiting for Tetris ... [U] acknowledged updates ([E] entries) were never
  promoted and WILL BE LOST`. One update is one transaction (a batch is one), so `U` is exactly
  the number of acknowledged transactions the restart won't give back (it can include a write
  that committed as the connections closed and never got its reply);
- a POV is paused (or its Tetris join was deferred under memory pressure, #250): nothing
  promotes its backlog, so it isn't waited for, and `orlyi` logs the same `WILL BE LOST` count
  for it, as the README's ephemeral-POV caveat says.

**Time.** A stop takes as long as the backlog takes to promote (up to the budget), plus the
disk merge in flight when it starts, plus the flush (each of the repo flush and the durable flush
is also bounded at 30 s). `orlyi` logs each step's time: `Tetris idle after` (with how many
updates it promoted), `merge runners stopped after` and `flushed after`. A stop with no backlog
takes about the 300 ms window.

How long the promotion can take depends on how much the POVs may hold and how fast Tetris
promotes. Each POV's backlog is capped at 1/32 of the Update pool in updates and of the Update
Entry pool in entries (#721), so eight busy POVs on the default pools can hold about 25,000
updates; Tetris promotes them at a few hundred to a few thousand a second depending on the
machine and its load, so a stop under sustained write load can take a minute or more. The
reporting port's `Writer Backlog` line shows what a stop would have to promote right now
(`unpromoted U updates / E entries`). Give `docker stop` (`--time`, 10 s by default) or your
service manager at least `--stop_promote_budget_s` of grace, or lower the budget and accept the
logged loss: a SIGKILL during the wait is a crash, and loses the whole backlog.

`tests/graceful_stop_test.sh` stops `orlyi` with SIGTERM under the campaign's write load and
requires every acknowledged write back after each restart; CI runs it on every push and pull
request. Its last stop is a long-load stop: the writers write until the backlogs hold 75% of
what the caps allow (or 30 s pass), whatever the clients' speed, so the stop has the most it can
ever have to promote (#769). `SIGNAL=TERM` runs the campaign the same way. Before #744 a graceful stop under load
usually hung instead, and `docker stop`'s SIGKILL then made it a crash.

The graceful-stop, kill-campaign and restart tests each use a unique disk instance per run,
reuse it across that run's restarts, and detach their own loop device on exit. The Python
drivers keep their work directories on violations or with `--keep`, but the retained images
do not share instance names with later runs. For parallel runs of either Python driver, pass
distinct `--port=<base>` values with non-overlapping four-port ranges; the restart test still
uses ports 19600–19603.
`python3 -m unittest discover -s tests -p stop_instance_test.py` checks instance isolation,
restart reuse and owned-resource cleanup without root or a server build.

## Power loss

A SIGKILL leaves the kernel's page cache alone, so the campaign doesn't test what a power cut
does to writes the kernel hadn't written to the device. The fault-injection harness
(`orly/indy/fault_injection.test.cc`) cuts power at chosen points in the durable path instead.

## How it is checked

`tests/kill_campaign.sh` runs `orlyi` on a loopback volume (it needs root, like
`tests/restart_test.sh`) with ten writers: safe and fast POVs, private and shared, two writers
sharing a POV, two POVs made `from` another POV, single writes and batches, and batches whose
calls evaluate an `if`, which Tetris replays before promoting them (#751). Each write puts a
key, bumps the writer's `+=` counter and bumps a `+=` total shared by every writer, in one
transaction. It SIGKILLs `orlyi` at a random moment (40 times by default), restarts it on the same
volume, and checks, against a ledger of what each writer sent and had acknowledged:

| check | what fails it |
|---|---|
| prefix | a writer's keys that came back have a hole |
| floor | a write that an earlier restart gave back is missing |
| phantom | a key came back that was never sent, or holds an older write's value |
| atomic | a batch came back in part |
| counter | a writer's `+=` counter differs from its key count, or the shared total from the sum |
| bound | more acknowledged transactions were lost than the Update pool held just before the kill, plus those acknowledged after that look |
| progress | over the whole campaign, none of a writer's writes ever came back after a restart: its POV was never promoted (#751) |
| open | the restart fails, or the #700 open check finds more than leaked blocks or a merge's leftover input |
| ephemeral | a POV from before the kill still accepts calls (#439) |

It prints a line per kill and a summary
(`KILL CAMPAIGN: kills=... recovered=... lost_per_kill=... max_lost=... violations=...`), and
exits nonzero on any violation. `NEGATIVE=rollback` is its negative control: it puts back an
older volume image before one restart, which is how a lost data file would look, and the floor
check must fail it.

CI runs 3 kills and the negative control in the release job on every push and pull request; the
`kill-campaign` workflow runs the full campaign on dispatch and on pull requests that change it.

## Measured

One 40-kill run on a release build (arm64 Linux, 16 cores, in Docker; seed 1791372360), with
loads of 1 to 6 s at about 1,500 transactions a second between kills:

    KILL CAMPAIGN: kills=40 recovered=40 lost_per_kill=min 82 / median 2066.5 / max 11153
    max_lost=11153 max_lost_txns=5855 max_lost_age_ms=3891 min_bound_margin=729 violations=0

Every check held on every kill. The losses are large next to the 40 ms flush because promotion
runs behind: the global POV had at most one memory layer waiting at each kill, so nearly all of a
kill's loss was writes still in their POVs' backlogs, the oldest acknowledged up to 3.9 s
earlier. Faster promotion would shrink the loss; nothing here bounds it in time.
