/* <orly/indy/disk/util/index_manager.test.cc>

   Unit test for <orly/indy/disk/util/index_manager.h>.

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

#include <orly/indy/disk/util/index_manager.h>

#include <orly/indy/disk/sim/mem_engine.h>
#include <orly/indy/fiber/fiber_test_runner.h>

#include <base/test/kit.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Orly::Indy;
using namespace Orly::Indy::Disk;
using namespace Orly::Indy::Disk::Util;
using namespace Orly::Indy::Fiber;
using namespace Orly::Indy::Util;

static const size_t BlockSize = 4096UL * 16;

TBufBlock::TPool TBufBlock::Pool(BlockSize, 200);

FIXTURE(Typical) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    const size_t consol_thresh = 20UL;
    const size_t num_iter = 10000L;
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256 MB */,
                               256,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);

    typedef TIndexManager<size_t, 1000, 100> TMyManager;
    TMyManager manager(HERE, 0UL, consol_thresh, TVolume::TDesc::Fast, mem_engine.GetEngine(), true /* do cache */);
    for (size_t i = 0; i < num_iter; ++i) {
      manager.Emplace(num_iter - 1 - i);
    }
    size_t found = 0U;
    size_t matched = 0UL;
    for (TMyManager::TCursor csr(&manager, 50); csr; ++csr) {
      matched += (*csr == found) ? 1UL : 0UL;
      ++found;
    }
    EXPECT_EQ(found, num_iter);
    EXPECT_EQ(num_iter, found);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #1025: a sort that is told to poll still sorts (with many duplicates), calls the poll, and a poll that throws
   abandons it. */
FIXTURE(PollingSort) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    const size_t num_iter = 100000UL;
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    typedef TIndexManager<size_t, 200000, 100> TMyManager;
    const auto fill = [&](TMyManager &manager) {
      uint64_t x = 88172645463325252ULL;
      for (size_t i = 0; i < num_iter; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        manager.Emplace(x % 5000UL);  // many equal keys
      }
    };
    /* polls, never throws: the order must come out right */ {
      TMyManager manager(HERE, 0UL, 20UL, TVolume::TDesc::Fast, mem_engine.GetEngine(), true);
      size_t polls = 0UL;
      manager.SetCancelPoll([&polls] { ++polls; });
      fill(manager);
      size_t found = 0UL;
      size_t prev = 0UL;
      bool ordered = true;
      for (TMyManager::TCursor csr(&manager, 50); csr; ++csr, ++found) {
        ordered = ordered && *csr >= prev;
        prev = *csr;
      }
      EXPECT_EQ(found, num_iter);
      EXPECT_TRUE(ordered);
      EXPECT_TRUE(polls > 0UL);
    }
    /* a poll that throws abandons the sort */ {
      TMyManager manager(HERE, 0UL, 20UL, TVolume::TDesc::Fast, mem_engine.GetEngine(), true);
      manager.SetCancelPoll([] { throw std::runtime_error("cancelled"); });
      bool threw = false;
      try {
        fill(manager);
        TMyManager::TCursor csr(&manager, 50);
      } catch (const std::runtime_error &) {
        threw = true;
      }
      EXPECT_TRUE(threw);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(ManySortFiles) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    const size_t consol_thresh = 20UL;
    const size_t num_iter = 1000000L;
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256 MB */,
                               256,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);

    typedef TIndexManager<size_t, 100, 100> TMyManager;
    TMyManager manager(HERE, 0UL, consol_thresh, TVolume::TDesc::Fast, mem_engine.GetEngine(), true /* do cache */);
    for (size_t i = 0; i < num_iter; ++i) {
      manager.Emplace(num_iter - 1 - i);
    }
    size_t found = 0U;
    size_t matched = 0UL;
    for (TMyManager::TCursor csr(&manager, 50); csr; ++csr) {
      matched += (*csr == found) ? 1UL : 0UL;
      ++found;
    }
    EXPECT_EQ(found, num_iter);
    EXPECT_EQ(num_iter, found);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}
