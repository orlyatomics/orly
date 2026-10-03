/* <orly/indy/disk/result.test.cc>

   Unit test for TCompletionTrigger in <orly/indy/disk/result.h> (#452).

   The property under test is the wake/destroy handshake, the same contract
   sem.test.cc pins for the fiber sync primitives: the trigger is typically
   stack-owned by the waiter and destroyed the moment Wait() returns (or
   throws), so no completer may touch the trigger after the waiter can
   observe completion.  Each hammer round constructs a trigger in a reused
   storage slot, fires its completions from a foreign thread, and destroys
   it on wake -- under ThreadSanitizer any completer touch that races the
   waiter's return (the old unlocked NumFinished bump / Result read) shows
   up deterministically.

   Two deterministic fixtures pin the #452 error contract: Wait() only
   returns after ALL completions have arrived even when one of them errored
   (no early wake with stragglers in flight), including when the error lands
   before the waiter ever parks (which used to leave the trailing successes
   unable to wake it at all -- a hang).

   #590 adds the unwind case: a trigger destroyed by an exception before
   anyone called Wait() must wait out its outstanding completions (a hammer
   like FreeOnWake, plus a deterministic ordering check), and the fiber that
   parks to do so must not leak its in-flight exception into the other
   fibers on its runner.

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

#include <orly/indy/disk/result.h>

#include <atomic>
#include <exception>
#include <functional>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Indy;
using namespace Orly::Indy::Disk;
using namespace Orly::Indy::Fiber;

using TFramePoolMngr = TJumpRunnable::TFramePoolMngr;
using TLocalPool = TJumpRunnable::TFramePool::TThreadLocalPool;
using TDiskEvent = Disk::Util::TDiskController::TEvent;

/* Frame pools of extra threads that jumped into the runner.  The runner frees a
   finished frame back to its origin pool after the jump has returned, so such a
   pool may only go once the runner has stopped: WithRunner deletes them then. */
static vector<TLocalPool *> ForeignPools;

/* Same scaffolding as sem.test.cc / jump_runnable.test.cc: stand up one fiber
   runner on its own thread, hand 'body' the frame pool manager and the runner,
   then tear everything down in the order the frame machinery requires. */
static void WithRunner(const function<void (TFramePoolMngr *, TRunner *)> &body) {
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
  exception_ptr err;
  try {
    body(&frame_pool_manager, &runner);
  } catch (...) {
    err = current_exception();
  }
  runner.ShutDown();
  runner_thread.join();
  for (TLocalPool *pool : ForeignPools) {
    delete pool;
  }
  ForeignPools.clear();
  delete TFrame::LocalFramePool;
  TFrame::LocalFramePool = nullptr;
  if (err) {
    rethrow_exception(err);
  }
}

/* One round's completion work, published to the completer thread.  The waiter
   may destroy the TRIGGER the moment Wait() returns, but this descriptor lives
   on the waiter's stack until the jump completes -- which the #452 contract
   itself makes safe: Wait() cannot return before the final Callback. */
struct TRound {
  TCompletionTrigger *Trigger;
  size_t NumCompletions;
  size_t ErrAt;  // index whose completion reports Error; >= NumCompletions for all-success
};

/* 'num_rounds' construct / publish / wait / destroy cycles over one reused
   storage slot, completions fired from a dedicated foreign thread.  Every
   fourth round injects an Error mid-stream so the sticky-error path runs
   under the same lifetime hammer.  Returns the number of clean wakeups. */
static size_t HammerFreeOnWake(TFramePoolMngr *frame_pool_mngr, TRunner *runner, size_t num_rounds) {
  atomic<TRound *> slot(nullptr);
  atomic<bool> stop(false);
  thread completer([&] {
    for (;;) {
      TRound *round = slot.exchange(nullptr, memory_order_acq_rel);
      if (round) {
        /* Copy the descriptor's fields BEFORE firing: the moment the final Callback returns,
           the waiter may have woken, destroyed the trigger, and reused the stack region the
           descriptor lives in -- even the for-loop's own final condition re-read would be a
           touch-after-free (the hammer caught exactly that in an earlier draft of itself). */
        TCompletionTrigger *const trigger = round->Trigger;
        const size_t num_completions = round->NumCompletions;
        const size_t err_at = round->ErrAt;
        for (size_t i = 0; i < num_completions; ++i) {
          trigger->Callback(i == err_at ? Error : Success, "hammer injected error");
        }
      } else if (stop.load(memory_order_acquire)) {
        break;
      } else {
        this_thread::yield();
      }
    }
  });
  alignas(TCompletionTrigger) unsigned char storage[sizeof(TCompletionTrigger)];
  const size_t num_completions = 4;
  size_t wakeups = 0;
  for (size_t i = 0; i < num_rounds; ++i) {
    TJumpRunnable jump([&, i] {
      TCompletionTrigger *trigger = new (storage) TCompletionTrigger();
      trigger->WaitForMore(num_completions);
      TRound round{trigger, num_completions, (i % 4 == 0) ? (i % num_completions) : num_completions};
      slot.store(&round, memory_order_release);
      bool threw = false;
      try {
        trigger->Wait();
      } catch (const TDiskError &) {
        threw = true;
      }
      /* We own the trigger; Wait() returning (or throwing) must mean every
         completer touch is done. */
      trigger->~TCompletionTrigger();
      if (threw == (round.ErrAt < num_completions)) {
        ++wakeups;
      }
    });
    jump(frame_pool_mngr, runner);
  }
  stop.store(true, memory_order_release);
  completer.join();
  return wakeups;
}

