/* <orly/indy/disk/util/cache.test.cc>

   Unit test for <orly/indy/disk/util/cache.h>. */

#include <orly/indy/disk/util/cache.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <sched.h>

#include <base/mem_aligned_ptr.h>
#include <orly/indy/disk/sim/mem_engine.h>

#include <base/test/kit.h>

using namespace std;
using namespace std::chrono;
using namespace Base;
using namespace Orly::Indy::Disk;
using namespace Orly::Indy::Disk::Util;

FIXTURE(Typical) {
  const size_t cache_size = 4096;
  const size_t num_lru = 4UL;
  const size_t page_id = 4UL;
  const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
  TScheduler scheduler;
  scheduler.SetPolicy(scheduler_policy);
  Sim::TMemEngine mem_engine(&scheduler,
                             64 /* disk space: 64 MB */,
                             16,
                             4096 /* page cache slots: 1GB */,
                             1 /* num page lru */,
                             16 /* block cache slots: 1GB */,
                             1 /* num block lru */);
  TCache<4096> cache(mem_engine.GetVolMan(), cache_size, num_lru);
  TCache<4096>::TSlot *data_slot;
  TCache<4096>::TSlot *slot = cache.Get(page_id, data_slot);
  cache.Release(slot, page_id);
}

FIXTURE(SingleThreadPressureLRU) {
  const size_t cache_size = 4096;
  const size_t num_lru = 4UL;
  const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
  TScheduler scheduler;
  scheduler.SetPolicy(scheduler_policy);
  Sim::TMemEngine mem_engine(&scheduler,
                             64 /* disk space: 64 MB */,
                             16,
                             4096 /* page cache slots: 1GB */,
                             1 /* num page lru */,
                             16 /* block cache slots: 1GB */,
                             1 /* num block lru */);
  TCache<4096> cache(mem_engine.GetVolMan(), cache_size, num_lru);
  for (size_t i = 0; i < cache_size * 24; ++i ) {
    const size_t page_id = i * 3UL;
    TCache<4096>::TSlot *data_slot;
    TCache<4096>::TSlot *slot = cache.Get(page_id, data_slot);
    cache.Release(slot, page_id);
  }
}

/* With every page buffer referenced, caching one more page throws TOutOfCachePages, which a
   write-through insert skips, and leaves the cache usable: once a buffer is free again, the same
   page goes in. */
