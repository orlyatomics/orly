/* <orly/server/admission_wait.test.cc>

   Unit test for <orly/server/admission_wait.h>.

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

/* jump_runnable.h is not self-contained: see orly/indy/fiber/sem.test.cc. */
#include <orly/indy/disk/util/volume_manager.h>
#include <orly/indy/fiber/jump_runnable.h>

#include <orly/server/admission_wait.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace chrono;
using namespace Orly::Indy;
using namespace Orly::Indy::Fiber;
using namespace Orly::Server;

using TFramePoolMngr = TJumpRunnable::TFramePoolMngr;
using TLocalPool = TJumpRunnable::TFramePool::TThreadLocalPool;
using TDiskEvent = Disk::Util::TDiskController::TEvent;

/* 100 updates and 1,000 entries, a quarter kept for the merges: writers may hold 750 entries,
   and once refusing, are let in again at 688 (the reserve plus a quarter of it kept free). An
   admission of n entries takes 2 updates and 2n entries. The filler below holds 680, which fits
   either way, and a write of 40 (80 entries) then fits neither way. */
Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update");
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry");

static void InitPools() {
  static once_flag once;
  call_once(once, [] {
    TUpdate::InitUpdatePool(100UL);
    TUpdate::InitEntryPool(1000UL);
    TUpdate::SetPoolReservePct(25UL);
  });
}

/* A writer: on its own thread, jumps onto the runner and admits num_entries through wait,
   holding the admission until it is destroyed. */
class TWriter {
  public:

  TWriter(TFramePoolMngr *mngr, TRunner *runner, TAdmissionWait &wait, size_t num_entries, atomic<size_t> *seq = nullptr)
      : Thread([this, mngr, runner, &wait, num_entries, seq] {
          TJumpRunnable jump([&] {
            Result = wait.Admit(Admission, num_entries);
            if (seq) {
              Order = ++*seq;
            }
          });
          jump(mngr, runner);
          Done = true;
          lock_guard<mutex> lock(PoolMutex);
          Orphans.push_back(TFrame::LocalFramePool);
          TFrame::LocalFramePool = nullptr;
        }) {}

  ~TWriter() {
    Join();
  }

  void Join() {
    if (Thread.joinable()) {
      Thread.join();
    }
  }

  TUpdate::TWriteAdmission Admission;

  TAdmissionWait::TResult Result;

  size_t Order = 0UL;

  atomic<bool> Done {false};

  static mutex PoolMutex;

  static vector<TLocalPool *> Orphans;

  private:

  thread Thread;

};

mutex TWriter::PoolMutex;

vector<TLocalPool *> TWriter::Orphans;

/* One fiber runner on its own thread, as in sem.test.cc. */
static void WithRunner(const function<void (TFramePoolMngr *, TRunner *)> &body) {
  InitPools();
  TDiskEvent::InitializeDiskEventPoolManager(64);
  TRunner::TRunnerCons runner_cons(1);
  TRunner runner(runner_cons);
  TFramePoolMngr frame_pool_manager(32, 64 * 1024, &runner);
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
  for (auto *pool : TWriter::Orphans) {
    delete pool;
  }
  TWriter::Orphans.clear();
  delete TFrame::LocalFramePool;
  TFrame::LocalFramePool = nullptr;
  if (err) {
    rethrow_exception(err);
  }
}

static void WaitUntil(const function<bool ()> &cond) {
  const auto give_up = steady_clock::now() + seconds(10);
  while (!cond() && steady_clock::now() < give_up) {
    this_thread::sleep_for(milliseconds(1));
  }
}

/* With room, a write is admitted at once and doesn't queue. */
FIXTURE(AdmitsAtOnceWithRoom) {
  WithRunner([](TFramePoolMngr *mngr, TRunner *runner) {
    TAdmissionWait wait(milliseconds(10000));
    {
      TWriter writer(mngr, runner, wait, 10UL);
      writer.Join();
      EXPECT_TRUE(writer.Result.Admitted);
      EXPECT_FALSE(writer.Result.Waited);
    }
    EXPECT_EQ(wait.GetWaited(), 0UL);
  });
}

/* A write that doesn't fit waits, and is admitted when room comes back: woken by the release,
   well before its 10 s bound. */
FIXTURE(WaitsForRoom) {
  WithRunner([](TFramePoolMngr *mngr, TRunner *runner) {
    TAdmissionWait wait(milliseconds(10000));
    auto filler = make_unique<TUpdate::TWriteAdmission>();
    EXPECT_TRUE(filler->TryAcquire(340UL));
    {
      TWriter writer(mngr, runner, wait, 40UL);
      WaitUntil([&] { return wait.GetWaiting() == 1UL; });
      EXPECT_EQ(wait.GetWaiting(), 1UL);
      this_thread::sleep_for(milliseconds(50));
      EXPECT_FALSE(writer.Done.load());
      const auto released = steady_clock::now();
      filler.reset();
      writer.Join();
      EXPECT_LT(steady_clock::now() - released, seconds(2));
      EXPECT_TRUE(writer.Result.Admitted);
      EXPECT_TRUE(writer.Result.Waited);
      EXPECT_GE(writer.Result.WaitTime, milliseconds(50));
    }
    EXPECT_EQ(wait.GetWaiting(), 0UL);
    EXPECT_EQ(wait.GetWaited(), 1UL);
    EXPECT_EQ(wait.GetTimedOut(), 0UL);
  });
}

