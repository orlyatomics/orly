/* <orly/indy/disk/present_walk_file.test.cc>

   Unit test for <orly/indy/disk/present_walk_file.h>.

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

#include <orly/indy/disk/present_walk_file.h>

#include <memory>
#include <random>
#include <tuple>
#include <vector>

#include <base/scheduler.h>
#include <orly/indy/disk/data_file.h>
#include <orly/indy/disk/disk_test.h>
#include <orly/indy/disk/merge_data_file.h>
#include <orly/indy/disk/read_file.h>
#include <orly/indy/disk/sim/mem_engine.h>
#include <orly/indy/fiber/fiber_test_runner.h>
#include <orly/sabot/to_native.h>

#include <base/test/kit.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Orly;
using namespace Orly::Atom;
using namespace Orly::Indy;
using namespace Orly::Indy::Disk;
using namespace Orly::Indy::Disk::Util;
using namespace Orly::Indy::Util;

static const size_t BlockSize = Disk::Util::PhysicalBlockSize;

Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::Pool(sizeof(TRepo::TMapping), "Repo Mapping");
Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::TEntry::Pool(sizeof(TRepo::TMapping::TEntry), "Repo Mapping Entry");
Orly::Indy::Util::TPool L0::TManager::TRepo::TDataLayer::Pool(sizeof(TMemoryLayer), "Data Layer");

Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update", 1048578UL);
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry", 1048578UL);
Disk::TBufBlock::TPool Disk::TBufBlock::Pool(BlockSize, 2000UL);

class TRAIITest {
  NO_COPY(TRAIITest);
  public:

  using TLocalReadFileCache = Orly::Indy::Disk::TLocalReadFileCache<Orly::Indy::Disk::Util::LogicalPageSize,
    Orly::Indy::Disk::Util::LogicalBlockSize,
    Orly::Indy::Disk::Util::PhysicalBlockSize,
    Orly::Indy::Disk::Util::CheckedPage, true>;

  TRAIITest() {
    assert(!TLocalReadFileCache::Cache);
    TLocalReadFileCache::Cache = new TLocalReadFileCache();
    assert(!Disk::TLocalWalkerCache::Cache);
    Disk::TLocalWalkerCache::Cache = new Disk::TLocalWalkerCache();
  }

  ~TRAIITest() {
    assert(TLocalReadFileCache::Cache);
    delete TLocalReadFileCache::Cache;
    TLocalReadFileCache::Cache = nullptr;
    assert(Disk::TLocalWalkerCache::Cache);
    delete Disk::TLocalWalkerCache::Cache;
    Disk::TLocalWalkerCache::Cache = nullptr;
  }
};

FIXTURE(Typical) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRAIITest required_thread_locals;
    const TScheduler::TPolicy scheduler_policy(10, 10, milliseconds(10));
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);

    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256MB */,
                               256,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);

    Base::TUuid data_file_id(TUuid::TimeAndMAC);
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    size_t data_gen_id = 1;
    TSequenceNumber seq_num = 0U;
    Base::TUuid int_idx(Base::TUuid::Twister);
    /* data file 1 */ {
      TMockMem mem_layer;
      for (int64_t i = 0; i < 11; i += 2) {
        Insert(mem_layer, ++seq_num, int_idx, static_cast<int64_t>(rand()),
                 i);
      }
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, data_file_id, data_gen_id, 20UL, 0U, RealTime);
    }
    /* exists single */ {
      TPresentWalkFileWrapper walker(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, TKey(make_tuple(0L), &arena, state_alloc));
      size_t found = 0U;
      for (int64_t i = 0L; walker; ++walker, i += 2) {
        EXPECT_EQ(TKey((*walker).Key, (*walker).KeyArena), TKey(make_tuple(i), &arena, state_alloc));
        ++found;
      }
      EXPECT_EQ(found, 1U);
    }
    /* no exist single */ {
      TPresentWalkFileWrapper walker(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, TKey(make_tuple(5L), &arena, state_alloc));
      size_t found = 0U;
      for (int64_t i = 5L; walker; ++walker, ++i) {
        ++found;
      }
      EXPECT_EQ(found, 0U);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
    GracefullShutdown();
  });
}

template <typename ...TArgs>
void CheckExists(TEngine *engine, const Base::TUuid &file_id, size_t gen_id, const Base::TUuid &index_id, const TArgs &...args) {
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  TPresentWalkFileWrapper walker(engine, file_id, gen_id, index_id, TKey(make_tuple(args...), &arena, state_alloc));
  if (EXPECT_TRUE(walker)) {
    EXPECT_EQ(TKey((*walker).Key, (*walker).KeyArena), TKey(make_tuple(args...), &arena, state_alloc));
    ++walker;
    EXPECT_FALSE(walker);
  }
}