FIXTURE(FreeOnWake) {
  const size_t num_rounds = 10000;
  WithRunner([&](TFramePoolMngr *frame_pool_mngr, TRunner *runner) {
    EXPECT_EQ(HammerFreeOnWake(frame_pool_mngr, runner, num_rounds), num_rounds);
  });
}

/* An error must not wake the waiter while completions are still outstanding:
   the completer fires Success then Error, parks itself until the waiter has
   had every chance to (wrongly) wake, then fires the final Success.  The
   waiter must observe the error only at the full-count barrier, i.e. after
   'all_sent' is up. */
FIXTURE(ErrorWakesOnlyAtBarrier) {
  WithRunner([&](TFramePoolMngr *frame_pool_mngr, TRunner *runner) {
    TCompletionTrigger trigger;
    trigger.WaitForMore(3);
    atomic<bool> all_sent(false);
    bool saw_error = false;
    bool woke_after_all_sent = false;
    thread completer([&] {
      trigger.Callback(Success, nullptr);
      trigger.Callback(Error, "second completion failed");
      /* Give a buggy early wake ample time to happen before the final send. */
      this_thread::sleep_for(chrono::milliseconds(50));
      all_sent.store(true, memory_order_release);
      trigger.Callback(Success, nullptr);
    });
    TJumpRunnable jump([&] {
      try {
        trigger.Wait();
      } catch (const TDiskError &) {
        saw_error = true;
      }
      woke_after_all_sent = all_sent.load(memory_order_acquire);
    });
    jump(frame_pool_mngr, runner);
    completer.join();
    EXPECT_TRUE(saw_error);
    EXPECT_TRUE(woke_after_all_sent);
  });
}

/* An error that lands BEFORE the waiter parks used to put the trigger in an
   error state whose remaining completions were ignored entirely, so nothing
   ever woke the parked waiter (#452's hang case).  Pin: park after the error,
   then the trailing success must still wake us with the sticky error. */
FIXTURE(ErrorBeforeParkStillWakes) {
  WithRunner([&](TFramePoolMngr *frame_pool_mngr, TRunner *runner) {
    TCompletionTrigger trigger;
    trigger.WaitForMore(2);
    atomic<bool> error_landed(false);
    atomic<bool> parked(false);
    bool saw_error = false;
    thread completer([&] {
      trigger.Callback(Error, "first completion failed");
      error_landed.store(true, memory_order_release);
      /* Hold the final completion until the waiter is committed to parking. */
      while (!parked.load(memory_order_acquire)) {
        this_thread::yield();
      }
      this_thread::sleep_for(chrono::milliseconds(10));
      trigger.Callback(Success, nullptr);
    });
    TJumpRunnable jump([&] {
      while (!error_landed.load(memory_order_acquire)) {
        Yield();
      }
      parked.store(true, memory_order_release);
      try {
        trigger.Wait();
      } catch (const TDiskError &) {
        saw_error = true;
      }
    });
    jump(frame_pool_mngr, runner);
    completer.join();
    EXPECT_TRUE(saw_error);
  });
}

/* #590: the unwind twin of HammerFreeOnWake.  Each round's trigger lives on
   the fiber's stack, gets its completions published to the foreign
   completer, and is then destroyed by an exception without Wait() ever being
   called.  The destructor must not return (and free the stack) until every
   completion has landed; under ASan/TSan a completer touch after that shows
   up.  The round descriptor is declared BEFORE the trigger so it outlives the
   trigger's destructor -- the completer reads it before firing. */
static size_t HammerDrainOnUnwind(TFramePoolMngr *frame_pool_mngr, TRunner *runner, size_t num_rounds) {
  atomic<TRound *> slot(nullptr);
  atomic<bool> stop(false);
  thread completer([&] {
    for (;;) {
      TRound *round = slot.exchange(nullptr, memory_order_acq_rel);
      if (round) {
        TCompletionTrigger *const trigger = round->Trigger;
        const size_t num_completions = round->NumCompletions;
        const size_t err_at = round->ErrAt;
        for (size_t i = 0; i < num_completions; ++i) {
          trigger->Callback(i == err_at ? Error : Success, "hammer injected error");
        }
      } else if (stop.load(memory_order_acquire)) {
        break;
      } else {
        this_thread::yield();
      }
    }
  });
  const size_t num_completions = 4;
  size_t unwound = 0;
  for (size_t i = 0; i < num_rounds; ++i) {
    TJumpRunnable jump([&, i] {
      try {
        TRound round{nullptr, num_completions, (i % 4 == 0) ? (i % num_completions) : num_completions};
        TCompletionTrigger trigger;
        round.Trigger = &trigger;
        trigger.WaitForMore(num_completions);
        slot.store(&round, memory_order_release);
        throw runtime_error("abandon the trigger");
      } catch (const runtime_error &) {
        ++unwound;
      }
    });
    jump(frame_pool_mngr, runner);
  }
  stop.store(true, memory_order_release);
  completer.join();
  return unwound;
}

