/* <base/thread_local_sigma_calc.test.cc>

   Unit test for <base/thread_local_sigma_calc.h>.

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

#include <base/thread_local_sigma_calc.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Base;

/* Single-thread behaviour matches a plain TSigmaCalc. */
FIXTURE(SingleThread) {
  TThreadLocalSigmaCalc calc;
  double min, max, mean, sigma;
  EXPECT_EQ(calc.Report(min, max, mean, sigma), 0u);

  constexpr int vals[] = { 4, 2, 5, 8, 6 };
  for (int v : vals) {
    calc.Push(v);
  }
  if (EXPECT_EQ(calc.Report(min, max, mean, sigma), 5u)) {
    EXPECT_EQ(min, 2.0);
    EXPECT_EQ(max, 8.0);
    EXPECT_EQ(mean, 5.0);
    EXPECT_EQ(sigma, sqrt(5.0));
  }

  calc.Reset();
  EXPECT_EQ(calc.Report(min, max, mean, sigma), 0u);
}

/* Pushes from many threads fold into one exact aggregate. Each of N threads
   pushes the same value V exactly M times, so count == N*M, min == max == mean
   == V, and sigma == 0. */
FIXTURE(ManyThreads) {
  constexpr size_t thread_count = 8;
  constexpr size_t per_thread = 50000;
  constexpr double value = 7.0;

  TThreadLocalSigmaCalc calc;
  vector<thread> threads;
  threads.reserve(thread_count);
  for (size_t t = 0; t < thread_count; ++t) {
    threads.emplace_back([&calc]() {
      for (size_t i = 0; i < per_thread; ++i) {
        calc.Push(value);
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }

  double min, max, mean, sigma;
  if (EXPECT_EQ(calc.Report(min, max, mean, sigma), thread_count * per_thread)) {
    EXPECT_EQ(min, value);
    EXPECT_EQ(max, value);
    EXPECT_TRUE(fabs(mean - value) < 1e-9);
    EXPECT_TRUE(fabs(sigma) < 1e-9);
  }
}

/* Concurrent producers + a concurrent reporter (Report/Reset) must not race and
   must not lose count. Each producer pushes 1.0 a fixed number of times; the
   reporter periodically folds-and-resets, accumulating the count it observes.
   The observed total plus whatever remains must equal what was pushed. */
FIXTURE(ConcurrentReport) {
  constexpr size_t thread_count = 6;
  constexpr size_t per_thread = 40000;

  TThreadLocalSigmaCalc calc;
  size_t drained = 0;
  atomic<bool> done = false;

  vector<thread> threads;
  for (size_t t = 0; t < thread_count; ++t) {
    threads.emplace_back([&calc]() {
      for (size_t i = 0; i < per_thread; ++i) {
        calc.Push(1.0);
      }
    });
  }
  thread reporter([&]() {
    double mn, mx, me, sg;
    while (!done) {
      /* Atomic drain: no value pushed concurrently may be lost. */
      drained += calc.Drain(mn, mx, me, sg);
    }
  });
  for (auto &th : threads) {
    th.join();
  }
  done = true;
  reporter.join();

  double mn, mx, me, sg;
  drained += calc.Drain(mn, mx, me, sg);
  EXPECT_EQ(drained, thread_count * per_thread);
}

/* A thread's stats survive that thread's exit (its calculator is folded into the
   retired accumulator at exit, not lost) -- owner outlives the thread. */
FIXTURE(SurvivesThreadExit) {
  TThreadLocalSigmaCalc calc;
  {
    thread th([&calc]() {
      calc.Push(1.0);
      calc.Push(3.0);
    });
    th.join();
  }
  double min, max, mean, sigma;
  if (EXPECT_EQ(calc.Report(min, max, mean, sigma), 2u)) {
    EXPECT_EQ(min, 1.0);
    EXPECT_EQ(max, 3.0);
    EXPECT_EQ(mean, 2.0);
  }
}

/* Regression for the teardown bug: the owner is destroyed BEFORE a thread that
   pushed into it runs its thread_local registry destructor. The registry must
   not touch the dead owner; the shared control block keeps its mutex alive so
   the registry can fold/remove its entry without throwing from ~TRegistry. The
   assertion is simply that the process does not terminate. */
FIXTURE(OwnerDestroyedWhileThreadAlive) {
  mutex m;
  condition_variable cv;
  bool pushed = false;
  bool may_exit = false;

  auto calc = make_unique<TThreadLocalSigmaCalc>();
  TThreadLocalSigmaCalc *raw = calc.get();

  thread th([&]() {
    raw->Push(5.0);
    {
      lock_guard<mutex> l(m);
      pushed = true;
    }
    cv.notify_all();
    unique_lock<mutex> l(m);
    cv.wait(l, [&]() { return may_exit; });
    /* Thread now returns and exits; its thread_local TRegistry destructor runs
       and must fold into the (now owner-less) block without locking a dead
       mutex. */
  });

  {
    unique_lock<mutex> l(m);
    cv.wait(l, [&]() { return pushed; });
  }
  /* Destroy the owner while the producing thread is still alive and still holds
     a registry reference to the block. */
  calc.reset();
  {
    lock_guard<mutex> l(m);
    may_exit = true;
  }
  cv.notify_all();
  th.join();

  EXPECT_TRUE(true);  // reached here without std::terminate
}

/* The exact shape of the original CI failure: a local (non-static) instance is
   pushed into by THIS (main/test) thread, then destroyed. The test thread's
   registry keeps a stale reference, exercised when the registry is destroyed at
   process exit. With the shared-block design the block outlives the registry and
   teardown is clean -- the gate is that the test process exits without
   terminate. */
FIXTURE(LocalInstanceOnPushingThread) {
  {
    TThreadLocalSigmaCalc calc;
    calc.Push(1.0);
    calc.Push(2.0);
    double mn, mx, me, sg;
    EXPECT_EQ(calc.Report(mn, mx, me, sg), 2u);
  }  // calc destroyed here; this thread's registry still references its block
  EXPECT_TRUE(true);
}