template <typename ...TArgs>
size_t CheckExistsFree(TEngine *engine, const Base::TUuid &file_id, size_t gen_id, const Base::TUuid &index_id, const std::function<void (const TKey &)> &cb, const TArgs &...args) {
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  size_t matched = 0UL;
  for (TPresentWalkFileWrapper walker(engine, file_id, gen_id, index_id, TKey(make_tuple(args...), &arena, state_alloc)); walker; ++walker, ++matched) {
    cb(TKey((*walker).Key, (*walker).KeyArena));
  }
  return matched;
}

template <typename ...TArgs>
void CheckNotExists(TEngine *engine, const Base::TUuid &file_id, size_t gen_id, const Base::TUuid &index_id, const TArgs &...args) {
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  TPresentWalkFileWrapper walker(engine, file_id, gen_id, index_id, TKey(make_tuple(args...), &arena, state_alloc));
  EXPECT_FALSE(walker);
}

FIXTURE(MultiIndex) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRAIITest required_thread_locals;
    const TScheduler::TPolicy scheduler_policy(10, 10, milliseconds(10));
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);

    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256MB */,
                               256,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);

    Base::TUuid data_file_id(TUuid::TimeAndMAC);
    TSuprena arena;
    size_t data_gen_id = 1;
    TSequenceNumber seq_num = 0U;
    Base::TUuid int_idx(Base::TUuid::Twister);
    Base::TUuid int_int_idx(Base::TUuid::Twister);
    /* data file 1 */ {
      TMockMem mem_layer;
      /* insert <[int64_t]> <- int64_t */ {
        Insert(mem_layer, ++seq_num, int_idx, 2L,
               1L);
        Insert(mem_layer, ++seq_num, int_idx, 4L,
               2L);
        Insert(mem_layer, ++seq_num, int_idx, 6L,
               7L);
        Insert(mem_layer, ++seq_num, int_idx, 8L,
               91L);
        Insert(mem_layer, ++seq_num, int_idx, 2L,
               92L);
      }
      /* insert <[int64_t, int64_t]> <- int64_t */ {
        Insert(mem_layer, ++seq_num, int_int_idx, 2L,
               1L, 2L);
        Insert(mem_layer, ++seq_num, int_int_idx, 4L,
               2L, 4L);
        Insert(mem_layer, ++seq_num, int_int_idx, 6L,
               7L, 14L);
        Insert(mem_layer, ++seq_num, int_int_idx, 8L,
               91L, 182L);
        Insert(mem_layer, ++seq_num, int_int_idx, 2L,
               92L, 184L);
      }
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, data_file_id, data_gen_id, 20UL, 0U, RealTime);
    }
    /* exists <[int64_t]> */ {
      CheckExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, 1L);
      CheckExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, 2L);
      CheckExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, 7L);
      CheckExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, 91L);
      CheckExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, 92L);
      std::vector<TKey> expected;
      TSuprena arena;
      expected.push_back(TKey(make_tuple(1L), &arena, state_alloc));
      expected.push_back(TKey(make_tuple(2L), &arena, state_alloc));
      expected.push_back(TKey(make_tuple(7L), &arena, state_alloc));
      expected.push_back(TKey(make_tuple(91L), &arena, state_alloc));
      expected.push_back(TKey(make_tuple(92L), &arena, state_alloc));
      size_t pos = 0UL;
      auto cb = [&expected, &pos](const TKey &key) {
        EXPECT_EQ(key, expected[pos]);
        ++pos;
      };
      EXPECT_EQ(CheckExistsFree(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, cb, Native::TFree<int64_t>()), 5UL);
    }
    /* exists <[int64_t, int64_t]> */ {
      CheckExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_int_idx, 1L, 2L);
      CheckExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_int_idx, 2L, 4L);
      CheckExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_int_idx, 7L, 14L);
      CheckExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_int_idx, 91L, 182L);
      CheckExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_int_idx, 92L, 184L);
      /* check range <[1L, Free<int64_t>]> */ {
        TSuprena arena;
        TKey expected(make_tuple(1L, 2L), &arena, state_alloc);
        auto cb = [&expected](const TKey &key) {
          EXPECT_EQ(key, expected);
        };
        EXPECT_EQ(CheckExistsFree(mem_engine.GetEngine(), data_file_id, data_gen_id, int_int_idx, cb, 1L, Native::TFree<int64_t>()), 1UL);
      }
    }
    /* no exist <[int64_t]> */ {
      CheckNotExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, 3L);
      CheckNotExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, 4L);
      CheckNotExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, 8L);
      CheckNotExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, 90L);
      CheckNotExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, 93L);
    }
    /* no exist <[int64_t]> */ {
      CheckNotExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_int_idx, 1L, 184L);
      CheckNotExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_int_idx, 2L, 182L);
      CheckNotExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_int_idx, 7L, 2L);
      CheckNotExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_int_idx, 91L, 4L);
      CheckNotExists(mem_engine.GetEngine(), data_file_id, data_gen_id, int_int_idx, 92L, 14L);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
    GracefullShutdown();
  });
}

