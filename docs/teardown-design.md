# TServer teardown design note (#440, roadmap keystone)

## Why nothing destroys a TServer today

- orlyi: `RunUntilCtrlC` blocks in `sigsuspend`; SIGINT stops the scheduler
  and the process exits with fiber runners dying mid-syscall ("FATAL ERROR:
  Fiber Runner caught exception ... Interrupted system call"). `~TServer`
  never runs.
- orlyc: documented leak-and-`_exit` policy in `RunTestsOnIndy`.
- `~TServer` exists (WsRunner stop, TetrisManager delete, DurableManager
  Clear/reset, GlobalRepo/RepoManager reset, frame free) but is dead code.

## Proven blockers (empirical, from #436 work)

1. `~TDurableManager` runs `TSingleSem::Pop()` which asserts
   `TRunner::LocalRunner` — requires fiber context; member destructors run
   on whatever thread drops the last reference (non-fiber).
2. The orlyc comment: `~TServer` must run on a non-fiber thread to free the
   fiber frame it allocated (`Frame` via `LocalFramePool`), yet teardown
   paths take fiber locks (`StopAllPlayers`) asserting fiber context.
3. Un-flushed state: stopping without flushing loses whatever the mergers
   had not written (durability window = merge cadence, #440).

## Plan

1. **Inventory** every fiber-context-requiring teardown path:
   `TSingleSem`, `TCompletionTrigger`, `StopAllPlayers`, tetris player
   teardown, durable manager clear, repo manager reset. Also inventory all
   threads TServer owns (WsThread, Slow/Fast runner vecs, BGFastRunner via
   scheduler, WS server threads, durable/merge threads).
2. **`TServer::Shutdown()`** — explicit, called before destruction:
   - stop accepting (close listeners, Ws.reset())
   - quiesce writers, flush mem layers + durable layers (closes #440)
   - run fiber-needing teardown ON a fiber (TJumpRunnable onto a fast
     runner), incl. tetris stop + durable manager clear
   - shut down + join all runner threads (order: ws, fast, slow, bg)
   - after Shutdown(), ~TServer only touches non-fiber state
3. **Wire orlyi**: SIGINT → Shutdown → destroy → clean exit. Replace
   orlyc's leak-and-_exit with the same (keeps _exit as fallback).
4. **Test**: extend tests/restart_test.sh — graceful stop must make data
   durable with NO flush-window sleep; add ASan smoke of construct+destroy.

## Sequencing (PR-sized)

- PR A: inventory + Shutdown() skeleton stopping/joining threads only
  (no flush), orlyi wiring, restart test keeps sleep.
- PR B: flush-on-shutdown (drop the sleeps in restart_test.sh) → #440.
- PR C: make ~TServer safe after Shutdown; orlyc drops _exit; ASan smoke.