FIXTURE(ReplaceWhenFullThrowsOutOfCachePages) {
  const size_t cache_size = 16UL;
  const size_t num_lru = 1UL;
  const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
  TScheduler scheduler;
  scheduler.SetPolicy(scheduler_policy);
  Sim::TMemEngine mem_engine(&scheduler,
                             64 /* disk space: 64 MB */,
                             16,
                             4096 /* page cache slots: 1GB */,
                             1 /* num page lru */,
                             16 /* block cache slots: 1GB */,
                             1 /* num block lru */);
  TCache<4096> cache(mem_engine.GetVolMan(), cache_size, num_lru);
  vector<TCache<4096>::TSlot *> pinned;
  for (size_t page_id = 0; page_id < cache_size; ++page_id) {
    TCache<4096>::TSlot *data_slot;
    pinned.push_back(cache.Get(page_id, data_slot));
  }
  const size_t new_page_id = 100UL;
  Base::TMemAlignedPtr<char> buf = Base::MemAlignedAlloc<char>(4096, 4096);
  memset(buf.get(), 0x5A, 4096);
  bool threw = false;
  try {
    cache.Replace(new_page_id, buf.get());
  } catch (const TOutOfCachePages &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
  for (size_t page_id = 0; page_id < cache_size; ++page_id) {
    cache.Release(pinned[page_id], page_id);
  }
  cache.Replace(new_page_id, buf.get());
  EXPECT_TRUE(cache.AssertNoRefCount(new_page_id));
}

/* Retain takes a lock-free extra reference on a page the caller already holds, and the data-slot Release
   drops it without the lock. The page stays referenced until the last reference goes, and the last drop
   returns it to the LRU so it can be reclaimed. */
FIXTURE(RetainAndReleaseKeepCounts) {
  const size_t cache_size = 16UL;
  const size_t num_lru = 1UL;
  const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
  TScheduler scheduler;
  scheduler.SetPolicy(scheduler_policy);
  Sim::TMemEngine mem_engine(&scheduler, 64, 16, 4096, 1, 16, 1);
  TCache<4096> cache(mem_engine.GetVolMan(), cache_size, num_lru);
  const size_t page_id = 7UL;
  TCache<4096>::TSlot *data_slot;
  TCache<4096>::TSlot *const main_slot = cache.Get(page_id, data_slot);
  EXPECT_TRUE(cache.Retain(main_slot, data_slot, page_id) == main_slot);
  EXPECT_TRUE(cache.Retain(main_slot, data_slot, page_id) == main_slot);
  /* Three references. Drop two through the fast path; one is still held, so the probe (which takes a
     reference of its own) must see a second holder. */
  cache.Release(main_slot, data_slot, page_id);
  cache.Release(main_slot, data_slot, page_id);
  EXPECT_FALSE(cache.AssertNoRefCount(page_id));
  /* Drop the last reference. This is the 1 -> 0 transition, so it takes the locked path. */
  cache.Release(main_slot, data_slot, page_id);
  EXPECT_TRUE(cache.AssertNoRefCount(page_id));
  /* Back in the LRU: filling the whole cache with other pages reclaims it without running out. */
  for (size_t i = 0; i < cache_size * 4; ++i) {
    TCache<4096>::TSlot *other_data;
    TCache<4096>::TSlot *other = cache.Get(100UL + i, other_data);
    cache.Release(other, other_data, 100UL + i);
  }
}

/* Threads share a page: one set takes and drops extra references with Retain and the fast Release, another
   set takes and drops whole references with Get and Release, so the lock-free and locked paths overlap on
   one slot. The counts must balance. */
FIXTURE(RetainAndGetRaceOnOneSlot) {
  const size_t cache_size = 64UL;
  const size_t num_lru = 2UL;
  const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
  TScheduler scheduler;
  scheduler.SetPolicy(scheduler_policy);
  Sim::TMemEngine mem_engine(&scheduler, 64, 16, 4096, 1, 16, 1);
  TCache<4096> cache(mem_engine.GetVolMan(), cache_size, num_lru);
  const size_t page_id = 11UL;
  const size_t iters = 20000UL;
  vector<thread> threads;
  for (size_t t = 0; t < 4; ++t) {
    threads.emplace_back([&cache, page_id, iters] {
      for (size_t i = 0; i < iters; ++i) {
        TCache<4096>::TSlot *data_slot;
        TCache<4096>::TSlot *const main_slot = cache.Get(page_id, data_slot);
        cache.Retain(main_slot, data_slot, page_id);
        cache.Release(main_slot, data_slot, page_id);
        cache.Release(main_slot, data_slot, page_id);
      }
    });
  }
  for (size_t t = 0; t < 2; ++t) {
    threads.emplace_back([&cache, page_id, iters] {
      for (size_t i = 0; i < iters; ++i) {
        TCache<4096>::TSlot *data_slot;
        TCache<4096>::TSlot *const main_slot = cache.Get(page_id, data_slot);
        cache.Release(main_slot, page_id);
      }
    });
  }
  for (thread &t : threads) {
    t.join();
  }
  EXPECT_TRUE(cache.AssertNoRefCount(page_id));
}

/* The pages that share the main slot of `base_page`, found by probing. Each probe pins and drops the page, so
   nothing is left held. Returns the number of slots (the distance between colliding page ids). */
static size_t FindSlotStride(TCache<4096> &cache, size_t base_page) {
  TCache<4096>::TSlot *base_data;
  TCache<4096>::TSlot *const base_main = cache.Get(base_page, base_data);
  size_t stride = 0UL;
  for (size_t d = 1UL; d < 100000UL && stride == 0UL; ++d) {
    TCache<4096>::TSlot *data;
    TCache<4096>::TSlot *const main = cache.Get(base_page + d, data);
    if (main == base_main) {
      stride = d;
    }
    cache.Release(main, data, base_page + d);
  }
  cache.Release(base_main, base_data, base_page);
  return stride;
}

/* As RetainAndGetRaceOnOneSlot, but on a page that is not the head of its chain: two pages share a main slot,
   the first stays pinned, and the race runs on the second, so the data slot is not the main slot. */
FIXTURE(RetainAndGetRaceOnChainSlot) {
  const size_t cache_size = 64UL;
  const size_t num_lru = 2UL;
  const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
  TScheduler scheduler;
  scheduler.SetPolicy(scheduler_policy);
  Sim::TMemEngine mem_engine(&scheduler, 64, 16, 4096, 1, 16, 1);
  TCache<4096> cache(mem_engine.GetVolMan(), cache_size, num_lru);
  const size_t head_page = 5UL;
  const size_t stride = FindSlotStride(cache, head_page);
  if (EXPECT_TRUE(stride != 0UL)) {
    const size_t chain_page = head_page + stride;
    TCache<4096>::TSlot *head_data;
    TCache<4096>::TSlot *const head_main = cache.Get(head_page, head_data);
    TCache<4096>::TSlot *chain_data;
    TCache<4096>::TSlot *const chain_main = cache.Get(chain_page, chain_data);
    EXPECT_TRUE(chain_main == head_main);
    EXPECT_TRUE(chain_data != chain_main);
    const size_t iters = 20000UL;
    vector<thread> threads;
    for (size_t t = 0; t < 4; ++t) {
      threads.emplace_back([&cache, chain_page, head_main, iters] {
        for (size_t i = 0; i < iters; ++i) {
          TCache<4096>::TSlot *data_slot;
          TCache<4096>::TSlot *const main_slot = cache.Get(chain_page, data_slot);
          if (main_slot != head_main || data_slot == main_slot) {
            abort();
          }
          cache.Retain(main_slot, data_slot, chain_page);
          cache.Release(main_slot, data_slot, chain_page);
          cache.Release(main_slot, data_slot, chain_page);
        }
      });
    }
    for (size_t t = 0; t < 2; ++t) {
      threads.emplace_back([&cache, chain_page, iters] {
        for (size_t i = 0; i < iters; ++i) {
          TCache<4096>::TSlot *data_slot;
          TCache<4096>::TSlot *const main_slot = cache.Get(chain_page, data_slot);
          cache.Release(main_slot, chain_page);
        }
      });
    }
    for (thread &t : threads) {
      t.join();
    }
    cache.Release(chain_main, chain_data, chain_page);
    cache.Release(head_main, head_data, head_page);
    EXPECT_TRUE(cache.AssertNoRefCount(chain_page));
    EXPECT_TRUE(cache.AssertNoRefCount(head_page));
  }
}

/* A pinned page keeps its contents while other threads cycle many distinct pages through a small cache, so that
   buffer reclaim (NewPageBuf and TryRemoveSlot) runs at the same time as lock-free Retain and Release. Some of
   the cycled pages share a main slot with a pinned page, so reclaim also edits chains next to it. */
FIXTURE(RetainWhileReclaimingPages) {
  const size_t cache_size = 32UL;
  const size_t num_lru = 2UL;
  const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
  TScheduler scheduler;
  scheduler.SetPolicy(scheduler_policy);
  Sim::TMemEngine mem_engine(&scheduler, 64, 16, 4096, 1, 16, 1);
  TCache<4096> cache(mem_engine.GetVolMan(), cache_size, num_lru);
  const size_t num_pinned = 4UL;
  const size_t first_page = 1000UL;
  const size_t stride = FindSlotStride(cache, first_page);
  if (EXPECT_TRUE(stride != 0UL)) {
    /* Write known contents into each page, then pin it before any reclaim starts. */
    TCache<4096>::TSlot *mains[4];
    TCache<4096>::TSlot *datas[4];
    size_t pages[4];
    char pattern[4][4096];
    for (size_t p = 0; p < num_pinned; ++p) {
      pages[p] = first_page + p;
      memset(pattern[p], static_cast<int>(0x41 + p), sizeof(pattern[p]));
      cache.Replace(pages[p], pattern[p]);
      mains[p] = cache.Get(pages[p], datas[p]);
    }
    atomic<bool> stop(false);
    atomic<size_t> bad(0UL);
    const size_t iters = 20000UL;
    vector<thread> holders;
    for (size_t t = 0; t < 4; ++t) {
      holders.emplace_back([&, t] {
        const size_t p = t % num_pinned;
        for (size_t i = 0; i < iters; ++i) {
          cache.Retain(mains[p], datas[p], pages[p]);
          if (memcmp(datas[p]->KnownGetData(&cache), pattern[p], sizeof(pattern[p])) != 0) {
            ++bad;
          }
          cache.Release(mains[p], datas[p], pages[p]);
        }
      });
    }
    vector<thread> cyclers;
    for (size_t t = 0; t < 2; ++t) {
      cyclers.emplace_back([&, t] {
        for (size_t i = 0; !stop.load(); ++i) {
          /* Alternate between pages that collide with a pinned page and pages that don't. */
          const size_t page_id = (i % 2 == 0) ? first_page + stride * (2UL + (i + t * 7UL) % 50UL) : 50000UL + t * 100000UL + i;
          try {
            TCache<4096>::TSlot *data_slot;
            TCache<4096>::TSlot *const main_slot = cache.Get(page_id, data_slot);
            cache.Release(main_slot, data_slot, page_id);
          } catch (const TOutOfCachePages &) {
          }
        }
      });
    }
    for (thread &t : holders) {
      t.join();
    }
    stop = true;
    for (thread &t : cyclers) {
      t.join();
    }
    EXPECT_EQ(bad.load(), 0UL);
    for (size_t p = 0; p < num_pinned; ++p) {
      EXPECT_TRUE(memcmp(datas[p]->KnownGetData(&cache), pattern[p], sizeof(pattern[p])) == 0);
      cache.Release(mains[p], datas[p], pages[p]);
      EXPECT_TRUE(cache.AssertNoRefCount(pages[p]));
    }
  }
}