FIXTURE(DrainOnUnwind) {
  const size_t num_rounds = 10000;
  WithRunner([&](TFramePoolMngr *frame_pool_mngr, TRunner *runner) {
    EXPECT_EQ(HammerDrainOnUnwind(frame_pool_mngr, runner, num_rounds), num_rounds);
  });
}

/* Sets a flag when destroyed; declared after a trigger, it runs just before
   the trigger's destructor does. */
struct TOnDestroy {
  atomic<bool> &Flag;
  ~TOnDestroy() { Flag.store(true, memory_order_release); }
};

/* Deterministic ordering: the destructor returns only after the final
   completion, which the completer holds back until well after the unwind
   has reached the trigger.  An error among the completions is not thrown
   from the destructor (the frame is already unwinding). */
FIXTURE(DrainWaitsForLastCompletion) {
  WithRunner([&](TFramePoolMngr *frame_pool_mngr, TRunner *runner) {
    atomic<TCompletionTrigger *> published(nullptr);
    atomic<bool> unwinding(false);
    atomic<bool> all_sent(false);
    bool caught_own = false;
    bool returned_after_all_sent = false;
    thread completer([&] {
      TCompletionTrigger *trigger;
      while (!(trigger = published.load(memory_order_acquire))) {
        this_thread::yield();
      }
      trigger->Callback(Error, "first completion failed");
      while (!unwinding.load(memory_order_acquire)) {
        this_thread::yield();
      }
      this_thread::sleep_for(chrono::milliseconds(50));
      all_sent.store(true, memory_order_release);
      trigger->Callback(Success, nullptr);
    });
    TJumpRunnable jump([&] {
      try {
        TCompletionTrigger trigger;
        TOnDestroy on_destroy{unwinding};
        trigger.WaitForMore(2);
        published.store(&trigger, memory_order_release);
        throw runtime_error("own error");
      } catch (const runtime_error &ex) {
        caught_own = (string(ex.what()) == "own error");
        returned_after_all_sent = all_sent.load(memory_order_acquire);
      }
    });
    jump(frame_pool_mngr, runner);
    completer.join();
    EXPECT_TRUE(caught_own);
    EXPECT_TRUE(returned_after_all_sent);
  });
}

/* The drain parks a fiber in the middle of an unwind.  The C++ runtime keeps
   the in-flight exception in per-THREAD state, so unless the fiber switch
   swaps it, every other fiber on the runner sees that exception as its own:
   std::uncaught_exceptions() reads 1 there (the index-file destructors use it
   to tell an abandoned build from a finished one), and its own throw/catch
   runs on top of the parked fiber's state.  Fiber A parks mid-unwind on the
   only runner; fiber B then runs there, and must see a clean slate. */
FIXTURE(ParkedUnwindDoesNotLeakExceptionState) {
  WithRunner([&](TFramePoolMngr *frame_pool_mngr, TRunner *runner) {
    TCompletionTrigger *trigger_a = nullptr;
    atomic<bool> a_unwinding(false);
    int b_uncaught = -1;
    bool b_caught_own = false;
    int b_uncaught_after = -1;
    bool a_caught_own = false;
    int a_uncaught_in_catch = -1;
    thread thread_a([&] {
      TJumpRunnable jump([&] {
        try {
          TCompletionTrigger trigger;
          TOnDestroy on_destroy{a_unwinding};
          trigger.WaitForOneMore();
          trigger_a = &trigger;
          throw runtime_error("a's error");
        } catch (const runtime_error &ex) {
          a_caught_own = (string(ex.what()) == "a's error");
          a_uncaught_in_catch = uncaught_exceptions();
        }
      });
      jump(frame_pool_mngr, runner);
      ForeignPools.push_back(TFrame::LocalFramePool);  // read after thread_a.join()
      TFrame::LocalFramePool = nullptr;
    });
    /* A runs alone on the runner until it parks in the trigger's destructor;
       B's frame can't be picked up before then. */
    while (!a_unwinding.load(memory_order_acquire)) {
      this_thread::yield();
    }
    TJumpRunnable jump_b([&] {
      b_uncaught = uncaught_exceptions();
      try {
        throw logic_error("b's error");
      } catch (const logic_error &ex) {
        b_caught_own = (string(ex.what()) == "b's error");
      }
      b_uncaught_after = uncaught_exceptions();
    });
    jump_b(frame_pool_mngr, runner);
    trigger_a->Callback(Success, nullptr);
    thread_a.join();
    EXPECT_EQ(b_uncaught, 0);
    EXPECT_TRUE(b_caught_own);
    EXPECT_EQ(b_uncaught_after, 0);
    EXPECT_TRUE(a_caught_own);
    EXPECT_EQ(a_uncaught_in_catch, 0);
  });
}
