/* <orly/indy/disk/data_file.test.cc>

   Unit test for <orly/indy/disk/data_file.h>.

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

#include <orly/indy/disk/data_file.h>

#include <functional>
#include <limits>
#include <vector>

#include <valgrind/callgrind.h>

#include <base/scheduler.h>
#include <base/usage_meter.h>
#include <orly/indy/disk/disk_test.h>
#include <orly/indy/disk/read_file.h>
#include <orly/indy/disk/sim/mem_engine.h>
#include <orly/indy/disk/test_file_service.h>
#include <orly/indy/fiber/fiber_test_runner.h>

#include <base/test/kit.h>

using namespace std;
using namespace chrono;
using namespace Base;
using namespace Orly;
using namespace Orly::Atom;
using namespace Orly::Indy;
using namespace Orly::Indy::Disk;
using namespace Orly::Indy::Disk::Util;

static const size_t BlockSize = Disk::Util::PhysicalBlockSize;

Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::Pool(sizeof(TRepo::TMapping), "Repo Mapping");
Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::TEntry::Pool(sizeof(TRepo::TMapping::TEntry), "Repo Mapping Entry");
Orly::Indy::Util::TPool L0::TManager::TRepo::TDataLayer::Pool(sizeof(TMemoryLayer), "Data Layer");

Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update", 750010UL);
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry", 1500020UL);
Disk::TBufBlock::TPool Disk::TBufBlock::Pool(BlockSize, 2000UL);

FIXTURE(Typical) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);

    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256MB */,
                               256 /* slow disk space: 256MB */,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);

    Base::TUuid file_id(TUuid::Best);
    TSequenceNumber seq_num = 0U;
    TUuid int_str_int_idx(TUuid::Twister);
    TUuid int_str_str_idx(TUuid::Twister);
    TUuid int_str_idx(TUuid::Twister);
    TUuid int_idx(TUuid::Twister);
    /* Make a data file */ {
      TSuprena arena;
      TMockMem mem_layer;
      /* insert data */ {
        /* insert <[int64_t, string, int64_t]> */
        Insert(mem_layer, ++seq_num, int_str_int_idx, 7L,
               1L, string("Orly"), 1L);
        Insert(mem_layer, ++seq_num, int_str_int_idx, 8L,
               1L, string("Orly"), 2L);
        /* insert <[int64_t, string, string]> */
        Insert(mem_layer, ++seq_num, int_str_str_idx, 14L,
               1L, string("Orly"), string("Orly"));
        Insert(mem_layer, ++seq_num, int_str_str_idx, 15L,
               1L, string("Orly"), string("Stih"));
        /* insert <[int64_t, string]> */
        Insert(mem_layer, ++seq_num, int_str_idx, 21L,
               1L, string("Orly"));
        Insert(mem_layer, ++seq_num, int_str_idx, 22L,
               1L, string("Taggef"));
        /* insert <[int64_t]> */
        Insert(mem_layer, ++seq_num, int_idx, 28L,
               1L);
        Insert(mem_layer, ++seq_num, int_idx, 29L,
               2L);
      }
      size_t data_gen_id = 1;
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
    }
    GracefullShutdown();
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(Deep) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);

    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256MB */,
                               256 /* slow disk space: 256MB */,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);

    Base::TUuid file_id(TUuid::Best);
    TSequenceNumber seq_num = 0U;
    TUuid int_str_decint_decstr_idx(TUuid::Twister);
    TUuid int_str_int_str_idx(TUuid::Twister);
    TUuid int_str_decint_str_idx(TUuid::Twister);
    /* Make data file 1 */ {
      TSuprena arena;
      TMockMem mem_layer;
      /* insert data */ {
        TSuprena suprena;
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        /* insert <[int64_t, string, desc<int64_t>, desc<string>]> */
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hello")}, {TDesc<int64_t>(9L), string("This is also a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short"));
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(20L), string("Some form of a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core"));
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(27L), string("This is also a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core"));
        /* insert <[int64_t, string, int64_t, string]> */
        Insert(mem_layer, ++seq_num, int_str_int_str_idx, TKey(std::set<int64_t>{1, 3, 9}, &suprena, state_alloc),
               1L, string("Orly"), 1L, string("short"));
      }
      size_t data_gen_id = 1;
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
      TReader reader(HERE, mem_engine.GetEngine(), file_id, data_gen_id);
      TReader::TArena main_arena(&reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TReader::TIndexFile int_str_decint_decstr_idx_file(&reader, int_str_decint_decstr_idx, RealTime);
      TReader::TArena int_str_decint_decstr_idx_arena(&int_str_decint_decstr_idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TReader::TIndexFile int_str_int_str_idx_file(&reader, int_str_int_str_idx, RealTime);
      TReader::TArena int_str_int_str_idx_arena(&int_str_int_str_idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TStream<Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::PhysicalBlockSize, Orly::Indy::Disk::Util::PageCheckedBlock, 0UL> in_stream(HERE, Source::PresentWalk, RealTime, &reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), 0);
      size_t out_offset;
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_int_str_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), 1L, string("short")), &arena, state_alloc), out_offset, in_stream, &int_str_int_str_idx_arena));
      /* check the <[int64_t, string, desc<int64_t>, desc<string>]> index */ {
        //TReader::TIndexFile idx_file(&reader, int_str_decint_decstr_idx, RealTime);
        std::vector<std::pair<TKey, TKey>> expected_vec;
        expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short")), &arena, state_alloc),
                                  TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hello")}, {TDesc<int64_t>(9L), string("This is also a longer string")}}, &arena, state_alloc));
        expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc),
                                  TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(27L), string("This is also a longer string")}}, &arena, state_alloc));
        size_t seen = 0UL;
        for (TReader::TIndexFile::TKeyCursor cur_key_csr(&int_str_decint_decstr_idx_file); cur_key_csr; ++cur_key_csr, ++seen) {
          const TReader::TIndexFile::TKeyItem &item = *cur_key_csr;
          EXPECT_TRUE(cur_key_csr);
          if (EXPECT_GE(expected_vec.size(), seen + 1)) {
            EXPECT_EQ(TKey(item.Key, &int_str_decint_decstr_idx_arena), expected_vec[seen].first);
            EXPECT_EQ(TKey(item.Value, &main_arena), expected_vec[seen].second);
          }
        }
        EXPECT_EQ(expected_vec.size(), seen);
      }  // done checking the <[int64_t, string, desc<int64_t>, desc<string>]> index
    }
    /* Make data file 2 (more of the same) */ {
      TSuprena arena;
      TMockMem mem_layer;
      /* insert data */ {
        TSuprena suprena;
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        /* insert <[int64_t, string, desc<int64_t>, desc<string>]> */
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hello")}, {TDesc<int64_t>(9L), string("This is also a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(2L), TDesc<string>("short"));
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(20L), string("Some form of a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(2L), TDesc<string>("This string should be too long to fit in a core"));
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(27L), string("This is also a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(4L), TDesc<string>("This string should be too long to fit in a core"));
        /* insert <[int64_t, string, int64_t, string]> */
        Insert(mem_layer, ++seq_num, int_str_int_str_idx, TKey(std::set<int64_t>{1, 3, 9}, &suprena, state_alloc),
               1L, string("Orly"), 2L, string("short"));
      }
      size_t data_gen_id = 2;
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
      TReader reader(HERE, mem_engine.GetEngine(), file_id, data_gen_id);
      TReader::TArena main_arena(&reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TReader::TIndexFile int_str_decint_decstr_idx_file(&reader, int_str_decint_decstr_idx, RealTime);
      TReader::TArena int_str_decint_decstr_idx_arena(&int_str_decint_decstr_idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TReader::TIndexFile int_str_int_str_idx_file(&reader, int_str_int_str_idx, RealTime);
      TReader::TArena int_str_int_str_idx_arena(&int_str_int_str_idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TStream<Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::PhysicalBlockSize, Orly::Indy::Disk::Util::PageCheckedBlock, 0UL> in_stream(HERE, Source::PresentWalk, RealTime, &reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), 0);
      size_t out_offset;
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(2L), TDesc<string>("short")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(2L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(4L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_int_str_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), 2L, string("short")), &arena, state_alloc), out_offset, in_stream, &int_str_int_str_idx_arena));
      /* check the <[int64_t, string, desc<int64_t>, desc<string>]> index */ {
        std::vector<std::pair<TKey, TKey>> expected_vec;
        expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(4L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc),
                                  TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(27L), string("This is also a longer string")}}, &arena, state_alloc));
        expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(2L), TDesc<string>("short")), &arena, state_alloc),
                                  TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hello")}, {TDesc<int64_t>(9L), string("This is also a longer string")}}, &arena, state_alloc));
        expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(2L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc),
                                  TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(20L), string("Some form of a longer string")}}, &arena, state_alloc));
        size_t seen = 0UL;
        for (TReader::TIndexFile::TKeyCursor cur_key_csr(&int_str_decint_decstr_idx_file); cur_key_csr; ++cur_key_csr, ++seen) {
          const TReader::TIndexFile::TKeyItem &item = *cur_key_csr;
          EXPECT_TRUE(cur_key_csr);
          if (EXPECT_GE(expected_vec.size(), seen + 1)) {
            EXPECT_EQ(TKey(item.Key, &int_str_decint_decstr_idx_arena), expected_vec[seen].first);
            EXPECT_EQ(TKey(item.Value, &main_arena), expected_vec[seen].second);
          }
        }
        EXPECT_EQ(expected_vec.size(), seen);
      }  // done checking the <[int64_t, string, desc<int64_t>, desc<string>]> index
    }
    /* Make data file 3 (something different) */ {
      TSuprena arena;
      TMockMem mem_layer;
      /* insert data */ {
        TSuprena suprena;
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        /* insert <[int64_t, string, desc<int64_t>, desc<string>]> */
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hello")}, {TDesc<int64_t>(9L), string("This is also a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(5L), TDesc<string>("short"));
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(20L), string("Some form of a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(5L), TDesc<string>("This string should be too long to fit in a core"));
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(27L), string("This is also a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(6L), TDesc<string>("This string should be too long to fit in a core"));
        /* insert <[int64_t, string, desc<int64_t>, string]> */
        Insert(mem_layer, ++seq_num, int_str_decint_str_idx, TKey(std::set<TDesc<int64_t>>{TDesc<int64_t>(1), TDesc<int64_t>(7), TDesc<int64_t>(20)}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(2L), string("short"));
      }
      size_t data_gen_id = 3;
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
      TReader reader(HERE, mem_engine.GetEngine(), file_id, data_gen_id);
      TReader::TArena main_arena(&reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TReader::TIndexFile int_str_decint_decstr_idx_file(&reader, int_str_decint_decstr_idx, RealTime);
      TReader::TArena int_str_decint_decstr_idx_arena(&int_str_decint_decstr_idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TReader::TIndexFile int_str_decint_str_idx_file(&reader, int_str_decint_str_idx, RealTime);
      TReader::TArena int_str_decint_str_idx_arena(&int_str_decint_str_idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TStream<Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::PhysicalBlockSize, Orly::Indy::Disk::Util::PageCheckedBlock, 0UL> in_stream(HERE, Source::PresentWalk, RealTime, &reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), 0);
      size_t out_offset;
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(5L), TDesc<string>("short")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(5L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(6L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_decint_str_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(2L), string("short")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_str_idx_arena));
      /* check the <[int64_t, string, desc<int64_t>, desc<string>]> index */ {
        //TReader::TIndexFile idx_file(&reader, int_str_decint_decstr_idx, RealTime);
        std::vector<std::pair<TKey, TKey>> expected_vec;
        expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(6L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc),
                                  TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(27L), string("This is also a longer string")}}, &arena, state_alloc));
        expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(5L), TDesc<string>("short")), &arena, state_alloc),
                                  TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hello")}, {TDesc<int64_t>(9L), string("This is also a longer string")}}, &arena, state_alloc));
        expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(5L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc),
                                  TKey(std::map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(20L), string("Some form of a longer string")}}, &arena, state_alloc));
        size_t seen = 0UL;
        for (TReader::TIndexFile::TKeyCursor cur_key_csr(&int_str_decint_decstr_idx_file); cur_key_csr; ++cur_key_csr, ++seen) {
          const TReader::TIndexFile::TKeyItem &item = *cur_key_csr;
          EXPECT_TRUE(cur_key_csr);
          if (EXPECT_GE(expected_vec.size(), seen + 1)) {
            EXPECT_EQ(TKey(item.Key, &int_str_decint_decstr_idx_arena), expected_vec[seen].first);
            EXPECT_EQ(TKey(item.Value, &main_arena), expected_vec[seen].second);
          }
        }
        EXPECT_EQ(expected_vec.size(), seen);
      }  // done checking the <[int64_t, string, desc<int64_t>, desc<string>]> index
    }
    GracefullShutdown();
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(History) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);
    Base::TUuid file_id(TUuid::Best);
    TSequenceNumber seq_num = 0U;

    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256MB */,
                               256 /* slow disk space: 256MB */,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);

    TUuid int_str_int_idx(TUuid::Twister);
    /* Make a data file */ {
      TSuprena arena;
      TMockMem mem_layer;
      /* insert data */ {
        /* insert <[int64_t, string, int64_t]> */
        Insert(mem_layer, ++seq_num, int_str_int_idx, 7L,
               1L, string("Orly"), 1L);
        Insert(mem_layer, ++seq_num, int_str_int_idx, 8L,
               1L, string("Orly"), 2L);
        Insert(mem_layer, ++seq_num, int_str_int_idx, 9L,
               1L, string("Orly"), 3L);
        Insert(mem_layer, ++seq_num, int_str_int_idx, 10L,
               1L, string("Orly"), 4L);
        Insert(mem_layer, ++seq_num, int_str_int_idx, 11L,
               1L, string("Orly"), 2L);
        Insert(mem_layer, ++seq_num, int_str_int_idx, 12L,
               1L, string("Orly"), 3L);
      }
      size_t data_gen_id = 1;
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
    }
    GracefullShutdown();
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #925: a note that straddles a block boundary is copied out of two cache pages, and the second page may not fit. With
   every page buffer in the cache referenced, reading it throws TOutOfCachePages. TryAcquireNote used to swallow that and
   return a null note, which a release build then dereferenced; it must let the error out instead, from both overloads
   and from Pin. This checks the return value directly because a debug build's TPin already turned a null into
   TNotFound. */