FIXTURE(Range) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRAIITest required_thread_locals;
    const TScheduler::TPolicy scheduler_policy(10, 10, milliseconds(10));
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);

    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256MB */,
                               256,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);

    Base::TUuid data_file_id(TUuid::TimeAndMAC);
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    size_t data_gen_id = 1;
    TSequenceNumber seq_num = 0U;
    Base::TUuid int_idx(Base::TUuid::Twister);
    /* data file 1 */ {
      TMockMem mem_layer;
      for (int64_t i = 0; i < 11; i += 2) {
        Insert(mem_layer, ++seq_num, int_idx, static_cast<int64_t>(rand()),
                 i);
      }
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, data_file_id, data_gen_id, 20UL, 0U, RealTime);
    }
    /* full range using existing start, limit */ {
      TPresentWalkFileWrapper walker(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, TKey(make_tuple(0L), &arena, state_alloc), TKey(make_tuple(10L), &arena, state_alloc));
      size_t found = 0U;
      for (int64_t i = 0L; walker; ++walker, i += 2) {
        EXPECT_EQ(TKey((*walker).Key, (*walker).KeyArena), TKey(make_tuple(i), &arena, state_alloc));
        ++found;
      }
      EXPECT_EQ(found, 6U);
    }
    /* full range using existing start, non-existing limit */ {
      TPresentWalkFileWrapper walker(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, TKey(make_tuple(0L), &arena, state_alloc), TKey(make_tuple(11L), &arena, state_alloc));
      size_t found = 0U;
      for (int64_t i = 0L; walker; ++walker, i += 2) {
        EXPECT_EQ(TKey((*walker).Key, (*walker).KeyArena), TKey(make_tuple(i), &arena, state_alloc));
        ++found;
      }
      EXPECT_EQ(found, 6U);
    }
    /* partial range using existing start, limit */ {
      TPresentWalkFileWrapper walker(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, TKey(make_tuple(2L), &arena, state_alloc), TKey(make_tuple(8L), &arena, state_alloc));
      size_t found = 0U;
      for (int64_t i = 2L; walker; ++walker, i += 2) {
        EXPECT_EQ(TKey((*walker).Key, (*walker).KeyArena), TKey(make_tuple(i), &arena, state_alloc));
        ++found;
      }
      EXPECT_EQ(found, 4U);
    }
    /* partial range using existing start, non-existing limit */ {
      TPresentWalkFileWrapper walker(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, TKey(make_tuple(2L), &arena, state_alloc), TKey(make_tuple(9L), &arena, state_alloc));
      size_t found = 0U;
      for (int64_t i = 2L; walker; ++walker, i += 2) {
        EXPECT_EQ(TKey((*walker).Key, (*walker).KeyArena), TKey(make_tuple(i), &arena, state_alloc));
        ++found;
      }
      EXPECT_EQ(found, 4U);
    }
    /* full range using non-existing start, non-existing limit */ {
      TPresentWalkFileWrapper walker(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, TKey(make_tuple(-1L), &arena, state_alloc), TKey(make_tuple(11L), &arena, state_alloc));
      size_t found = 0U;
      for (int64_t i = 0L; walker; ++walker, i += 2) {
        EXPECT_EQ(TKey((*walker).Key, (*walker).KeyArena), TKey(make_tuple(i), &arena, state_alloc));
        ++found;
      }
      EXPECT_EQ(found, 6U);
    }
    /* partial range using non-existing start, non-existing limit */ {
      TPresentWalkFileWrapper walker(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, TKey(make_tuple(1L), &arena, state_alloc), TKey(make_tuple(9L), &arena, state_alloc));
      size_t found = 0U;
      for (int64_t i = 2L; walker; ++walker, i += 2) {
        EXPECT_EQ(TKey((*walker).Key, (*walker).KeyArena), TKey(make_tuple(i), &arena, state_alloc));
        ++found;
      }
      EXPECT_EQ(found, 4U);
    }
    /* empty range using non-existing start, non-existing limit */ {
      TPresentWalkFileWrapper walker(mem_engine.GetEngine(), data_file_id, data_gen_id, int_idx, TKey(make_tuple(11L), &arena, state_alloc), TKey(make_tuple(13L), &arena, state_alloc));
      EXPECT_FALSE(walker);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
    GracefullShutdown();
  });
}

/* The number of the first num_blocks blocks whose first page someone still references. */
static size_t CountHeldBlocks(TEngine *engine, size_t num_blocks) {
  size_t held = 0UL;
  for (size_t block_id = 0UL; block_id < num_blocks; ++block_id) {
    if (!engine->GetPageCache()->AssertNoRefCount(block_id * 16UL)) {
      ++held;
    }
  }
  return held;
}

