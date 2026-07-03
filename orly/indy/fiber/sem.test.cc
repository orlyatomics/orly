/* <orly/indy/fiber/sem.test.cc>

   Unit test for the fiber sync primitives in <orly/indy/fiber/fiber.h>:
   TSingleSem, TSem, and TSafeSync.

   The interesting property under test is the wake/destroy handshake: a
   waiter that owns the primitive on its stack must be free to destroy it
   the moment its wait returns, so the signaller must not touch the
   primitive after making the waiter runnable (#386, the same contract
   jump_runnable.test.cc pins for TJumpRunnable).  Each round constructs a
   primitive in a reused storage slot, waits on it from a fiber, signals it
   from a foreign thread, and destroys it on wake -- the next round's
   constructor immediately rewrites the same bytes, so under ThreadSanitizer
   any signaller touch that races the waiter's return (e.g. releasing the
   spinlock after ScheduleFrame) shows up deterministically.

   Copyright 2010-2026 Atomic Kismet Company

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

/* jump_runnable.h is not self-contained: EnsureLocalDiskEventPool() needs the
   disk controller's event type, whose header its other users include first. */
#include <orly/indy/disk/util/volume_manager.h>
#include <orly/indy/fiber/jump_runnable.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Indy;
using namespace Orly::Indy::Fiber;

using TFramePoolMngr = TJumpRunnable::TFramePoolMngr;
using TLocalPool = TJumpRunnable::TFramePool::TThreadLocalPool;
using TDiskEvent = Disk::Util::TDiskController::TEvent;

/* Same scaffolding as jump_runnable.test.cc: stand up one fiber runner on its
   own thread, hand 'body' the frame pool manager and the runner, then tear
   everything down in the order the frame machinery requires.  Pools created
   by extra caller threads go into 'orphaned_pools'; they are deleted only
   after the runner is joined. */
static void WithRunner(
    const function<void (TFramePoolMngr *, TRunner *, vector<TLocalPool *> &)> &body) {
  TDiskEvent::InitializeDiskEventPoolManager(64);
  TRunner::TRunnerCons runner_cons(1);
  TRunner runner(runner_cons);
  TFramePoolMngr frame_pool_manager(32, 64 * 1024, &runner);
  assert(!TFrame::LocalFramePool);
  TFrame::LocalFramePool = new TLocalPool(&frame_pool_manager);
  thread runner_thread([&] {
    runner.Run();
    delete TDiskEvent::LocalEventPool;
    TDiskEvent::LocalEventPool = nullptr;
  });
  vector<TLocalPool *> orphaned_pools;
  exception_ptr err;
  try {
    body(&frame_pool_manager, &runner, orphaned_pools);
  } catch (...) {
    err = current_exception();
  }
  runner.ShutDown();
  runner_thread.join();
  for (auto *pool : orphaned_pools) {
    delete pool;
  }
  delete TFrame::LocalFramePool;
  TFrame::LocalFramePool = nullptr;
  if (err) {
    rethrow_exception(err);
  }
}

static void SemWait(TSingleSem &sem) { sem.Pop(); }
static void SemSignal(TSingleSem &sem) { sem.Push(); }
static void SemWait(TSem &sem) { sem.Pop(); }
static void SemSignal(TSem &sem) { sem.Push(); }
static void SemWait(TSafeSync &sync) { sync.Sync(); }
static void SemSignal(TSafeSync &sync) { sync.Complete(); }

template <typename TPrim>
static TPrim *ConstructPrim(void *storage) {
  if constexpr (is_same_v<TPrim, TSafeSync>) {
    return new (storage) TPrim(1UL);
  } else {
    return new (storage) TPrim();
  }
}

/* One hammer lane: 'num_rounds' construct / publish / wait / signal /
   destroy cycles over a single reused storage slot, with a dedicated
   signaller thread.  Counts successful wakeups so a lost wakeup fails the
   fixture (rather than hanging it) via the EXPECT in the caller. */
template <typename TPrim>
static size_t HammerFreeOnWake(TFramePoolMngr *frame_pool_mngr, TRunner *runner, size_t num_rounds) {
  atomic<TPrim *> slot(nullptr);
  atomic<bool> stop(false);
  thread signaller([&] {
    for (;;) {
      TPrim *prim = slot.exchange(nullptr, memory_order_acq_rel);
      if (prim) {
        SemSignal(*prim);
      } else if (stop.load(memory_order_acquire)) {
        break;
      } else {
        this_thread::yield();
      }
    }
  });
  alignas(TPrim) unsigned char storage[sizeof(TPrim)];
  size_t wakeups = 0;
  for (size_t i = 0; i < num_rounds; ++i) {
    TJumpRunnable jump([&] {
      TPrim *prim = ConstructPrim<TPrim>(storage);
      slot.store(prim, memory_order_release);
      SemWait(*prim);
      /* We own the primitive; the wait returning must mean the signaller is
         completely done with it. */
      prim->~TPrim();
      ++wakeups;
    });
    jump(frame_pool_mngr, runner);
  }
  stop.store(true, memory_order_release);
  signaller.join();
  return wakeups;
}

template <typename TPrim>
static void SequentialHammer() {
  const size_t num_rounds = 10000;
  WithRunner([&](TFramePoolMngr *frame_pool_mngr, TRunner *runner, vector<TLocalPool *> &) {
    EXPECT_EQ(HammerFreeOnWake<TPrim>(frame_pool_mngr, runner, num_rounds), num_rounds);
  });
}

/* Four caller threads, each with its own lane (slot, storage, signaller),
   all parking and waking fibers on the same runner -- exercises the handoff
   with several frames in flight. */
template <typename TPrim>
static void ConcurrentHammer() {
  const size_t num_callers = 4;
  const size_t rounds_per_caller = 2500;
  WithRunner([&](TFramePoolMngr *frame_pool_mngr, TRunner *runner,
                 vector<TLocalPool *> &orphaned_pools) {
    atomic<size_t> wakeups(0);
    mutex pool_mutex;
    vector<thread> callers;
    for (size_t i = 0; i < num_callers; ++i) {
      callers.emplace_back([&] {
        wakeups.fetch_add(
            HammerFreeOnWake<TPrim>(frame_pool_mngr, runner, rounds_per_caller),
            memory_order_relaxed);
        lock_guard<mutex> lock(pool_mutex);
        orphaned_pools.push_back(TFrame::LocalFramePool);
        TFrame::LocalFramePool = nullptr;
      });
    }
    for (auto &caller : callers) {
      caller.join();
    }
    EXPECT_EQ(wakeups.load(), num_callers * rounds_per_caller);
  });
}

FIXTURE(SingleSemFreeOnWake) {
  SequentialHammer<TSingleSem>();
}

FIXTURE(SemFreeOnWake) {
  SequentialHammer<TSem>();
}

FIXTURE(SafeSyncFreeOnWake) {
  SequentialHammer<TSafeSync>();
}

FIXTURE(SingleSemFreeOnWakeConcurrent) {
  ConcurrentHammer<TSingleSem>();
}

FIXTURE(SemFreeOnWakeConcurrent) {
  ConcurrentHammer<TSem>();
}

FIXTURE(SafeSyncFreeOnWakeConcurrent) {
  ConcurrentHammer<TSafeSync>();
}
