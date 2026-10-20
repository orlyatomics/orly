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

/* A sort file is only ever read back through the block cache, so writing it must not fill the
   page cache: every page it put there was a page no reader would ask for, and getting a buffer
   for it could evict one that a reader would. Every block a sort file writes, its streams' and
   its collision blocks, should carry the manager's block-only instruction. */
FIXTURE(SortFilesSkipThePageCache) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    const size_t consol_thresh = 3UL;
    const size_t num_iter = 10000UL;
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    std::atomic<size_t> block_only_writes(0UL), page_cache_writes(0UL);
    TBlockCache *block_cache_ptr = nullptr;
    TMemoryDevice device(512, 512, 65536, true, true);
    TVolume volume(TVolume::TDesc{TVolume::TDesc::Striped, device.GetDesc(), TVolume::TDesc::Fast, 1UL, 1UL, 1024UL, 8UL, 0.85},
                   [&](TCacheInstr instr, const TOffset logical_start_offset, void *buf, size_t count) {
                     switch (instr) {
                       case TCacheInstr::CacheBlockOnly: {
                         ++block_only_writes;
                         for (size_t i = 0; i < count / TBlockCache::DataSize; ++i) {
                           block_cache_ptr->Replace((logical_start_offset / TBlockCache::DataSize) + i, reinterpret_cast<uint8_t *>(buf) + (i * TBlockCache::DataSize));
                         }
                         break;
                       }
                       case TCacheInstr::ClearBlockOnly:
                       case TCacheInstr::NoCache: {
                         break;
                       }
                       default: {
                         ++page_cache_writes;
                         break;
                       }
                     }
                   }, &scheduler);
    volume.AddDevice(&device, 0);
    TVolumeManager vol_man(&scheduler);
    vol_man.AddNewVolume(&volume);
    TPageCache page_cache(&vol_man, 1024, 1);
    TBlockCache block_cache(&vol_man, 1024, 1);
    block_cache_ptr = &block_cache;
    TEngine engine(&vol_man, &page_cache, &block_cache, nullptr, false);

    typedef TIndexManager<size_t, 1000, 100> TMyManager;
    /* scope for the manager, whose sort files free their blocks when it goes */ {
      TMyManager manager(HERE, 0UL, consol_thresh, TVolume::TDesc::Fast, &engine, true /* do cache */);
      for (size_t i = 0; i < num_iter; ++i) {
        manager.Emplace(num_iter - 1 - i);
      }
      size_t found = 0UL;
      size_t matched = 0UL;
      for (TMyManager::TCursor csr(&manager, 50); csr; ++csr) {
        matched += (*csr == found) ? 1UL : 0UL;
        ++found;
      }
      EXPECT_EQ(found, num_iter);
      EXPECT_EQ(matched, num_iter);
    }
    EXPECT_GT(block_only_writes.load(), 0UL);
    EXPECT_EQ(page_cache_writes.load(), 0UL);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}
