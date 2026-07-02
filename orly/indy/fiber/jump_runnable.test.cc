/* <orly/indy/fiber/jump_runnable.test.cc>

   Unit test for <orly/indy/fiber/jump_runnable.h>.

   The interesting property under test is the completion handshake between
   the fiber and the calling thread: the fiber must not lose the wakeup and
   must be completely done touching the TJumpRunnable before the caller can
   observe the completion flag and destroy it (#386).  The fixtures hammer
   that handshake; run them under ThreadSanitizer to catch regressions
   deterministically.

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
#include <stdexcept>
#include <thread>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Indy;
using namespace Orly::Indy::Fiber;

using TFramePoolMngr = TJumpRunnable::TFramePoolMngr;
using TLocalPool = TJumpRunnable::TFramePool::TThreadLocalPool;
using TDiskEvent = Disk::Util::TDiskController::TEvent;

/* Stand up one fiber runner on its own thread, hand 'body' the frame pool
   manager and the runner, then tear everything down in the order the frame
   machinery requires: the runner thread is joined before any frame pool
   dies (it frees jump frames back to their origin pools after the fiber
   switches back to the scheduler), and every thread-local pool is deleted
   before its manager.  Pools created by extra caller threads go into
   'orphaned_pools'; they are deleted only after the runner is joined. */
static void WithRunner(
    const function<void (TFramePoolMngr *, TRunner *, vector<TLocalPool *> &)> &body) {
  /* TJumpRunnable::Main() lazily creates a thread-local disk event pool on
     the runner thread, which requires the global manager to exist. */
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

FIXTURE(ManyJumps) {
  /* Each jump crosses the completion handshake once; iterate enough times
     that a lost wakeup or a use-after-free of the just-destroyed
     TJumpRunnable has plenty of chances to bite. */
  const size_t num_jumps = 10000;
  WithRunner([&](TFramePoolMngr *frame_pool_mngr, TRunner *runner, vector<TLocalPool *> &) {
    size_t count = 0;
    for (size_t i = 0; i < num_jumps; ++i) {
      TJumpRunnable jump([&count] {
        ++count;
      });
      jump(frame_pool_mngr, runner);
    }
    EXPECT_EQ(count, num_jumps);
  });
}

FIXTURE(ConcurrentJumps) {
  const size_t num_callers = 4;
  const size_t jumps_per_caller = 2500;
  WithRunner([&](TFramePoolMngr *frame_pool_mngr, TRunner *runner,
                 vector<TLocalPool *> &orphaned_pools) {
    atomic<size_t> count(0);
    /* Each caller thread gets its own thread-local frame pool on first use;
       hand them to WithRunner for deletion after the runner is joined -- the
       runner frees each jump's frame back to the pool it came from, possibly
       after the caller thread has already finished. */
    mutex pool_mutex;
    vector<thread> callers;
    for (size_t i = 0; i < num_callers; ++i) {
      callers.emplace_back([&] {
        for (size_t j = 0; j < jumps_per_caller; ++j) {
          TJumpRunnable jump([&count] {
            count.fetch_add(1, memory_order_relaxed);
          });
          jump(frame_pool_mngr, runner);
        }
        lock_guard<mutex> lock(pool_mutex);
        orphaned_pools.push_back(TFrame::LocalFramePool);
        TFrame::LocalFramePool = nullptr;
      });
    }
    for (auto &caller : callers) {
      caller.join();
    }
    EXPECT_EQ(count.load(), num_callers * jumps_per_caller);
  });
}

FIXTURE(PropagatesException) {
  WithRunner([&](TFramePoolMngr *frame_pool_mngr, TRunner *runner, vector<TLocalPool *> &) {
    auto throwing_jump = [&] {
      TJumpRunnable jump([] {
        throw runtime_error("boom");
      });
      jump(frame_pool_mngr, runner);
      /* ~TJumpRunnable rethrows the fiber's exception here. */
    };
    EXPECT_THROW_FUNC(runtime_error, throwing_jump);
  });
}