FIXTURE(StraddlingNoteOutOfCachePages) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);

    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256MB */,
                               256 /* slow disk space: 256MB */,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               256 /* block cache slots: 16MB */,
                               1 /* num block lru */);
    Disk::Util::TEngine *engine = mem_engine.GetEngine();
    using TCachePage = Disk::Util::TCache<TReader::PhysicalCachePageSize>;
    TCachePage *cache = engine->GetCache<TReader::PhysicalCachePageSize>();

    Base::TUuid file_id(TUuid::Best);
    const size_t data_gen_id = 1;
    TSequenceNumber seq_num = 0U;
    TUuid idx(TUuid::Twister);
    /* Long values land in the main arena as notes of a few KB, so some cross a block boundary. */ {
      TMockMem mem_layer;
      for (int64_t i = 0; i < 100; ++i) {
        TSuprena suprena;
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        Insert(mem_layer, ++seq_num, idx, TKey(string(3000 + static_cast<size_t>(i) * 37, 'v'), &suprena, state_alloc), i);
      }
      TDataFile data_file(engine, TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
    }

    TReader reader(HERE, engine, file_id, data_gen_id);
    const size_t arena_start = static_cast<TArenaInFile &>(reader).GetByteOffsetOfArena();
    /* Find a note that starts in the same block as a chunk-contiguous note before it (so the earlier note can load the
       first page) and ends in the next block, in a block other than the arena's first. */
    bool found = false;
    size_t prev_offset = 0UL, prev_size = 0UL, target_offset = 0UL, target_size = 0UL;
    /* The scan's stream keeps every page it read referenced until it goes. */ {
      TReader::TArena scan(&reader, cache, RealTime);
      bool have_prev = false;
      for (TReader::TArena::TCursor csr(&scan, 0, scan.GetNumBytesOfArena()); csr && !found; ++csr) {
        const size_t offset = csr.GetOffset();
        const size_t size = sizeof(TCore::TNote) + csr->GetRawSize();
        const size_t abs = arena_start + offset;
        if (have_prev &&
            abs % LogicalBlockSize != 0UL &&
            abs / LogicalBlockSize != arena_start / LogicalBlockSize &&
            (arena_start + prev_offset) / LogicalBlockSize == abs / LogicalBlockSize &&
            (arena_start + prev_offset) / LogicalBlockSize == (arena_start + prev_offset + prev_size) / LogicalBlockSize &&
            abs / LogicalBlockSize != (abs + size) / LogicalBlockSize) {
          found = true;
          target_offset = offset;
          target_size = size;
        } else {
          prev_offset = offset;
          prev_size = size;
          have_prev = true;
        }
      }
    }
    if (EXPECT_TRUE(found)) {
      TReader::TArena arena(&reader, cache, RealTime);
      /* Load the first page into the arena's stream, then use up every other page buffer. */ {
        void *d1 = nullptr, *d2 = nullptr, *d3 = nullptr;
        const TCore::TNote *warm = arena.TryAcquireNote(prev_offset, d1, d2, d3);
        EXPECT_TRUE(warm != nullptr);
        arena.ReleaseNote(warm, prev_offset, d1, d2, d3);
      }
      const size_t bogus_page_id_base = 100000000UL;
      std::vector<std::pair<TCachePage::TSlot *, size_t>> held;
      bool full = false;
      try {
        for (size_t i = 0; i < 100000UL; ++i) {
          TCachePage::TSlot *data_slot;
          const size_t page_id = bogus_page_id_base + i;
          held.emplace_back(cache->Get(page_id, data_slot), page_id);
        }
      } catch (const Disk::Util::TOutOfCachePages &) {
        full = true;
      }
      EXPECT_TRUE(full);
      /* Returns true if the call threw TOutOfCachePages, and false, having left nothing held, if it came back with a note. */
      auto reports_out_of_cache = [&](const std::function<const TCore::TNote *(void *&, void *&, void *&)> &acquire) {
        void *d1 = nullptr, *d2 = nullptr, *d3 = nullptr;
        bool threw = false;
        const TCore::TNote *note = nullptr;
        try {
          note = acquire(d1, d2, d3);
        } catch (const Disk::Util::TOutOfCachePages &) {
          threw = true;
        }
        if (note) {
          arena.ReleaseNote(note, target_offset, d1, d2, d3);
        }
        return threw;
      };
      EXPECT_TRUE(reports_out_of_cache([&](void *&d1, void *&d2, void *&d3) {
        return arena.TryAcquireNote(target_offset, d1, d2, d3);
      }));
      EXPECT_TRUE(reports_out_of_cache([&](void *&d1, void *&d2, void *&d3) {
        return arena.TryAcquireNote(target_offset, target_size, d1, d2, d3);
      }));
      /* Pin goes through the same calls and must not hand back a pin on a null note either. */ {
        void *pin_alloc = alloca(sizeof(TCore::TArena::TFinalPin));
        bool threw = false;
        try {
          TCore::TArena::TFinalPin *pin = arena.Pin(target_offset, pin_alloc);
          pin->TPin::~TPin();
        } catch (const Disk::Util::TOutOfCachePages &) {
          threw = true;
        } catch (const std::exception &) {
        }
        EXPECT_TRUE(threw);
      }
      for (const auto &slot : held) {
        cache->Release(slot.first, slot.second);
      }
      /* With buffers free again the same note reads fine, and is the size the scan saw. */ {
        void *d1 = nullptr, *d2 = nullptr, *d3 = nullptr;
        const TCore::TNote *note = arena.TryAcquireNote(target_offset, d1, d2, d3);
        if (EXPECT_TRUE(note != nullptr)) {
          EXPECT_EQ(sizeof(TCore::TNote) + note->GetRawSize(), target_size);
          arena.ReleaseNote(note, target_offset, d1, d2, d3);
        }
      }
    }
    GracefullShutdown();
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* A note pin is taken from the page the arena's stream holds, with no slot lock. Move the stream to another page
   while the note is held (the stream's own release of the first page is then the fast one, count 2 -> 1), then
   release the note (the locked 1 -> 0). Neither page may be left referenced. */
FIXTURE(NotePinOutlivesStreamMove) {
  Fiber::TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    const TScheduler::TPolicy scheduler_policy(4, 10, milliseconds(10));
    TScheduler scheduler;
    scheduler.SetPolicy(scheduler_policy);

    Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 256, 1);
    Disk::Util::TEngine *engine = mem_engine.GetEngine();
    using TCachePage = Disk::Util::TCache<TReader::PhysicalCachePageSize>;
    TCachePage *cache = engine->GetCache<TReader::PhysicalCachePageSize>();

    Base::TUuid file_id(TUuid::Best);
    const size_t data_gen_id = 1;
    TSequenceNumber seq_num = 0U;
    TUuid idx(TUuid::Twister);
    /* Small notes, so most sit inside one chunk and can be pinned in place, and enough of them to span several pages. */ {
      TMockMem mem_layer;
      for (int64_t i = 0; i < 3000; ++i) {
        TSuprena suprena;
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        Insert(mem_layer, ++seq_num, idx, TKey(to_string(i) + string(200, 'v'), &suprena, state_alloc), i);
      }
      TDataFile data_file(engine, TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
    }

    TReader reader(HERE, engine, file_id, data_gen_id);
    std::vector<size_t> offsets;
    /* The scan's stream goes out of scope before the check below. */ {
      TReader::TArena scan(&reader, cache, RealTime);
      for (TReader::TArena::TCursor csr(&scan, 0, scan.GetNumBytesOfArena()); csr; ++csr) {
        offsets.push_back(csr.GetOffset());
      }
    }
    /* References held on a page other than the probe's own. Something outside the arena (the reader) may keep a
       steady one, so compare against a run that has already finished instead of expecting zero. */
    auto held = [cache](size_t page_id) { return cache->CountReferences(page_id); };
    size_t page_a = 0UL, page_b = 0UL;
    /* One arena: pin note A, move its stream to another page while A is held, then release A. `baseline_a` is
       the steady count on A's page from an earlier run, or SIZE_MAX if there is none yet. */
    auto run = [&](size_t baseline_a) {
      bool moved = false;
      TReader::TArena arena(&reader, cache, RealTime);
      void *a1 = nullptr, *a2 = nullptr, *a3 = nullptr;
      const TCore::TNote *note_a = nullptr;
      size_t offset_a = 0UL;
      for (size_t i = 0; i < offsets.size() && !note_a; ++i) {
        const TCore::TNote *note = arena.TryAcquireNote(offsets[i], a1, a2, a3);
        if (note && a1 == nullptr) {
          /* Copied out rather than pinned; try the next. */
          arena.ReleaseNote(note, offsets[i], a1, a2, a3);
        } else if (note) {
          note_a = note;
          offset_a = offsets[i];
        }
      }
      if (EXPECT_TRUE(note_a != nullptr)) {
        page_a = reinterpret_cast<size_t>(a3);
        /* Move the stream to a note on another page, holding note A throughout. */
        for (size_t i = 0; i < offsets.size() && !moved; ++i) {
          void *b1 = nullptr, *b2 = nullptr, *b3 = nullptr;
          const TCore::TNote *note_b = arena.TryAcquireNote(offsets[i], b1, b2, b3);
          if (note_b) {
            if (b1 != nullptr && reinterpret_cast<size_t>(b3) != page_a) {
              page_b = reinterpret_cast<size_t>(b3);
              moved = true;
            }
            arena.ReleaseNote(note_b, offsets[i], b1, b2, b3);
          }
        }
        EXPECT_TRUE(moved);
        /* The note still pins A's page after the stream has left it. */
        if (baseline_a != std::numeric_limits<size_t>::max()) {
          EXPECT_TRUE(held(page_a) > baseline_a);
        }
        arena.ReleaseNote(note_a, offset_a, a1, a2, a3);
      }
      return moved;
    };
    if (EXPECT_TRUE(run(std::numeric_limits<size_t>::max()))) {
      const size_t steady_a = held(page_a);
      const size_t steady_b = held(page_b);
      EXPECT_TRUE(run(steady_a));
      EXPECT_EQ(held(page_a), steady_a);
      EXPECT_EQ(held(page_b), steady_b);
    }
    GracefullShutdown();
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}