/* #910: a walker must not keep pages pinned once its file's caches are cleared, whatever index it was for and however it
   comes back. */
FIXTURE(ClearWithWalkerOut) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRAIITest required_thread_locals;
    const TScheduler::TPolicy scheduler_policy(10, 10, milliseconds(10));
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);

    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256MB */,
                               256,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);
    TEngine *engine = mem_engine.GetEngine();

    Base::TUuid data_file_id(TUuid::TimeAndMAC);
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    size_t data_gen_id = 1;
    TSequenceNumber seq_num = 0U;
    Base::TUuid int_idx(Base::TUuid::Twister);
    /* data file */ {
      TMockMem mem_layer;
      for (int64_t i = 0; i < 11; i += 2) {
        Insert(mem_layer, ++seq_num, int_idx, static_cast<int64_t>(rand()), i);
      }
      TDataFile data_file(engine, TVolume::TDesc::Fast, &mem_layer, data_file_id, data_gen_id, 20UL, 0U, RealTime);
    }
    const size_t num_blocks = 64UL;
    auto clear_caches = [&] {
      Disk::TLocalWalkerCache::Cache->Clear(data_file_id, data_gen_id);
      TRAIITest::TLocalReadFileCache::Cache->Clear(data_file_id, data_gen_id);
    };
    auto new_walker = [&] {
      return std::make_unique<TPresentWalkFileWrapper>(engine, data_file_id, data_gen_id, int_idx, TKey(make_tuple(0L), &arena, state_alloc));
    };
    EXPECT_EQ(CountHeldBlocks(engine, num_blocks), 0U);
    /* The probe sees a walker's pages, a walker returned on its own runner is pooled with them, and Clear lets them go. */ {
      auto walker = new_walker();
      EXPECT_TRUE(*walker);
      EXPECT_GT(CountHeldBlocks(engine, num_blocks), 0U);
      walker.reset();
      EXPECT_GT(CountHeldBlocks(engine, num_blocks), 0U);
      clear_caches();
      EXPECT_EQ(CountHeldBlocks(engine, num_blocks), 0U);
    }
    /* Cleared while checked out: the walker comes back after Clear and is deleted, not pooled. */ {
      auto walker = new_walker();
      Disk::TLocalWalkerCache::Cache->Clear(data_file_id, data_gen_id);
      walker.reset();
      TRAIITest::TLocalReadFileCache::Cache->Clear(data_file_id, data_gen_id);
      EXPECT_EQ(CountHeldBlocks(engine, num_blocks), 0U);
    }
    /* Returned on another runner: the walker is deleted there, not pushed onto its home runner's free queue. */ {
      auto walker = new_walker();
      Disk::TLocalWalkerCache *home = Disk::TLocalWalkerCache::Cache.Get();
      auto *other = new Disk::TLocalWalkerCache();
      Disk::TLocalWalkerCache::Cache = other;
      walker.reset();
      Disk::TLocalWalkerCache::Cache = home;
      delete other;
      TRAIITest::TLocalReadFileCache::Cache->Clear(data_file_id, data_gen_id);
      EXPECT_EQ(CountHeldBlocks(engine, num_blocks), 0U);
      clear_caches();
      EXPECT_EQ(CountHeldBlocks(engine, num_blocks), 0U);
    }
    /* Asked for an index the file has no data for: the walker finds nothing but still reads the file and holds its
       pages while pooled, and Clear must reach it although the file lists no such index. */ {
      const Base::TUuid absent_idx(Base::TUuid::Twister);
      auto walker = std::make_unique<TPresentWalkFileWrapper>(engine, data_file_id, data_gen_id, absent_idx, TKey(make_tuple(0L), &arena, state_alloc));
      EXPECT_FALSE(*walker);
      walker.reset();
      TRAIITest::TLocalReadFileCache::Cache->Clear(data_file_id, data_gen_id);
      EXPECT_GT(CountHeldBlocks(engine, num_blocks), 0U);
      clear_caches();
      EXPECT_EQ(CountHeldBlocks(engine, num_blocks), 0U);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
    GracefullShutdown();
  });
}

/* #967: a trim frees the pooled walkers no reader took since the last one, and keeps the ones readers did. Each
   walker's Get trims on its own once the trim interval passes, so the test sets it to a day, however slowly it runs
   (TSan), and trims when it means to. */
