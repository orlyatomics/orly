/* <orly/server/edit_runner.test.cc>

   Unit test for <orly/server/edit_runner.h>.

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

#include <orly/server/edit_runner.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <base/thread_local_global_pool.h>
#include <base/test/kit.h>

using namespace std;
using namespace Base;
using namespace Orly::Indy::Fiber;
using namespace Orly::Server;

FIXTURE(ParseRoundTrips) {
  for (auto mode: {TEditRunner::Fast, TEditRunner::Slow, TEditRunner::Spill}) {
    EXPECT_TRUE(ParseEditRunner(ToString(mode)) == mode);
  }
  bool threw = false;
  try {
    (void)ParseEditRunner("medium");
  } catch (const invalid_argument &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
}

/* fast never moves; slow always does, to the least-loaded slow runner; spill moves only while
   that runner has fewer than the spill load. No slow runners: nothing moves. */
FIXTURE(PickFollowsTheMode) {
  TRunnerBalancer balancer(2UL);
  EXPECT_FALSE(PickEditRunner(TEditRunner::Fast, &balancer, 1UL));
  EXPECT_FALSE(PickEditRunner(TEditRunner::Slow, nullptr, 1UL));
  EXPECT_FALSE(PickEditRunner(TEditRunner::Spill, nullptr, 1UL));
  {
    auto a = PickEditRunner(TEditRunner::Slow, &balancer, 1UL);
    auto b = PickEditRunner(TEditRunner::Slow, &balancer, 1UL);
    auto c = PickEditRunner(TEditRunner::Slow, &balancer, 1UL);
    EXPECT_TRUE(a && b && c);
    EXPECT_NE(a.GetIndex(), b.GetIndex());
    EXPECT_EQ(balancer.GetLoad(0UL) + balancer.GetLoad(1UL), 3UL);
  }
  EXPECT_EQ(balancer.GetLoad(0UL) + balancer.GetLoad(1UL), 0UL);
  {
    auto a = PickEditRunner(TEditRunner::Spill, &balancer, 1UL);
    auto b = PickEditRunner(TEditRunner::Spill, &balancer, 1UL);
    EXPECT_TRUE(a && b);
    EXPECT_NE(a.GetIndex(), b.GetIndex());
    /* Both slow runners have one commit: the next stays where it is. */
    EXPECT_FALSE(PickEditRunner(TEditRunner::Spill, &balancer, 1UL));
    /* With room for two each, it goes. */
    auto c = PickEditRunner(TEditRunner::Spill, &balancer, 2UL);
    EXPECT_TRUE(c);
    a.Release();
    auto d = PickEditRunner(TEditRunner::Spill, &balancer, 1UL);
    EXPECT_TRUE(d);
  }
  EXPECT_EQ(balancer.GetLoad(0UL) + balancer.GetLoad(1UL), 0UL);
}

namespace {

  /* What a fiber saw while it ran a TEditRunnerSwitch. */
  struct TSeen {
    TRunner *Before = nullptr;
    TRunner *During = nullptr;
    TRunner *After = nullptr;
    size_t LoadDuring = 0UL;
    bool Moved = false;
    atomic<bool> Done{false};
  };

  class TEditRunnable final
      : public TRunnable {
    NO_COPY(TEditRunnable);
    public:

    TEditRunnable(TRunner *runner, TEditRunner mode, TRunnerBalancer *balancer,
        const vector<unique_ptr<TRunner>> &slow, size_t spill_load, TSeen &seen)
        : Mode(mode), Balancer(balancer), Slow(slow), SpillLoad(spill_load), Seen(seen) {
      Frame = TFrame::LocalFramePool->Alloc();
      Frame->Latch(runner, this, static_cast<TRunnable::TFunc>(&TEditRunnable::RunMe));
    }

    ~TEditRunnable() {
      TFrame::LocalFramePool->Free(Frame);
    }

    private:

    void RunMe() {
      Seen.Before = TRunner::LocalRunner;
      /* RAII scope */ {
        TEditRunnerSwitch edit_switch(Mode, Balancer, Slow, SpillLoad);
        Seen.Moved = edit_switch.Moved();
        Seen.During = TRunner::LocalRunner;
        Seen.LoadDuring = Balancer ? Balancer->GetLoad(0UL) : 0UL;
      }
      Seen.After = TRunner::LocalRunner;
      Seen.Done = true;
    }

    TFrame *Frame;
    const TEditRunner Mode;
    TRunnerBalancer *const Balancer;
    const vector<unique_ptr<TRunner>> &Slow;
    const size_t SpillLoad;
    TSeen &Seen;

  };  // TEditRunnable