/* No room within the bound: refused once it passes, with nothing held. */
FIXTURE(RefusedAfterTheBound) {
  WithRunner([](TFramePoolMngr *mngr, TRunner *runner) {
    TAdmissionWait wait(milliseconds(100));
    auto filler = make_unique<TUpdate::TWriteAdmission>();
    EXPECT_TRUE(filler->TryAcquire(340UL));
    const size_t admitted = TUpdate::GetEntryPool().GetNumBlocksAdmitted();
    {
      TWriter writer(mngr, runner, wait, 40UL);
      writer.Join();
      EXPECT_FALSE(writer.Result.Admitted);
      EXPECT_TRUE(writer.Result.Waited);
      EXPECT_GE(writer.Result.WaitTime, milliseconds(100));
      EXPECT_LT(writer.Result.WaitTime, milliseconds(5000));
      EXPECT_EQ(TUpdate::GetEntryPool().GetNumBlocksAdmitted(), admitted);
      EXPECT_TRUE(writer.Admission.WasEntryPoolRefused());
    }
    EXPECT_EQ(wait.GetTimedOut(), 1UL);
    filler.reset();
  });
}

/* First come, first served: a small write that would fit doesn't pass a large one waiting
   ahead of it. */
FIXTURE(FirstComeFirstServed) {
  WithRunner([](TFramePoolMngr *mngr, TRunner *runner) {
    TAdmissionWait wait(milliseconds(10000));
    atomic<size_t> seq {0UL};
    auto filler = make_unique<TUpdate::TWriteAdmission>();
    EXPECT_TRUE(filler->TryAcquire(340UL));
    {
      TWriter big(mngr, runner, wait, 300UL, &seq);
      WaitUntil([&] { return wait.GetWaiting() == 1UL; });
      TWriter small(mngr, runner, wait, 5UL, &seq);
      WaitUntil([&] { return wait.GetWaiting() == 2UL; });
      EXPECT_EQ(wait.GetWaiting(), 2UL);
      /* Swap the filler for a copy claim of 200 entries: room for the small write, not for
         the big one. */
      TUpdate::TCopyClaim claim;
      EXPECT_TRUE(claim.TryAcquire(0UL, 200UL));
      filler.reset();
      this_thread::sleep_for(milliseconds(100));
      EXPECT_EQ(wait.GetWaiting(), 2UL);
      EXPECT_FALSE(small.Done.load());
      claim.Release();
      big.Join();
      small.Join();
      EXPECT_TRUE(big.Result.Admitted);
      EXPECT_TRUE(small.Result.Admitted);
      EXPECT_EQ(big.Order, 1UL);
      EXPECT_EQ(small.Order, 2UL);
    }
    EXPECT_EQ(wait.GetWaited(), 2UL);
    EXPECT_EQ(wait.GetTimedOut(), 0UL);
  });
}

/* Close refuses every waiter at once (a graceful stop must not wait on them), and a write
   after it tries once without waiting. */
FIXTURE(CloseRefusesWaiters) {
  WithRunner([](TFramePoolMngr *mngr, TRunner *runner) {
    TAdmissionWait wait(milliseconds(60000));
    auto filler = make_unique<TUpdate::TWriteAdmission>();
    EXPECT_TRUE(filler->TryAcquire(340UL));
    {
      TWriter first(mngr, runner, wait, 40UL), second(mngr, runner, wait, 40UL);
      WaitUntil([&] { return wait.GetWaiting() == 2UL; });
      EXPECT_EQ(wait.GetWaiting(), 2UL);
      const auto closed = steady_clock::now();
      wait.Close();
      first.Join();
      second.Join();
      EXPECT_LT(steady_clock::now() - closed, seconds(2));
      EXPECT_FALSE(first.Result.Admitted);
      EXPECT_FALSE(second.Result.Admitted);
      EXPECT_EQ(wait.GetClosedOut(), 2UL);
      TWriter late(mngr, runner, wait, 40UL);
      late.Join();
      EXPECT_FALSE(late.Result.Admitted);
      EXPECT_FALSE(late.Result.Waited);
    }
    filler.reset();
    EXPECT_EQ(TUpdate::GetEntryPool().GetNumBlocksAdmitted(), 0UL);
    EXPECT_EQ(TUpdate::GetUpdatePool().GetNumBlocksAdmitted(), 0UL);
  });
}

/* Many writers through a pool that keeps filling up: each holds its admission until the test
   sees it done and drops it, so the rest queue. Every one is admitted, and nothing is left
   promised. */
FIXTURE(ManyWritersDrain) {
  WithRunner([](TFramePoolMngr *mngr, TRunner *runner) {
    TAdmissionWait wait(milliseconds(10000));
    vector<unique_ptr<TWriter>> writers;
    for (size_t i = 0; i < 24UL; ++i) {
      writers.push_back(make_unique<TWriter>(mngr, runner, wait, 100UL));
    }
    size_t admitted = 0UL, left = writers.size();
    const auto give_up = steady_clock::now() + seconds(30);
    while (left && steady_clock::now() < give_up) {
      for (auto &writer : writers) {
        if (writer && writer->Done.load()) {
          writer->Join();
          admitted += writer->Result.Admitted;
          writer.reset();
          --left;
        }
      }
      this_thread::sleep_for(milliseconds(1));
    }
    EXPECT_EQ(admitted, 24UL);
    EXPECT_EQ(wait.GetTimedOut(), 0UL);
    EXPECT_GT(wait.GetWaited(), 0UL);
    EXPECT_EQ(TUpdate::GetEntryPool().GetNumBlocksAdmitted(), 0UL);
  });
}