FIXTURE(PoolIsTrimmed) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRAIITest required_thread_locals;
    const TScheduler::TPolicy scheduler_policy(10, 10, milliseconds(10));
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);

    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256MB */,
                               256,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);
    TEngine *engine = mem_engine.GetEngine();

    Base::TUuid data_file_id(TUuid::TimeAndMAC);
    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    size_t data_gen_id = 1;
    TSequenceNumber seq_num = 0U;
    Base::TUuid int_idx(Base::TUuid::Twister);
    Base::TUuid other_idx(Base::TUuid::Twister);
    /* data file */ {
      TMockMem mem_layer;
      for (int64_t i = 0; i < 11; i += 2) {
        Insert(mem_layer, ++seq_num, int_idx, static_cast<int64_t>(rand()), i);
        Insert(mem_layer, ++seq_num, other_idx, static_cast<int64_t>(rand()), i);
      }
      TDataFile data_file(engine, TVolume::TDesc::Fast, &mem_layer, data_file_id, data_gen_id, 20UL, 0U, RealTime);
    }
    Disk::TLocalWalkerCache *cache = Disk::TLocalWalkerCache::Cache.Get();
    cache->SetTrimInterval(hours(24));
    cache->Trim();
    auto new_walker = [&](const Base::TUuid &idx) {
      return std::make_unique<TPresentWalkFileWrapper>(engine, data_file_id, data_gen_id, idx, TKey(make_tuple(0L), &arena, state_alloc));
    };
    /* Twenty walkers out at once over two indexes, then all back in the pool. */ {
      std::vector<std::unique_ptr<TPresentWalkFileWrapper>> out;
      for (size_t i = 0UL; i < 20UL; ++i) {
        out.push_back(new_walker(i % 2UL ? int_idx : other_idx));
        EXPECT_TRUE(*out.back());
      }
      EXPECT_EQ(cache->GetNumPooled(), 0UL);
      out.clear();
      EXPECT_EQ(cache->GetNumPooled(), 20UL);
    }
    /* The pool grew from empty since the last trim, so a trim keeps it all. */
    cache->Trim();
    EXPECT_EQ(cache->GetNumPooled(), 20UL);
    /* Readers need three walkers of one index at once, then go. The next trim keeps those three and frees the rest. */ {
      std::vector<std::unique_ptr<TPresentWalkFileWrapper>> out;
      for (size_t i = 0UL; i < 3UL; ++i) {
        out.push_back(new_walker(int_idx));
        EXPECT_TRUE(*out.back());
      }
      EXPECT_EQ(cache->GetNumPooled(), 17UL);
    }
    EXPECT_EQ(cache->GetNumPooled(), 20UL);
    cache->Trim();
    EXPECT_EQ(cache->GetNumPooled(), 3UL);
    /* A trimmed pool still hands out walkers that read. */ {
      auto walker = new_walker(int_idx);
      EXPECT_TRUE(*walker);
      EXPECT_EQ(cache->GetNumPooled(), 2UL);
    }
    /* The next trim keeps the one walker taken since the last, and the trim after that empties the pool. */
    EXPECT_EQ(cache->Trim(), 2UL);
    EXPECT_EQ(cache->GetNumPooled(), 1UL);
    EXPECT_EQ(cache->Trim(), 1UL);
    EXPECT_EQ(cache->GetNumPooled(), 0UL);
    /* MaybeTrim trims only once the interval has passed. */ {
      {
        auto walker_a = new_walker(int_idx);
        auto walker_b = new_walker(other_idx);
      }
      EXPECT_EQ(cache->GetNumPooled(), 2UL);
      EXPECT_EQ(cache->MaybeTrim(), 0UL);
      EXPECT_EQ(cache->GetNumPooled(), 2UL);
      cache->SetTrimInterval(steady_clock::duration::zero());
      /* The first trim since they went back keeps them; the second frees them. */
      EXPECT_EQ(cache->MaybeTrim(), 0UL);
      EXPECT_EQ(cache->MaybeTrim(), 2UL);
      EXPECT_EQ(cache->GetNumPooled(), 0UL);
      cache->SetTrimInterval(hours(24));
    }
    /* A trim takes its walkers out of every loader before it deletes any, since a delete can yield. Here the file is
       cleared in between, as it could be on this runner while a delete waits, and its loaders go. Deleting the
       walkers afterwards must not touch them (ASan and TSan check). */ {
      {
        std::vector<std::unique_ptr<TPresentWalkFileWrapper>> out;
        for (size_t i = 0UL; i < 4UL; ++i) {
          out.push_back(new_walker(i % 2UL ? int_idx : other_idx));
        }
      }
      EXPECT_EQ(cache->Trim(), 0UL);
      Disk::TLocalWalkerCache::TPresentWalkFile *idle = cache->TakeIdle();
      EXPECT_EQ(cache->GetNumPooled(), 0UL);
      cache->Clear(data_file_id, data_gen_id);
      EXPECT_EQ(Disk::TLocalWalkerCache::DeleteWalkers(idle), 4UL);
      /* Nothing is left held. */
      TRAIITest::TLocalReadFileCache::Cache->Clear(data_file_id, data_gen_id);
      EXPECT_EQ(CountHeldBlocks(engine, 64UL), 0U);
    }