  /* Runs one TEditRunnable on a "fast" runner, with one "slow" runner beside it whose balancer
     already carries 'preload'. */
  void RunOne(TEditRunner mode, size_t preload, size_t spill_load, TSeen &seen, TRunner *&fast_out, TRunner *&slow_out) {
    TRunner::TRunnerCons runner_cons(2UL);
    TRunner fast(runner_cons);
    vector<unique_ptr<TRunner>> slow;
    slow.emplace_back(make_unique<TRunner>(runner_cons));
    TRunnerBalancer balancer(1UL);
    auto held = balancer.Charge(0UL, preload);
    auto *manager = new TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>(2UL, 8UL * 1024UL * 1024UL, &fast);
    TFrame::LocalFramePool = new TThreadLocalGlobalPoolManager<TFrame, size_t, TRunner *>::TThreadLocalPool(manager);
    thread t1([&fast] { fast.Run(); });
    thread t2([&slow] { slow[0]->Run(); });
    {
      TEditRunnable runnable(&fast, mode, &balancer, slow, spill_load, seen);
      for (size_t i = 0UL; i < 2000UL && !seen.Done; ++i) {
        this_thread::sleep_for(chrono::milliseconds(5));
      }
      fast.ShutDown();
      slow[0]->ShutDown();
      t1.join();
      t2.join();
    }
    delete TFrame::LocalFramePool;
    TFrame::LocalFramePool = nullptr;
    delete manager;
    /* The lease was given back once the statement switched home. */
    EXPECT_EQ(balancer.GetLoad(0UL), preload);
    fast_out = &fast == seen.Before ? seen.Before : nullptr;
    slow_out = slow[0].get() == seen.During ? seen.During : nullptr;
  }

}  // namespace

/* fast: the commit runs where the statement ran. */
FIXTURE(FastStays) {
  TSeen seen;
  TRunner *fast = nullptr, *slow = nullptr;
  RunOne(TEditRunner::Fast, 0UL, 1UL, seen, fast, slow);
  EXPECT_TRUE(seen.Done);
  EXPECT_FALSE(seen.Moved);
  EXPECT_TRUE(fast != nullptr);
  EXPECT_TRUE(seen.During == seen.Before);
  EXPECT_TRUE(seen.After == seen.Before);
  EXPECT_EQ(seen.LoadDuring, 0UL);
}

/* slow: the commit runs on the slow runner, counted in its load, and the statement comes back. */
FIXTURE(SlowMovesAndComesBack) {
  TSeen seen;
  TRunner *fast = nullptr, *slow = nullptr;
  RunOne(TEditRunner::Slow, 0UL, 1UL, seen, fast, slow);
  EXPECT_TRUE(seen.Done);
  EXPECT_TRUE(seen.Moved);
  EXPECT_TRUE(fast != nullptr);
  EXPECT_TRUE(slow != nullptr);
  EXPECT_TRUE(seen.After == seen.Before);
  EXPECT_EQ(seen.LoadDuring, 1UL);
}

/* spill: moves while the slow runner has room, and stays once it is at the spill load. */
FIXTURE(SpillMovesOnlyWithRoom) {
  {
    TSeen seen;
    TRunner *fast = nullptr, *slow = nullptr;
    RunOne(TEditRunner::Spill, 0UL, 1UL, seen, fast, slow);
    EXPECT_TRUE(seen.Moved);
    EXPECT_TRUE(slow != nullptr);
    EXPECT_TRUE(seen.After == seen.Before);
  }
  {
    TSeen seen;
    TRunner *fast = nullptr, *slow = nullptr;
    RunOne(TEditRunner::Spill, 1UL, 1UL, seen, fast, slow);
    EXPECT_TRUE(seen.Done);
    EXPECT_FALSE(seen.Moved);
    EXPECT_TRUE(seen.During == seen.Before);
    EXPECT_EQ(seen.LoadDuring, 1UL);
  }
}
