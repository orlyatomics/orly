# TServer teardown design note (#440, roadmap keystone)

Originally a pre-implementation design note; now the record of what landed,
the constraints that shaped it, and what deliberately remains. All three
slices are in (A: `de4a38c8` / PR #442, B: `250294ba` / PR #447, C: PR #459).

## Where teardown stands

- orlyi: SIGINT → `TServer::Shutdown()` → destroy → clean exit (slice A).
  A graceful stop makes all data durable with no flush-window sleep
  (slice B; `tests/restart_test.sh` gates this — its remaining `sleep`s are
  startup/exit polling, not flush windows).
- orlyc: constructs, runs, and fully destroys the server on every test run
  (slice C) — the old leak-and-`_exit` policy is gone, so every orlyc
  invocation doubles as a construct/run/destroy smoke of the whole server.
  `_exit` survives only as the fallback when teardown itself throws
  (a deterministic non-zero exit beats a hang).
- `Shutdown()` destroys everything: service loops joined, dirty mem layers
  flushed, tetris deleted, DurableManager cleared and reset,
  GlobalRepo/RepoManager reset. After it, `~TServer` touches only
  non-fiber state.
- CI: the `asan-smoke` job builds orlyc with `asan.jhm` and runs lang
  tests through it, policing the teardown paths at the memory level
  (use-after-free, not leak accounting — see "deliberate leaks" below).
  `-U_FORTIFY_SOURCE` in the config is load-bearing: Ubuntu's default
  fortify (active at the sanitizer build's `-O1`) aborts on the fibers'
  cross-stack longjmps.  Unlike TSan (#194), no fiber-switch annotations
  proved necessary for a clean run.  The smoke's very first run caught
  two real teardown bugs: a `TCmd` use-after-scope in orlyc's
  Shutdown-on-every-path flow, and `Base::MemAlignedAlloc`'s
  posix_memalign/operator-delete alloc-dealloc mismatch on every aligned
  disk buffer.

## Constraints (empirical — the reasons the code looks the way it does)

The original three, from the #436 work:

1. `~TDurableManager` runs `TSingleSem::Pop()` which asserts
   `TRunner::LocalRunner` — fiber-entangled teardown must run ON a fiber.
   Hence the `TJumpRunnable teardown_jumper` in `Shutdown()`.
2. `~TServer` must run on a non-fiber thread to free the fiber frame it
   allocated, yet teardown paths take fiber locks (`StopAllPlayers`).
   Hence: fiber-needing work happens in `Shutdown()`'s jumper, and the
   post-Shutdown destructor is non-fiber-safe by construction.
3. Stopping without flushing loses whatever the mergers had not written
   (durability window = merge cadence). Hence flush-on-shutdown (slice B),
   done inline by `FlushMemMerges` so it cannot depend on merge scheduling.

Discovered during slice C (each cost a real bug):

4. **A stop is only a flag plus a wake.** Signaling a service loop proves
   nothing about when it exits; destroying a manager while a loop is
   mid-body is a use-after-free. Every loop therefore counts itself in on
   entry and pushes an exited-latch on the way out (exception-safe,
   `Base::TPushOnExit`), and `Shutdown()` joins them all before the
   teardown jumper runs. Order matters twice over: the settle window must
   come BEFORE the stops (a stopped release loop can no longer drain its
   queue), and the merge runners must be stopped and joined BEFORE
   `FlushMemMerges` (so the flush is the queue's only drainer and a
   mid-step re-enqueue cannot be lost).
5. **Dirty repos pin themselves open.** A written repo holds a
   `MakeDirty()` self-ptr until its updates are released (tetris
   promotion) or its mem layer merges out. Paused-and-written repos —
   every compile-time test pov — are never promoted, so the pin never
   drops; `~TManager` now calls `ReleaseDirtySelfPins()` first, and the
   close cascade force-releases parent-repo ptrs. Never seen before
   slice C because `PreDtor` was dead code until teardown became real.
6. **The scheduler silently starves jobs past the worker cap.**
   `TScheduler::Schedule` queues but never launches another worker once
   the cap is reached, and long-lived service jobs never return their
   worker. An under-provisioned cap therefore silently never runs the
   last N jobs (orlyc's old 8-worker policy never ran ~6 of them).
   `TScheduler::Shutdown` now logs any jobs that were scheduled but never
   ran; orlyc provisions 32.

## Deliberate leaks (why the ASan smoke disables leak detection)

Teardown frees every real resource but intentionally leaks a few pool
slabs: thread-local frame/event pools created on scheduler-worker threads
cannot be reclaimed without thread-exit hooks, so their MANAGERS are
released, not destroyed (`FramePoolManager.release()`,
`DiskEventPoolManager.release()`). Similarly, `Clear()`/`PreDtor` log and
LEAK an object that still has live ptrs rather than free it out from under
them (release builds; debug asserts). The ASan smoke exists to catch
use-after-free and heap corruption in the teardown paths, so it runs with
`detect_leaks=0`; turning LSan on would require suppressions for exactly
these documented leaks and would gate on noise, not signal.

## Known follow-ons (tracked; also noted at the code sites)

- **#460 — drain live client connections before `Clear()`**: orlyi under
  load can reach `Clear()` while a connection still holds its session ptr;
  today that is a logged leak (and a debug assert), not a drain. The real
  fix is closing established connections and joining their serving fibers
  before the teardown jumper.
- **#461 — master-mode replication**: a replicate loop wedged in a remote
  `future->Sync()` holds `JoinReplicationServices()` until the RPC
  resolves; interrupting in-flight RPCs is future work.
- **#462 — never-scheduled fibers (FIXED)**: a fiber that was latched but
  never granted a worker could not be joined (the started/exited handshake
  skips it) and would touch freed state if it ever ran mid-teardown. Fixed
  by scheduler job cancellation: `TScheduler::ScheduleCancelable`/`Cancel`
  give every long-lived host job's owner an exact cancel-or-join at
  teardown (`if (!Cancel(handle)) exited_latch.Pop()`), so a never-started
  host can neither start late nor be waited for — and a fiber latched onto
  a cancelled host can never run at all, which is what makes the
  fiber-level started/exited skips sound. `TScheduler::Shutdown` also
  drains the queue, and its starved-jobs warning now flags only live,
  never-cancelled leftovers (an owner contract violation).

## The #463 teardown race (fixed)

The flake class this arc's instrumentation finally caught (four one-shot
SIGSEGVs and two hangs, all post-test, never on rerun) was not in the
teardown *ordering* at all but in the fiber runtime's cross-runner
channel: every `TRunner::Run` lap dereferenced every peer `TRunner`
(`RunnerArray[i]->QueueArray[me]`), yet runners die at different times —
`~TDurableManager` destroys its writer/merger schedulers and
`delete TetrisManager` its fiber scheduler while a dozen service runners
still poll. A peer that loaded the pointer just before the destruction
read freed heap being actively churned by the same teardown's pool
traffic; a garbage read longjmps a phantom frame (the crash form), a
zero read silently drops a queued handoff (the hang form). The handoff
slots now live in a `TRunnerCons`-owned all-pairs matrix that outlives
every runner, so polling touches no peer object; two adjacent
unjoined-loop windows (the `LaunchSlowFiberSched` scheduler jobs hosting
TServer's service runners, and `TFileService`'s `BGScheduler` host) got
the standard entered/exited-latch join.