#ifndef NDEBUG
    /* The same, with the clear run between two of the trim's deletes, where a delete can yield. */ {
      {
        std::vector<std::unique_ptr<TPresentWalkFileWrapper>> out;
        for (size_t i = 0UL; i < 4UL; ++i) {
          out.push_back(new_walker(i % 2UL ? int_idx : other_idx));
        }
      }
      EXPECT_EQ(cache->Trim(), 0UL);
      /* Runs between each two of the four deletes; the first time, it clears the file. */
      size_t num_calls = 0UL;
      Disk::TLocalWalkerCache::AfterWalkerDeleted = [&] {
        if (!num_calls++) {
          cache->Clear(data_file_id, data_gen_id);
        }
      };
      EXPECT_EQ(cache->Trim(), 4UL);
      Disk::TLocalWalkerCache::AfterWalkerDeleted = nullptr;
      EXPECT_EQ(num_calls, 3UL);
      EXPECT_EQ(cache->GetNumPooled(), 0UL);
      TRAIITest::TLocalReadFileCache::Cache->Clear(data_file_id, data_gen_id);
      EXPECT_EQ(CountHeldBlocks(engine, 64UL), 0U);
    }
#endif
    cache->SetTrimInterval(Disk::TLocalWalkerCache::DefaultTrimInterval);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
    GracefullShutdown();
  });
}

/* The filter a disk layer keeps over a hash table, as TSafeRepo::MakeKeyFilter makes it. */
static unique_ptr<const TKeyFilter> MakeFilter(TEngine *engine, const Base::TUuid &file_id, size_t gen_id, const Base::TUuid &index_id,
                                               size_t num_defined, size_t max_slots = 1UL << 18) {
  unique_ptr<TLocalWalkerCache::TPresentWalkFile, TLocalWalkerCache::TPresentWalkFile::TDeleter> file(
      TLocalWalkerCache::Cache->Get(engine, file_id, gen_id, index_id));
  return file->MakeKeyFilter(num_defined, max_slots);
}

/* A read skips a file when the filter over its hash table rules the read's key out. The filter must never rule out a key or
   prefix the file holds, and a file it rules out must be one the read's walker finds empty: for a point, a pattern of
   trailing frees, and a range from a key to a pattern it unifies with, at the file's edges and in its gaps. */
FIXTURE(KeyFilter) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRAIITest required_thread_locals;
    const TScheduler::TPolicy scheduler_policy(10, 10, milliseconds(10));
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);

    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256MB */,
                               256,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);
    TEngine *engine = mem_engine.GetEngine();

    Base::TUuid data_file_id(TUuid::TimeAndMAC);
    TSuprena arena;
    size_t data_gen_id = 1;
    TSequenceNumber seq_num = 0U;
    Base::TUuid int_int_idx(Base::TUuid::Twister);
    Base::TUuid absent_idx(Base::TUuid::Twister);
    const vector<pair<int64_t, int64_t>> held{{1L, 2L}, {2L, 4L}, {7L, 14L}, {91L, 182L}, {92L, 184L}};
    /* data file 1 */ {
      TMockMem mem_layer;
      for (const auto &[a, b] : held) {
        Insert(mem_layer, ++seq_num, int_int_idx, 2L, a, b);
      }
      TDataFile data_file(engine, TVolume::TDesc::Fast, &mem_layer, data_file_id, data_gen_id, 20UL, 0U, RealTime);
    }
    const unique_ptr<const TKeyFilter> by_first = MakeFilter(engine, data_file_id, data_gen_id, int_int_idx, 1UL);
    const unique_ptr<const TKeyFilter> by_both = MakeFilter(engine, data_file_id, data_gen_id, int_int_idx, 2UL);
    EXPECT_TRUE(by_first != nullptr);
    EXPECT_TRUE(by_both != nullptr);
    if (by_first && by_both) {
    /* Everything the file holds passes. */
    for (const auto &[a, b] : held) {
      const TKey key(make_tuple(a, b), &arena, state_alloc);
      const TKey prefix(make_tuple(a, Native::TFree<int64_t>()), &arena, state_alloc);
      EXPECT_EQ(key.GetNumDefined(), 2UL);
      EXPECT_EQ(prefix.GetNumDefined(), 1UL);
      EXPECT_TRUE(by_both->MayContain(key.GetHash()));
      EXPECT_TRUE(by_first->MayContain(prefix.GetHash()));
    }
    /* Keys it doesn't hold: below its first, between and beside its keys, and just past its last. Each one ruled out is one
       the point walk finds nothing for. */ {
      const vector<pair<int64_t, int64_t>> probes{{0L, 0L}, {1L, 1L}, {1L, 3L}, {7L, 13L}, {7L, 15L}, {8L, 14L}, {92L, 185L}, {93L, 0L}};
      size_t ruled_out = 0UL;
      for (const auto &[a, b] : probes) {
        const TKey key(make_tuple(a, b), &arena, state_alloc);
        if (!by_both->MayContain(key.GetHash())) {
          ++ruled_out;
          CheckNotExists(engine, data_file_id, data_gen_id, int_int_idx, a, b);
        }
      }
      EXPECT_GE(ruled_out + 1UL, probes.size());
    }
    /* Prefixes it doesn't hold. A pattern walk and a range walk from (a, 0) to (a, free) find nothing for one ruled out, the
       range walk included: it starts at the next key in the file, (91, 182) for a = 8, which is past the range. */ {
      size_t ruled_out = 0UL;
      const vector<int64_t> probes{0L, 3L, 8L, 90L, 93L};
      for (const int64_t a : probes) {
        const TKey prefix(make_tuple(a, Native::TFree<int64_t>()), &arena, state_alloc);
        if (!by_first->MayContain(prefix.GetHash())) {
          ++ruled_out;
          EXPECT_EQ(CheckExistsFree(engine, data_file_id, data_gen_id, int_int_idx, [](const TKey &) {}, a, Native::TFree<int64_t>()), 0UL);
          TPresentWalkFileWrapper range(engine, data_file_id, data_gen_id, int_int_idx, TKey(make_tuple(a, 0L), &arena, state_alloc), prefix);
          EXPECT_FALSE(range);
        }
      }
      EXPECT_GE(ruled_out + 1UL, probes.size());
    }
    /* A range from (7, 0) to (7, free) yields only the key with that prefix. */ {
      const TKey prefix(make_tuple(7L, Native::TFree<int64_t>()), &arena, state_alloc);
      size_t found = 0UL;
      for (TPresentWalkFileWrapper range(engine, data_file_id, data_gen_id, int_int_idx, TKey(make_tuple(7L, 0L), &arena, state_alloc), prefix); range; ++range) {
        EXPECT_EQ(TKey((*range).Key, (*range).KeyArena), TKey(make_tuple(7L, 14L), &arena, state_alloc));
        ++found;
      }
      EXPECT_EQ(found, 1UL);
    }
    }  // if
    /* An index the file has no data for: the filter holds nothing. */ {
      const unique_ptr<const TKeyFilter> none = MakeFilter(engine, data_file_id, data_gen_id, absent_idx, 2UL);
      EXPECT_TRUE(none != nullptr);
      if (none) {
        EXPECT_FALSE(none->MayContain(TKey(make_tuple(1L, 2L), &arena, state_alloc).GetHash()));
        EXPECT_EQ(none->GetNumBytes(), 0UL);
      }
    }
    /* No filter, so every key may be there, for a table the file doesn't have or one past the slot bound. */ {
      EXPECT_TRUE(MakeFilter(engine, data_file_id, data_gen_id, int_int_idx, 0UL) == nullptr);
      EXPECT_TRUE(MakeFilter(engine, data_file_id, data_gen_id, int_int_idx, 3UL) == nullptr);
      EXPECT_TRUE(MakeFilter(engine, data_file_id, data_gen_id, int_int_idx, 2UL, 1UL) == nullptr);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
    GracefullShutdown();
  });
}

/* A pooled walker learns where its range ends only as far as it walks, so the next walk to take it from the pool must not
   inherit what the last one learned. Walk random ranges over one file with one pooled walker, stopping each walk after a
   random number of rows, and check every walk surfaces exactly the keys of its range, in order, up to where it stopped.
   Then release the file, as a merge does once it replaces it, and check a walk of the file that replaced it, and of the
   released one, builds a fresh walker and reads right. */
FIXTURE(PooledRangeWalkerReuse) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TRAIITest required_thread_locals;
    const TScheduler::TPolicy scheduler_policy(10, 10, milliseconds(10));
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);

    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256MB */,
                               256,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);
    TEngine *engine = mem_engine.GetEngine();

    TSuprena arena;
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    void *key_state_alloc = alloca(Sabot::State::GetMaxStateSize());
    TSequenceNumber seq_num = 0U;
    const Base::TUuid int_idx(Base::TUuid::Twister);
    const Base::TUuid old_file_id(TUuid::TimeAndMAC), new_file_id(TUuid::TimeAndMAC);
    const size_t old_gen_id = 1UL, new_gen_id = 2UL;
    /* The released file holds the even keys 0 through 198; the one that replaces it, the multiples of 3 from 0 through 297. */
    std::vector<int64_t> old_keys, new_keys;
    for (int64_t i = 0; i < 200; i += 2) {
      old_keys.push_back(i);
    }
    for (int64_t i = 0; i < 300; i += 3) {
      new_keys.push_back(i);
    }
    auto write_file = [&](const Base::TUuid &file_id, size_t gen_id, const std::vector<int64_t> &keys) {
      TMockMem mem_layer;
      for (int64_t key : keys) {
        Insert(mem_layer, ++seq_num, int_idx, static_cast<int64_t>(rand()), key);
      }
      TDataFile data_file(engine, TVolume::TDesc::Fast, &mem_layer, file_id, gen_id, 20UL, 0U, RealTime);
    };
    write_file(old_file_id, old_gen_id, old_keys);
    Disk::TLocalWalkerCache *cache = Disk::TLocalWalkerCache::Cache.Get();
    cache->SetTrimInterval(hours(24));
    /* Walk [from, to] of a file, stopping after `stop` rows, and check what it surfaced against `keys`. */
    auto check_walk = [&](const Base::TUuid &file_id, size_t gen_id, const std::vector<int64_t> &keys, int64_t from, int64_t to, size_t stop) {
      std::vector<int64_t> want;
      for (int64_t key : keys) {
        if (key >= from && key <= to && want.size() < stop) {
          want.push_back(key);
        }
      }
      std::vector<int64_t> got;
      TPresentWalkFileWrapper walker(engine, file_id, gen_id, int_idx, TKey(make_tuple(from), &arena, state_alloc), TKey(make_tuple(to), &arena, state_alloc));
      for (; walker && got.size() < stop; ++walker) {
        got.push_back(std::get<0>(Sabot::AsNative<std::tuple<int64_t>>(*Sabot::State::TAny::TWrapper((*walker).Key.NewState((*walker).KeyArena, key_state_alloc)))));
      }
      if (got.size() < stop) {
        EXPECT_FALSE(walker);
      }
      EXPECT_TRUE(got == want);
      return got == want;
    };
    /* A short walk of a long range, then walks of ranges that end before, at and after where it stopped. */
    EXPECT_TRUE(check_walk(old_file_id, old_gen_id, old_keys, 0, 198, 3));
    EXPECT_EQ(cache->GetNumPooled(), 1UL);
    EXPECT_TRUE(check_walk(old_file_id, old_gen_id, old_keys, 0, 2, 1000));
    EXPECT_TRUE(check_walk(old_file_id, old_gen_id, old_keys, 0, 5, 1000));
    EXPECT_TRUE(check_walk(old_file_id, old_gen_id, old_keys, 100, 120, 1000));
    EXPECT_TRUE(check_walk(old_file_id, old_gen_id, old_keys, -1, 1000, 1000));
    EXPECT_TRUE(check_walk(old_file_id, old_gen_id, old_keys, 50, 51, 1000));
    EXPECT_TRUE(check_walk(old_file_id, old_gen_id, old_keys, 199, 199, 1000));
    EXPECT_TRUE(check_walk(old_file_id, old_gen_id, old_keys, -5, -1, 1000));
    EXPECT_TRUE(check_walk(old_file_id, old_gen_id, old_keys, 198, 198, 1000));
    /* Random ranges and stops, all through the one pooled walker. */ {
      std::mt19937_64 rng(1018);
      size_t bad = 0UL;
      for (size_t walk = 0UL; walk < 400UL; ++walk) {
        const int64_t from = static_cast<int64_t>(rng() % 206UL) - 3;
        const int64_t to = from + static_cast<int64_t>(rng() % 60UL);
        const size_t stop = rng() % 4UL ? rng() % 8UL : 1000UL;
        if (!check_walk(old_file_id, old_gen_id, old_keys, from, to, stop)) {
          ++bad;
        }
      }
      EXPECT_EQ(bad, 0UL);
    }
    EXPECT_EQ(cache->GetNumPooled(), 1UL);
    /* The file is merged away: the walkers cache lets its walkers go, and the file that replaced it gets walkers of its own. */
    write_file(new_file_id, new_gen_id, new_keys);
    Disk::TLocalWalkerCache::Cache->Clear(old_file_id, old_gen_id);
    TRAIITest::TLocalReadFileCache::Cache->Clear(old_file_id, old_gen_id);
    EXPECT_EQ(cache->GetNumPooled(), 0UL);
    EXPECT_TRUE(check_walk(new_file_id, new_gen_id, new_keys, 0, 297, 3));
    EXPECT_TRUE(check_walk(new_file_id, new_gen_id, new_keys, 10, 40, 1000));
    EXPECT_TRUE(check_walk(new_file_id, new_gen_id, new_keys, -1, 1000, 1000));
    EXPECT_EQ(cache->GetNumPooled(), 1UL);
    /* A reader still holding the released file's generation builds a fresh walker for it, which reads it right. */
    EXPECT_TRUE(check_walk(old_file_id, old_gen_id, old_keys, 7, 31, 1000));
    EXPECT_EQ(cache->GetNumPooled(), 2UL);
    Disk::TLocalWalkerCache::Cache->Clear(old_file_id, old_gen_id);
    Disk::TLocalWalkerCache::Cache->Clear(new_file_id, new_gen_id);
    TRAIITest::TLocalReadFileCache::Cache->Clear(old_file_id, old_gen_id);
    TRAIITest::TLocalReadFileCache::Cache->Clear(new_file_id, new_gen_id);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
    GracefullShutdown();
  });
}
