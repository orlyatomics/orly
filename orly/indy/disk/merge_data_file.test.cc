/* <orly/indy/disk/merge_data_file.test.cc>

   Unit test for <orly/indy/disk/merge_data_file.h>.

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

#include <orly/indy/disk/merge_data_file.h>

#include <valgrind/callgrind.h>

#include <base/scheduler.h>
#include <orly/indy/disk/data_file.h>
#include <orly/indy/disk/disk_test.h>
#include <orly/indy/disk/fold_data_file.h>
#include <orly/indy/disk/read_file.h>
#include <orly/indy/disk/sim/mem_engine.h>
#include <orly/indy/disk/update_walk_file.h>
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
using namespace Orly::Indy::Fiber;

static const size_t BlockSize = Disk::Util::PhysicalBlockSize;

Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::Pool(sizeof(TRepo::TMapping), "Repo Mapping");
Orly::Indy::Util::TPool L0::TManager::TRepo::TMapping::TEntry::Pool(sizeof(TRepo::TMapping::TEntry), "Repo Mapping Entry");
Orly::Indy::Util::TPool L0::TManager::TRepo::TDataLayer::Pool(sizeof(TMemoryLayer), "Data Layer");

Orly::Indy::Util::TPool TUpdate::Pool(sizeof(TUpdate), "Update", 1048578UL);
Orly::Indy::Util::TPool TUpdate::TEntry::Pool(sizeof(TUpdate::TEntry), "Entry", 1048578UL);
Disk::TBufBlock::TPool Disk::TBufBlock::Pool(BlockSize, 2000UL);

FIXTURE(BasicTailing) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));

    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256MB */,
                               256 /* slow disk space: 256MB */,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);

    Base::TUuid file_id(TUuid::Best);
    TSequenceNumber seq_num = 0U;
    TUuid index_id(TUuid::Twister);
    /* Make data file 1 */ {
      TSuprena arena;
      TMockMem mem_layer;
      /* insert data */ {
        TSuprena suprena;
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        /* insert <[int64_t, string, desc<int64_t>, desc<string>]> */
        Insert(mem_layer, ++seq_num, index_id, TKey(46L, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short"));
        Insert(mem_layer, ++seq_num, index_id, TKey(49L, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core"));
        Insert(mem_layer, ++seq_num, index_id, TKey(57L, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core"));
      }
      size_t data_gen_id = 1;
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
    }
    /* Make data file 2 (more of the same) */ {
      TSuprena arena;
      TMockMem mem_layer;
      /* insert data */ {
        TSuprena suprena;
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        /* insert <[int64_t, string, desc<int64_t>, desc<string>]> */
        Insert(mem_layer, ++seq_num, index_id, TKey(7L, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short"));
        Insert(mem_layer, ++seq_num, index_id, TKey(409L, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("Here's a new one"));
        Insert(mem_layer, ++seq_num, index_id, TKey(Native::TTombstone::Tombstone, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core"));
      }
      size_t data_gen_id = 2;
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
    }
    /* merge them. Here seq (4,5,6) should be visible */ {
      TSuprena arena;
      TMergeDataFile merge_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, vector<size_t>{1, 2}, file_id, 4UL, 0U, Low, 16384, 20UL, true, false);
      TReader reader(HERE, mem_engine.GetEngine(), file_id, 4UL);
      TReader::TArena main_arena(&reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TReader::TIndexFile idx_file(&reader, index_id, RealTime);
      TReader::TArena index_arena(&idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TStream<Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::PhysicalBlockSize, Orly::Indy::Disk::Util::PageCheckedBlock, 0UL> in_stream(HERE, Source::PresentWalk, RealTime, &reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), 0);
      size_t out_offset;
      /* what came from file 1 but got overriden in 2 */
      EXPECT_TRUE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short")), &arena, state_alloc), out_offset, in_stream, &index_arena));
      /* this has been tombstoned in file 2, but because it had history it's still visible as a tombstone */
      EXPECT_TRUE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &index_arena));
      /* this got added in file 2 */
      EXPECT_TRUE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("Here's a new one")), &arena, state_alloc), out_offset, in_stream, &index_arena));
      vector<pair<TKey, TKey>> expected_vec;
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short")), &arena, state_alloc),
                                TKey(7L, &arena, state_alloc));
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc),
                                TKey(Native::TTombstone::Tombstone, &arena, state_alloc));
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("Here's a new one")), &arena, state_alloc),
                                TKey(409L, &arena, state_alloc));
      size_t seen = 0UL;
      for (TReader::TIndexFile::TKeyCursor cur_key_csr(&idx_file); cur_key_csr; ++cur_key_csr, ++seen) {
        const TReader::TIndexFile::TKeyItem &item = *cur_key_csr;
        EXPECT_TRUE(cur_key_csr);
        if (EXPECT_GE(expected_vec.size(), seen + 1)) {
          EXPECT_EQ(TKey(item.Key, &index_arena), expected_vec[seen].first);
          EXPECT_EQ(TKey(item.Value, &main_arena), expected_vec[seen].second);
        }
      }
      EXPECT_EQ(expected_vec.size(), seen);
    }
    /* merge it again to get rid of the remaining history behind the tombstone. Here seq (4,5,6) should be visible */ {
      TSuprena arena;
      TMergeDataFile merge_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, vector<size_t>{4}, file_id, 5UL, 0U, Low, 16384, 20UL, true, true);
      TReader reader(HERE, mem_engine.GetEngine(), file_id, 5UL);
      TReader::TArena main_arena(&reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TReader::TIndexFile idx_file(&reader, index_id, RealTime);
      TReader::TArena index_arena(&idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TStream<Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::PhysicalBlockSize, Orly::Indy::Disk::Util::PageCheckedBlock, 0UL> in_stream(HERE, Source::PresentWalk, RealTime, &reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), 0);
      size_t out_offset;
      /* what came from file 1 but got overriden in 2 */
      EXPECT_TRUE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short")), &arena, state_alloc), out_offset, in_stream, &index_arena));
      /* this has been tombstoned in file 2, but because it had history it's still visible as a tombstone */
      EXPECT_TRUE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &index_arena));
      /* this got added in file 2 */
      EXPECT_TRUE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("Here's a new one")), &arena, state_alloc), out_offset, in_stream, &index_arena));
      vector<pair<TKey, TKey>> expected_vec;
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short")), &arena, state_alloc),
                                TKey(7L, &arena, state_alloc));
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc),
                                TKey(Native::TTombstone::Tombstone, &arena, state_alloc));
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("Here's a new one")), &arena, state_alloc),
                                TKey(409L, &arena, state_alloc));
      size_t seen = 0UL;
      for (TReader::TIndexFile::TKeyCursor cur_key_csr(&idx_file); cur_key_csr; ++cur_key_csr, ++seen) {
        const TReader::TIndexFile::TKeyItem &item = *cur_key_csr;
        EXPECT_TRUE(cur_key_csr);
        if (EXPECT_GE(expected_vec.size(), seen + 1)) {
          EXPECT_EQ(TKey(item.Key, &index_arena), expected_vec[seen].first);
          EXPECT_EQ(TKey(item.Value, &main_arena), expected_vec[seen].second);
        }
      }
      EXPECT_EQ(expected_vec.size(), seen);
    }
    /* merge it again to get rid of the tombstone. Here seq (4,5) should be visible */ {
      TSuprena arena;
      TMergeDataFile merge_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, vector<size_t>{5}, file_id, 6UL, 0U, Low, 16384, 20UL, true, true);
      TReader reader(HERE, mem_engine.GetEngine(), file_id, 6UL);
      TReader::TArena main_arena(&reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TReader::TIndexFile idx_file(&reader, index_id, RealTime);
      TReader::TArena index_arena(&idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TStream<Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::PhysicalBlockSize, Orly::Indy::Disk::Util::PageCheckedBlock, 0UL> in_stream(HERE, Source::PresentWalk, RealTime, &reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), 0);
      size_t out_offset;
      /* what came from file 1 but got overriden in 2 */
      EXPECT_TRUE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short")), &arena, state_alloc), out_offset, in_stream, &index_arena));
      /* this has been tombstoned in file 2. */
      EXPECT_FALSE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &index_arena));
      /* this got added in file 2 */
      EXPECT_TRUE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("Here's a new one")), &arena, state_alloc), out_offset, in_stream, &index_arena));
      vector<pair<TKey, TKey>> expected_vec;
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short")), &arena, state_alloc),
                                TKey(7L, &arena, state_alloc));
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("Here's a new one")), &arena, state_alloc),
                                TKey(409L, &arena, state_alloc));
      size_t seen = 0UL;
      for (TReader::TIndexFile::TKeyCursor cur_key_csr(&idx_file); cur_key_csr; ++cur_key_csr, ++seen) {
        const TReader::TIndexFile::TKeyItem &item = *cur_key_csr;
        EXPECT_TRUE(cur_key_csr);
        if (EXPECT_GE(expected_vec.size(), seen + 1)) {
          EXPECT_EQ(TKey(item.Key, &index_arena), expected_vec[seen].first);
          EXPECT_EQ(TKey(item.Value, &main_arena), expected_vec[seen].second);
        }
      }
      EXPECT_EQ(expected_vec.size(), seen);
    }
    GracefullShutdown();
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(BasicTailingDisabled) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));

    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256MB */,
                               256 /* slow disk space: 256MB */,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);

    Base::TUuid file_id(TUuid::Best);
    TSequenceNumber seq_num = 0U;
    TUuid index_id(TUuid::Twister);
    /* Make data file 1 */ {
      TSuprena arena;
      TMockMem mem_layer;
      /* insert data */ {
        TSuprena suprena;
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        /* insert <[int64_t, string, desc<int64_t>, desc<string>]> */
        Insert(mem_layer, ++seq_num, index_id, TKey(46L, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short"));
        Insert(mem_layer, ++seq_num, index_id, TKey(49L, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core"));
        Insert(mem_layer, ++seq_num, index_id, TKey(57L, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core"));
      }
      size_t data_gen_id = 1;
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
    }
    /* Make data file 2 (more of the same) */ {
      TSuprena arena;
      TMockMem mem_layer;
      /* insert data */ {
        TSuprena suprena;
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        /* insert <[int64_t, string, desc<int64_t>, desc<string>]> */
        Insert(mem_layer, ++seq_num, index_id, TKey(7L, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short"));
        Insert(mem_layer, ++seq_num, index_id, TKey(409L, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("Here's a new one"));
        Insert(mem_layer, ++seq_num, index_id, TKey(Native::TTombstone::Tombstone, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core"));
      }
      size_t data_gen_id = 2;
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
    }
    /* merge them. Here seq (4,5,6) should be visible */ {
      TSuprena arena;
      TMergeDataFile merge_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, vector<size_t>{1, 2}, file_id, 4UL, 0U, Low, 16384, 20UL, false, false);
      TReader reader(HERE, mem_engine.GetEngine(), file_id, 4UL);
      TReader::TArena main_arena(&reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TReader::TIndexFile idx_file(&reader, index_id, RealTime);
      TReader::TArena index_arena(&idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TStream<Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::PhysicalBlockSize, Orly::Indy::Disk::Util::PageCheckedBlock, 0UL> in_stream(HERE, Source::PresentWalk, RealTime, &reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), 0);
      size_t out_offset;
      /* what came from file 1 but got overriden in 2 */
      EXPECT_TRUE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short")), &arena, state_alloc), out_offset, in_stream, &index_arena));
      /* this has been tombstoned in file 2, but because it had history it's still visible as a tombstone */
      EXPECT_TRUE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &index_arena));
      /* this got added in file 2 */
      EXPECT_TRUE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("Here's a new one")), &arena, state_alloc), out_offset, in_stream, &index_arena));
      vector<pair<TKey, TKey>> expected_vec;
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short")), &arena, state_alloc),
                                TKey(7L, &arena, state_alloc));
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc),
                                TKey(Native::TTombstone::Tombstone, &arena, state_alloc));
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("Here's a new one")), &arena, state_alloc),
                                TKey(409L, &arena, state_alloc));
      size_t seen = 0UL;
      for (TReader::TIndexFile::TKeyCursor cur_key_csr(&idx_file); cur_key_csr; ++cur_key_csr, ++seen) {
        const TReader::TIndexFile::TKeyItem &item = *cur_key_csr;
        EXPECT_TRUE(cur_key_csr);
        if (EXPECT_GE(expected_vec.size(), seen + 1)) {
          EXPECT_EQ(TKey(item.Key, &index_arena), expected_vec[seen].first);
          EXPECT_EQ(TKey(item.Value, &main_arena), expected_vec[seen].second);
        }
      }
      EXPECT_EQ(expected_vec.size(), seen);
    }
    for (size_t i = 0; i < 3; ++i) {
      /* merge it again to get make sure nothing changed. Here seq (4,5,6) should be visible */ {
        TSuprena arena;
        TMergeDataFile merge_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, vector<size_t>{(4 + i)}, file_id, 5UL + i, 0U, Low, 16384, 20UL, false, false);
        TReader reader(HERE, mem_engine.GetEngine(), file_id, 5UL);
        TReader::TArena main_arena(&reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
        TReader::TIndexFile idx_file(&reader, index_id, RealTime);
        TReader::TArena index_arena(&idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
        TStream<Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::PhysicalBlockSize, Orly::Indy::Disk::Util::PageCheckedBlock, 0UL> in_stream(HERE, Source::PresentWalk, RealTime, &reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), 0);
        size_t out_offset;
        /* what came from file 1 but got overriden in 2 */
        EXPECT_TRUE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short")), &arena, state_alloc), out_offset, in_stream, &index_arena));
        /* this has been tombstoned in file 2, but because it had history it's still visible as a tombstone */
        EXPECT_TRUE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &index_arena));
        /* this got added in file 2 */
        EXPECT_TRUE(idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("Here's a new one")), &arena, state_alloc), out_offset, in_stream, &index_arena));
        vector<pair<TKey, TKey>> expected_vec;
        expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short")), &arena, state_alloc),
                                  TKey(7L, &arena, state_alloc));
        expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc),
                                  TKey(Native::TTombstone::Tombstone, &arena, state_alloc));
        expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("Here's a new one")), &arena, state_alloc),
                                  TKey(409L, &arena, state_alloc));
        size_t seen = 0UL;
        for (TReader::TIndexFile::TKeyCursor cur_key_csr(&idx_file); cur_key_csr; ++cur_key_csr, ++seen) {
          const TReader::TIndexFile::TKeyItem &item = *cur_key_csr;
          EXPECT_TRUE(cur_key_csr);
          if (EXPECT_GE(expected_vec.size(), seen + 1)) {
            EXPECT_EQ(TKey(item.Key, &index_arena), expected_vec[seen].first);
            EXPECT_EQ(TKey(item.Value, &main_arena), expected_vec[seen].second);
          }
        }
        EXPECT_EQ(expected_vec.size(), seen);
      }
    }
    GracefullShutdown();
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(Deep) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));

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
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hello")}, {TDesc<int64_t>(9L), string("This is also a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short"));
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(20L), string("Some form of a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core"));
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(27L), string("This is also a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core"));
        /* insert <[int64_t, string, int64_t, string]> */
        Insert(mem_layer, ++seq_num, int_str_int_str_idx, TKey(set<int64_t>{1, 3, 9}, &suprena, state_alloc),
               1L, string("Orly"), 1L, string("short"));
      }
      size_t data_gen_id = 1;
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
    }
    /* Make data file 2 (more of the same) */ {
      TSuprena arena;
      TMockMem mem_layer;
      /* insert data */ {
        TSuprena suprena;
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        /* insert <[int64_t, string, desc<int64_t>, desc<string>]> */
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hello")}, {TDesc<int64_t>(9L), string("This is also a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(2L), TDesc<string>("short"));
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(20L), string("Some form of a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(2L), TDesc<string>("This string should be too long to fit in a core"));
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(27L), string("This is also a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(4L), TDesc<string>("This string should be too long to fit in a core"));
        /* insert <[int64_t, string, int64_t, string]> */
        Insert(mem_layer, ++seq_num, int_str_int_str_idx, TKey(set<int64_t>{1, 3, 9}, &suprena, state_alloc),
               1L, string("Orly"), 2L, string("short"));
      }
      size_t data_gen_id = 2;
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
    }
    /* Make data file 3 (something different) */ {
      TSuprena arena;
      TMockMem mem_layer;
      /* insert data */ {
        TSuprena suprena;
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        /* insert <[int64_t, string, desc<int64_t>, desc<string>]> */
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hello")}, {TDesc<int64_t>(9L), string("This is also a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(5L), TDesc<string>("short"));
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(20L), string("Some form of a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(5L), TDesc<string>("This string should be too long to fit in a core"));
        Insert(mem_layer, ++seq_num, int_str_decint_decstr_idx, TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(27L), string("This is also a longer string")}}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(6L), TDesc<string>("This string should be too long to fit in a core"));
        /* insert <[int64_t, string, desc<int64_t>, string]> */
        Insert(mem_layer, ++seq_num, int_str_decint_str_idx, TKey(set<TDesc<int64_t>>{TDesc<int64_t>(1), TDesc<int64_t>(7), TDesc<int64_t>(20)}, &suprena, state_alloc),
               1L, string("Orly"), TDesc<int64_t>(2L), string("short"));
      }
      size_t data_gen_id = 3;
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, data_gen_id, 20UL, 0U, Medium);
    }
    /* merge them */ {
      TSuprena arena;
      TMergeDataFile merge_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, vector<size_t>{1, 2, 3}, file_id, 4UL, 0U, Low, 16384, 20UL, true, false);
      TReader reader(HERE, mem_engine.GetEngine(), file_id, 4UL);
      TReader::TArena main_arena(&reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TReader::TIndexFile int_str_decint_decstr_idx_file(&reader, int_str_decint_decstr_idx, RealTime);
      TReader::TArena int_str_decint_decstr_idx_arena(&int_str_decint_decstr_idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TReader::TIndexFile int_str_int_str_idx_file(&reader, int_str_int_str_idx, RealTime);
      TReader::TArena int_str_int_str_idx_arena(&int_str_int_str_idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TReader::TIndexFile int_str_decint_str_idx_file(&reader, int_str_decint_str_idx, RealTime);
      TReader::TArena int_str_decint_str_idx_arena(&int_str_decint_str_idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
      TStream<Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::LogicalBlockSize, Orly::Indy::Disk::Util::PhysicalBlockSize, Orly::Indy::Disk::Util::PageCheckedBlock, 0UL> in_stream(HERE, Source::PresentWalk, RealTime, &reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), 0);
      size_t out_offset;
      /* what came from file 1 */
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_int_str_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), 1L, string("short")), &arena, state_alloc), out_offset, in_stream, &int_str_int_str_idx_arena));
      /* what came from file 2 */
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(2L), TDesc<string>("short")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(2L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(4L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_int_str_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), 2L, string("short")), &arena, state_alloc), out_offset, in_stream, &int_str_int_str_idx_arena));
      /* what came from file 3 */
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(5L), TDesc<string>("short")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(5L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(6L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_TRUE(int_str_decint_str_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(2L), string("short")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_str_idx_arena));
      /* what doesn't exist */
      EXPECT_FALSE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(6L), TDesc<string>("short")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_FALSE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(7L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_FALSE(int_str_decint_decstr_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(8L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_decstr_idx_arena));
      EXPECT_FALSE(int_str_decint_str_idx_file.FindInHash(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(3L), string("short")), &arena, state_alloc), out_offset, in_stream, &int_str_decint_str_idx_arena));
      vector<pair<TKey, TKey>> expected_vec;
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(6L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc),
                                TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(27L), string("This is also a longer string")}}, &arena, state_alloc));
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(5L), TDesc<string>("short")), &arena, state_alloc),
                                TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hello")}, {TDesc<int64_t>(9L), string("This is also a longer string")}}, &arena, state_alloc));
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(5L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc),
                                TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(20L), string("Some form of a longer string")}}, &arena, state_alloc));
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(4L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc),
                                TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(27L), string("This is also a longer string")}}, &arena, state_alloc));
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(2L), TDesc<string>("short")), &arena, state_alloc),
                                TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hello")}, {TDesc<int64_t>(9L), string("This is also a longer string")}}, &arena, state_alloc));
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(2L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc),
                                TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(20L), string("Some form of a longer string")}}, &arena, state_alloc));
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("short")), &arena, state_alloc),
                                TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hello")}, {TDesc<int64_t>(9L), string("This is also a longer string")}}, &arena, state_alloc));
      expected_vec.emplace_back(TKey(make_tuple(1L, string("Orly"), TDesc<int64_t>(1L), TDesc<string>("This string should be too long to fit in a core")), &arena, state_alloc),
                                TKey(map<TDesc<int64_t>, string>{{TDesc<int64_t>(30L), string("Hey yo")}, {TDesc<int64_t>(27L), string("This is also a longer string")}}, &arena, state_alloc));
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
    }
    GracefullShutdown();
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(SomeHistory) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    TScheduler scheduler(TScheduler::TPolicy(10, 10, milliseconds(10)));

    Sim::TMemEngine mem_engine(&scheduler,
                               256 /* disk space: 256MB */,
                               256 /* slow disk space: 256MB */,
                               16384 /* page cache slots: 64MB */,
                               1 /* num page lru */,
                               1024 /* block cache slots: 64MB */,
                               1 /* num block lru */);

    Base::TUuid file_id(TUuid::TimeAndMAC);
    TSuprena arena;
    TSequenceNumber seq_num = 0U;
    Base::TUuid int_idx(Base::TUuid::Twister);
    /* data file 1 */ {
      TMockMem mem_layer;
      for (int64_t i = 0; i < 11; i += 2) {
        mem_layer.Insert(TMockUpdate::NewMockUpdate(TUpdate::TOpByKey{ { TIndexKey(int_idx, TKey(make_tuple(i), &arena, state_alloc)), TKey(i * 10, &arena, state_alloc)}, { TIndexKey(int_idx, TKey(make_tuple(i + 1L), &arena, state_alloc)), TKey((i + 1L) * 10, &arena, state_alloc)} }, TKey(&arena), TKey(Base::TUuid(TUuid::Best), &arena, state_alloc), ++seq_num));
        mem_layer.Insert(TMockUpdate::NewMockUpdate(TUpdate::TOpByKey{ { TIndexKey(int_idx, TKey(make_tuple(i), &arena, state_alloc)), TKey(i * 10, &arena, state_alloc)}, { TIndexKey(int_idx, TKey(make_tuple(i + 1L), &arena, state_alloc)), TKey((i + 1L) * 10, &arena, state_alloc)} }, TKey(&arena), TKey(Base::TUuid(TUuid::Best), &arena, state_alloc), ++seq_num));
      }
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, 1UL, 20UL, 0U, RealTime);
    }
    /* data file 2 */ {
      TMockMem mem_layer;
      for (int64_t i = 0; i < 11; i += 2) {
        mem_layer.Insert(TMockUpdate::NewMockUpdate(TUpdate::TOpByKey{ { TIndexKey(int_idx, TKey(make_tuple(i), &arena, state_alloc)), TKey(i * 10, &arena, state_alloc)}, { TIndexKey(int_idx, TKey(make_tuple(i + 1L), &arena, state_alloc)), TKey((i + 1L) * 10, &arena, state_alloc)} }, TKey(&arena), TKey(Base::TUuid(TUuid::Best), &arena, state_alloc), ++seq_num));
        mem_layer.Insert(TMockUpdate::NewMockUpdate(TUpdate::TOpByKey{ { TIndexKey(int_idx, TKey(make_tuple(i), &arena, state_alloc)), TKey(i * 10, &arena, state_alloc)}, { TIndexKey(int_idx, TKey(make_tuple(i + 1L), &arena, state_alloc)), TKey((i + 1L) * 10, &arena, state_alloc)} }, TKey(&arena), TKey(Base::TUuid(TUuid::Best), &arena, state_alloc), ++seq_num));
      }
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, 2UL, 20UL, 0U, RealTime);
    }
    /* merge them */ {
      TMergeDataFile merge_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, vector<size_t>{1UL, 2UL}, file_id, 3UL, 0U, Low, 16384, 20UL, false, false);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(JumpGrowingMainArena) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    stringstream my_str;
    const string zello_str("zello World");
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    Base::TUuid file_id(TUuid::TimeAndMAC);
    Base::TUuid int_idx(Base::TUuid::Twister);
    const size_t start_amt = 4096 * 16;
    while (my_str.str().size() < start_amt) {
      my_str << (rand() % 10);
    }
    const std::string add_str = my_str.str();
    for (size_t i = start_amt; i < 4096 * 16 * 32; i += start_amt) {
      my_str << add_str;
      cout << "Trying i = [" << i << "]" << endl;
      TScheduler scheduler(TScheduler::TPolicy(10, 10, milliseconds(10)));

      Sim::TMemEngine mem_engine(&scheduler,
                                 1024 /* disk space: 1GB */,
                                 512 /* slow disk space: 512MB */,
                                 65536 /* page cache slots: 256MB */,
                                 1 /* num page lru */,
                                 2048 /* block cache slots: 128MB */,
                                 1 /* num block lru */);
      TSuprena arena;
      TSequenceNumber seq_num = 0U;
      /* data file 1 */ {
        TMockMem mem_layer;
        Insert(mem_layer, ++seq_num, int_idx, TKey(my_str.str(), &arena, state_alloc),
                 1L);
        Insert(mem_layer, ++seq_num, int_idx, TKey(zello_str, &arena, state_alloc),
                 2L);
        TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, 1UL, 20UL, 0U, RealTime);
      }
      /* push it through the merger */ {
        TMergeDataFile merge_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, vector<size_t>{1UL}, file_id, 3UL, 0U, Low, 16384, 20UL, false, false);
      }
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #64: TMergeDataFile reports a count of entries written with
   Mutator != Assign. The aggregate is used by TSafeRepo::MergeFiles
   to skip the TFoldDataFile pass when nothing needs folding. */
FIXTURE(NonAssignEntryCount) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);

    Base::TUuid file_id(TUuid::Best);
    Base::TUuid index_id(TUuid::Twister);
    TSuprena arena;

    /* Case A: pure-Assign source. Merge should report 0. */ {
      TMockMem mem_layer;
      TSequenceNumber seq = 0;
      Insert(mem_layer, ++seq, index_id, /*val*/ 10L, /*key*/ 1L);
      Insert(mem_layer, ++seq, index_id, /*val*/ 20L, /*key*/ 2L);
      Insert(mem_layer, ++seq, index_id, /*val*/ 30L, /*key*/ 3L);
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast,
                          &mem_layer, file_id, 1UL, 20UL, 0U, Medium);
      TMergeDataFile merge(mem_engine.GetEngine(), TVolume::TDesc::Fast,
                           file_id, vector<size_t>{1UL}, file_id, 2UL,
                           0U, Low, 16384, 20UL, false, false);
      EXPECT_EQ(merge.GetNumNonAssignEntries(), 0UL);
    }

    /* Case B: source with two Add entries plus an Assign. Merge should
       count the two Adds. */ {
      TMockMem mem_layer;
      TSequenceNumber seq = 0;
      Insert(mem_layer, ++seq, index_id, /*val*/ 100L, /*key*/ 5L);  // Assign
      /* Two Adds via the AddEntry-with-mutator overload. */
      auto u1 = TMockUpdate::NewMockUpdate(
          TUpdate::TOpByKey{},
          TKey(&arena),
          TKey(Base::TUuid(Base::TUuid::Twister), &arena, state_alloc),
          ++seq);
      u1->AddEntry(
          TIndexKey(index_id, TKey(make_tuple(5L), &arena, state_alloc)),
          TKey(int64_t(7), &arena, state_alloc),
          TMutator::Add);
      mem_layer.Insert(u1);
      auto u2 = TMockUpdate::NewMockUpdate(
          TUpdate::TOpByKey{},
          TKey(&arena),
          TKey(Base::TUuid(Base::TUuid::Twister), &arena, state_alloc),
          ++seq);
      u2->AddEntry(
          TIndexKey(index_id, TKey(make_tuple(5L), &arena, state_alloc)),
          TKey(int64_t(3), &arena, state_alloc),
          TMutator::Add);
      mem_layer.Insert(u2);
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast,
                          &mem_layer, file_id, 3UL, 20UL, 0U, Medium);
      TMergeDataFile merge(mem_engine.GetEngine(), TVolume::TDesc::Fast,
                           file_id, vector<size_t>{3UL}, file_id, 4UL,
                           0U, Low, 16384, 20UL, false, false);
      EXPECT_EQ(merge.GetNumNonAssignEntries(), 2UL);
    }

    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #592 helpers. */

/* Insert one update writing several keys of one index, each as (key, value, mutator). */
static void InsertTxn(TMockMem &mem_layer, TSequenceNumber seq_num, const TUuid &index_id,
                      const vector<tuple<int64_t, int64_t, TMutator>> &entries) {
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena), TKey(TUuid(TUuid::Twister), &arena, state_alloc));
  update->SetSequenceNumber(seq_num);
  for (const auto &[key, val, mut] : entries) {
    update->AddEntry(TIndexKey(index_id, TKey(make_tuple(key), &arena, state_alloc)), TKey(val, &arena, state_alloc), mut);
  }
  mem_layer.Insert(TUpdate::CopyUpdate(update.get(), state_alloc));
}

/* The current (key -> value) pairs of one index in a file. */
static map<int64_t, int64_t> ReadCurrent(Sim::TMemEngine &mem_engine, const TUuid &file_id, size_t gen_id, const TUuid &index_id) {
  TReader reader(HERE, mem_engine.GetEngine(), file_id, gen_id);
  TReader::TArena main_arena(&reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
  TReader::TIndexFile idx_file(&reader, index_id, RealTime);
  TReader::TArena idx_arena(&idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
  map<int64_t, int64_t> out;
  void *key_state = alloca(Sabot::State::GetMaxStateSize());
  void *val_state = alloca(Sabot::State::GetMaxStateSize());
  for (TReader::TIndexFile::TKeyCursor csr(&idx_file); csr; ++csr) {
    tuple<int64_t> key;
    int64_t val = 0L;
    Sabot::ToNative(*Sabot::State::TAny::TWrapper((*csr).Key.NewState(&idx_arena, key_state)), key);
    Sabot::ToNative(*Sabot::State::TAny::TWrapper((*csr).Value.NewState(&main_arena, val_state)), val);
    out[get<0>(key)] = val;
  }
  return out;
}

/* A file's update count, and its current and history entry counts for one index. */
static tuple<size_t, size_t, size_t> CountFile(Sim::TMemEngine &mem_engine, const TUuid &file_id, size_t gen_id, const TUuid &index_id) {
  TReader reader(HERE, mem_engine.GetEngine(), file_id, gen_id);
  TReader::TIndexFile idx_file(&reader, index_id, RealTime);
  return make_tuple(reader.GetNumUpdates(), idx_file.GetNumCurKeys(), idx_file.GetNumHistKeys());
}

/* #592: a tail merge keeps a commutative key's history. A `+=` entry is only a delta:
   reads, and the fold pass that follows a merge, add it onto the older entries down to
   the last Assign. The tail pass used to keep only the history that shared a sequence
   number with some current entry, so it dropped a key's earlier `+=` and its Assign
   base, and the key read back as its last delta alone. */
FIXTURE(TailKeepsCommutativeHistory) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    Base::TUuid file_id(TUuid::Best);
    Base::TUuid index_id(TUuid::Twister);
    /* key 42: Assign(10), += 1, += 1. key 7: overwritten once. */ {
      TMockMem mem_layer;
      InsertTxn(mem_layer, 1UL, index_id, {{42L, 10L, TMutator::Assign}});
      InsertTxn(mem_layer, 2UL, index_id, {{42L, 1L, TMutator::Add}});
      InsertTxn(mem_layer, 3UL, index_id, {{42L, 1L, TMutator::Add}});
      InsertTxn(mem_layer, 4UL, index_id, {{7L, 70L, TMutator::Assign}});
      InsertTxn(mem_layer, 5UL, index_id, {{7L, 71L, TMutator::Assign}});
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, 1UL, 20UL, 0U, Medium);
    }
    /* A tail merge, as TSafeRepo::StepMergeDisk now runs it. */ {
      TMergeDataFile merge(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, vector<size_t>{1UL}, file_id, 2UL, 0U, Low, 16384, 20UL, true, false);
      EXPECT_EQ(merge.GetNumNonAssignEntries(), 2UL);
    }
    /* Key 42 keeps both older entries. Key 7 drops its superseded version, and that version's update. */
    const auto counts = CountFile(mem_engine, file_id, 2UL, index_id);
    EXPECT_EQ(get<0>(counts), 4UL);
    EXPECT_EQ(get<1>(counts), 2UL);
    EXPECT_EQ(get<2>(counts), 2UL);
    /* The fold pass that MergeFiles runs next resolves the key to its full sum. */ {
      TFoldDataFile fold(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, 2UL, 3UL, Low, 20UL);
    }
    const auto values = ReadCurrent(mem_engine, file_id, 3UL, index_id);
    EXPECT_EQ(values.size(), 2UL);
    EXPECT_EQ(values.at(42L), 12L);
    EXPECT_EQ(values.at(7L), 71L);
    GracefullShutdown();
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #592: repeated tail merges keep a file's size flat while the same keys are overwritten.
   Each round overwrites every key twice. It also writes two keys in one update, then
   overwrites one of them. Two chains merge the rounds in one at a time: one as a tail
   merge, one as a plain merge. The plain chain keeps every version. The tail chain:
   - keeps each key's current version, plus at most one older version;
   - keeps whole any update that still holds a current value;
   - still records the full sequence range of its inputs. */
FIXTURE(TailBoundsOverwriteHistory) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    Base::TUuid file_id(TUuid::Best);
    Base::TUuid index_id(TUuid::Twister);
    TSuprena arena;
    const int64_t num_keys = 20L, num_rounds = 8L, pair_a = 1000L, pair_b = 1001L;
    const size_t updates_per_round = num_keys * 2L + 2L;
    TSequenceNumber seq = 0UL, last_pair_seq = 0UL;
    size_t tail_gen = 0UL, plain_gen = 0UL;
    vector<size_t> tail_updates, tail_hist, plain_updates, plain_hist;
    for (int64_t round = 1L; round <= num_rounds; ++round) {
      const size_t round_gen = round;
      /* This round's file. */ {
        TMockMem mem_layer;
        for (int64_t pass = 0L; pass < 2L; ++pass) {
          for (int64_t key = 0L; key < num_keys; ++key) {
            InsertTxn(mem_layer, ++seq, index_id, {{key, round * 10L + pass, TMutator::Assign}});
          }
        }
        last_pair_seq = ++seq;
        InsertTxn(mem_layer, last_pair_seq, index_id, {{pair_a, round, TMutator::Assign}, {pair_b, round, TMutator::Assign}});
        InsertTxn(mem_layer, ++seq, index_id, {{pair_a, round + 100L, TMutator::Assign}});
        TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, round_gen, 20UL, 0U, Medium);
      }
      if (round == 1L) {
        tail_gen = plain_gen = round_gen;
        continue;
      }
      const size_t new_tail_gen = 100UL + round, new_plain_gen = 200UL + round;
      /* tail chain */ {
        TMergeDataFile merge(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, vector<size_t>{tail_gen, round_gen}, file_id, new_tail_gen, 0U, Low, 16384, 20UL, true, false);
        /* the whole input range, though the first round's first versions are gone */
        EXPECT_EQ(merge.GetLowestSequence(), 1UL);
        EXPECT_EQ(merge.GetHighestSequence(), seq);
      }
      /* plain chain */ {
        TMergeDataFile merge(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, vector<size_t>{plain_gen, round_gen}, file_id, new_plain_gen, 0U, Low, 16384, 20UL, false, false);
      }
      tail_gen = new_tail_gen;
      plain_gen = new_plain_gen;
      const auto tail_counts = CountFile(mem_engine, file_id, tail_gen, index_id);
      const auto plain_counts = CountFile(mem_engine, file_id, plain_gen, index_id);
      tail_updates.push_back(get<0>(tail_counts));
      tail_hist.push_back(get<2>(tail_counts));
      plain_updates.push_back(get<0>(plain_counts));
      plain_hist.push_back(get<2>(plain_counts));
      /* the plain chain keeps every update ever written */
      EXPECT_EQ(get<0>(plain_counts), updates_per_round * round);
    }
    /* From the first merge on, the tail chain stays the same size while the plain chain grows. */
    for (size_t i = 1UL; i < tail_updates.size(); ++i) {
      EXPECT_EQ(tail_updates[i], tail_updates[0]);
      EXPECT_EQ(tail_hist[i], tail_hist[0]);
      EXPECT_GT(plain_updates[i], plain_updates[i - 1]);
      EXPECT_GT(plain_hist[i], plain_hist[i - 1]);
    }
    /* What the tail chain keeps:
       - every key's version from this round, plus its version from the round before (each
         round's file is merged in whole, so its current versions survive once as history);
       - this round's pair update and the previous round's, whole, because pair_b is still
         current in each;
       - pair_a's newer version from each of those two rounds. */
    EXPECT_EQ(tail_updates.back(), 2UL * (num_keys + 2L));
    /* every key reads its latest value in both chains */
    for (size_t gen : {tail_gen, plain_gen}) {
      const auto values = ReadCurrent(mem_engine, file_id, gen, index_id);
      EXPECT_EQ(values.size(), size_t(num_keys + 2L));
      for (int64_t key = 0L; key < num_keys; ++key) {
        EXPECT_EQ(values.at(key), num_rounds * 10L + 1L);
      }
      EXPECT_EQ(values.at(pair_a), num_rounds + 100L);
      EXPECT_EQ(values.at(pair_b), num_rounds);
    }
    /* Walk the tail chain's update index. Sequence numbers rise, every update that holds a
       current value is there, and the pair update still carries both of its entries. */ {
      TUpdateWalkFile walker(mem_engine.GetEngine(), file_id, tail_gen, 0U);
      TSequenceNumber prev = 0UL;
      size_t walked = 0UL;
      bool saw_pair = false;
      for (; walker; ++walker, ++walked) {
        const TSequenceNumber cur = (*walker).SequenceNumber;
        EXPECT_GT(cur, prev);
        prev = cur;
        if (cur == last_pair_seq) {
          saw_pair = true;
          map<TIndexKey, TKey> entry_map;
          for (const auto &entry : (*walker).EntryVec) {
            entry_map.insert(make_pair(entry.IndexKey, TKey(entry.Op, (*walker).MainArena)));
          }
          EXPECT_EQ(entry_map.size(), 2UL);
          const TIndexKey key_b(index_id, TKey(make_tuple(pair_b), &arena, state_alloc));
          if (EXPECT_TRUE(entry_map.find(key_b) != entry_map.end())) {
            EXPECT_EQ(entry_map.find(key_b)->second, TKey(num_rounds, &arena, state_alloc));
          }
        }
      }
      EXPECT_EQ(walked, tail_updates.back());
      EXPECT_EQ(prev, seq);
      EXPECT_TRUE(saw_pair);
    }
    GracefullShutdown();
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}


/* #592 follow-up helpers. */

/* One entry of a test update. A value of nullopt is a tombstone. */
struct TTestEntry {
  TUuid IndexId;
  int64_t Key;
  optional<int64_t> Val;
  TMutator Mutator;
};

/* Insert one update holding the given entries, which may span indexes. */
static void InsertEntries(TMockMem &mem_layer, TSequenceNumber seq_num, const vector<TTestEntry> &entries) {
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena), TKey(TUuid(TUuid::Twister), &arena, state_alloc));
  update->SetSequenceNumber(seq_num);
  for (const auto &entry : entries) {
    const TKey val = entry.Val ? TKey(*entry.Val, &arena, state_alloc) : TKey(Native::TTombstone::Tombstone, &arena, state_alloc);
    update->AddEntry(TIndexKey(entry.IndexId, TKey(make_tuple(entry.Key), &arena, state_alloc)), val, entry.Mutator);
  }
  mem_layer.Insert(TUpdate::CopyUpdate(update.get(), state_alloc));
}

/* The current entries of one index in a file: key -> (value, or nullopt for a tombstone; mutator). */
static map<int64_t, pair<optional<int64_t>, TMutator>> ReadState(Sim::TMemEngine &mem_engine, const TUuid &file_id, size_t gen_id, const TUuid &index_id) {
  TReader reader(HERE, mem_engine.GetEngine(), file_id, gen_id);
  TReader::TArena main_arena(&reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
  TReader::TIndexFile idx_file(&reader, index_id, RealTime);
  TReader::TArena idx_arena(&idx_file, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
  map<int64_t, pair<optional<int64_t>, TMutator>> out;
  void *key_state = alloca(Sabot::State::GetMaxStateSize());
  void *val_state = alloca(Sabot::State::GetMaxStateSize());
  for (TReader::TIndexFile::TKeyCursor csr(&idx_file); csr; ++csr) {
    tuple<int64_t> key;
    Sabot::ToNative(*Sabot::State::TAny::TWrapper((*csr).Key.NewState(&idx_arena, key_state)), key);
    optional<int64_t> val;
    if (!(*csr).Value.IsTombstone()) {
      int64_t v = 0L;
      Sabot::ToNative(*Sabot::State::TAny::TWrapper((*csr).Value.NewState(&main_arena, val_state)), v);
      val = v;
    }
    out[get<0>(key)] = make_pair(val, (*csr).Mutator);
  }
  return out;
}

/* #592 follow-up: tombstones survive a tail merge, and so does the chain of a `+=` written
   after one.
   - Key 10 is overwritten twice in the older input, then overwritten again and deleted in the
     newer one. It must stay deleted, though its older versions are still in the inputs.
   - Key 20 is assigned 100 in the older input. In the newer one it is deleted, then bumped by
     5 and by 7. The merge must keep its chain down to the tombstone, so it folds to 12; folding
     onto the old 100 would give 112. */
FIXTURE(TailKeepsTombstonesAndChainsAfterThem) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    Base::TUuid file_id(TUuid::Best);
    Base::TUuid idx(TUuid::Twister);
    /* older input */ {
      TMockMem mem_layer;
      InsertEntries(mem_layer, 1UL, {{idx, 10L, 1L, TMutator::Assign}});
      InsertEntries(mem_layer, 2UL, {{idx, 10L, 2L, TMutator::Assign}});
      InsertEntries(mem_layer, 3UL, {{idx, 20L, 100L, TMutator::Assign}});
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, 1UL, 20UL, 0U, Medium);
    }
    /* newer input */ {
      TMockMem mem_layer;
      InsertEntries(mem_layer, 4UL, {{idx, 10L, 3L, TMutator::Assign}});
      InsertEntries(mem_layer, 5UL, {{idx, 10L, nullopt, TMutator::Assign}});
      InsertEntries(mem_layer, 6UL, {{idx, 20L, nullopt, TMutator::Assign}});
      InsertEntries(mem_layer, 7UL, {{idx, 20L, 5L, TMutator::Add}});
      InsertEntries(mem_layer, 8UL, {{idx, 20L, 7L, TMutator::Add}});
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, 2UL, 20UL, 0U, Medium);
    }
    /* As StepMergeDisk runs it: a tail merge that keeps tombstones, then the fold. */ {
      TMergeDataFile merge(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, vector<size_t>{1UL, 2UL}, file_id, 3UL, 0U, Low, 16384, 20UL, true, false);
      EXPECT_GT(merge.GetNumNonAssignEntries(), 0UL);
    }
    {
      TFoldDataFile fold(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, 3UL, 4UL, Low, 20UL);
    }
    const auto state = ReadState(mem_engine, file_id, 4UL, idx);
    EXPECT_EQ(state.size(), 2UL);
    if (EXPECT_TRUE(state.count(10L) == 1UL)) {
      EXPECT_FALSE(state.at(10L).first.has_value());
    }
    if (EXPECT_TRUE(state.count(20L) == 1UL)) {
      EXPECT_TRUE(state.at(20L).first == optional<int64_t>(12L));
      EXPECT_TRUE(state.at(20L).second == TMutator::Assign);
    }
    GracefullShutdown();
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #592 follow-up: the keep rule spans indexes.
   - Update 1 writes key 1 in index X and key 2 in index Y. Key 1 is overwritten later, so only
     key 2 still holds a current value. The tail pass must keep update 1 whole, X entry
     included, because its sequence number is kept for Y's sake.
   - Update 3 writes key 5 in X and key 6 in Y. Both are overwritten later, by separate
     updates, so both of its entries must go together.
   All of this happens inside the older input. The newer input only adds an unrelated key:
   an input's own current versions survive a merge as history. */
FIXTURE(TailKeepsCrossIndexUpdatesWhole) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    Base::TUuid file_id(TUuid::Best);
    Base::TUuid idx_x(TUuid::Twister), idx_y(TUuid::Twister);
    TSuprena arena;
    /* older input */ {
      TMockMem mem_layer;
      InsertEntries(mem_layer, 1UL, {{idx_x, 1L, 10L, TMutator::Assign}, {idx_y, 2L, 20L, TMutator::Assign}});
      InsertEntries(mem_layer, 2UL, {{idx_x, 1L, 11L, TMutator::Assign}});
      InsertEntries(mem_layer, 3UL, {{idx_x, 5L, 50L, TMutator::Assign}, {idx_y, 6L, 60L, TMutator::Assign}});
      InsertEntries(mem_layer, 4UL, {{idx_x, 5L, 51L, TMutator::Assign}});
      InsertEntries(mem_layer, 5UL, {{idx_y, 6L, 61L, TMutator::Assign}});
      InsertEntries(mem_layer, 6UL, {{idx_x, 1L, 12L, TMutator::Assign}});
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, 1UL, 20UL, 0U, Medium);
    }
    /* newer input */ {
      TMockMem mem_layer;
      InsertEntries(mem_layer, 7UL, {{idx_x, 9L, 90L, TMutator::Assign}});
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, 2UL, 20UL, 0U, Medium);
    }
    {
      TMergeDataFile merge(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, vector<size_t>{1UL, 2UL}, file_id, 3UL, 0U, Low, 16384, 20UL, true, false);
    }
    /* latest values in both indexes */
    const auto x = ReadState(mem_engine, file_id, 3UL, idx_x);
    const auto y = ReadState(mem_engine, file_id, 3UL, idx_y);
    EXPECT_TRUE(x.size() == 3UL && x.at(1L).first == optional<int64_t>(12L) && x.at(5L).first == optional<int64_t>(51L) && x.at(9L).first == optional<int64_t>(90L));
    EXPECT_TRUE(y.size() == 2UL && y.at(2L).first == optional<int64_t>(20L) && y.at(6L).first == optional<int64_t>(61L));
    /* Every update the walker finds, by sequence number. The walker's cores live in arenas
       it moves on from, so copy them into ours. */
    map<TSequenceNumber, map<TIndexKey, TKey>> updates;
    for (TUpdateWalkFile walker(mem_engine.GetEngine(), file_id, 3UL, 0U); walker; ++walker) {
      auto &entries = updates[(*walker).SequenceNumber];
      for (const auto &entry : (*walker).EntryVec) {
        entries.insert(make_pair(TIndexKey(entry.IndexKey.GetIndexId(), TKey(&arena, state_alloc, entry.IndexKey.GetKey())),
                                 TKey(&arena, state_alloc, TKey(entry.Op, (*walker).MainArena))));
      }
    }
    /* update 1 is whole: both its X and its Y entry */
    if (EXPECT_TRUE(updates.count(1UL) == 1UL)) {
      const auto &entries = updates.at(1UL);
      EXPECT_EQ(entries.size(), 2UL);
      const TIndexKey key_1(idx_x, TKey(make_tuple(1L), &arena, state_alloc));
      const TIndexKey key_2(idx_y, TKey(make_tuple(2L), &arena, state_alloc));
      EXPECT_TRUE(entries.count(key_1) == 1UL && entries.at(key_1) == TKey(10L, &arena, state_alloc));
      EXPECT_TRUE(entries.count(key_2) == 1UL && entries.at(key_2) == TKey(20L, &arena, state_alloc));
    }
    /* update 3 is gone, both of its entries; so is key 1's middle version (update 2) */
    EXPECT_EQ(updates.count(3UL), 0UL);
    EXPECT_EQ(updates.count(2UL), 0UL);
    /* nothing of update 3 lingers as history in either index */
    const auto counts_x = CountFile(mem_engine, file_id, 3UL, idx_x);
    const auto counts_y = CountFile(mem_engine, file_id, 3UL, idx_y);
    EXPECT_EQ(get<2>(counts_x), 1UL);  // key 1 at update 1
    EXPECT_EQ(get<2>(counts_y), 0UL);
    EXPECT_EQ(get<0>(counts_x), 5UL);  // updates 1, 4, 5, 6 and 7
    GracefullShutdown();
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* #592 follow-up: a `+=` chain that spans the two inputs of a tail merge.
   - Case 1: the Assign base is in the older input. The chain folds to the base plus every
     delta.
   - Case 2: the base is in a third file, outside the merge. The merged chain must stay a
     commutative delta holding every increment from both inputs, so that a read can add it
     onto that base. */
FIXTURE(TailFoldsChainsAcrossInputs) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    Base::TUuid file_id(TUuid::Best);
    Base::TUuid idx(TUuid::Twister);
    auto write_file = [&](size_t gen, const vector<tuple<TSequenceNumber, int64_t, TMutator>> &writes) {
      TMockMem mem_layer;
      for (const auto &[seq, val, mut] : writes) {
        InsertEntries(mem_layer, seq, {{idx, 42L, val, mut}});
      }
      TDataFile data_file(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, gen, 20UL, 0U, Medium);
    };
    auto tail_and_fold = [&](size_t older, size_t newer, size_t merged, size_t folded) {
      {
        TMergeDataFile merge(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, vector<size_t>{older, newer}, file_id, merged, 0U, Low, 16384, 20UL, true, false);
      }
      TFoldDataFile fold(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, merged, folded, Low, 20UL);
    };
    /* case 1: base 10 in the older input, then three += 1 */ {
      write_file(1UL, {{1UL, 10L, TMutator::Assign}, {2UL, 1L, TMutator::Add}});
      write_file(2UL, {{3UL, 1L, TMutator::Add}, {4UL, 1L, TMutator::Add}});
      tail_and_fold(1UL, 2UL, 3UL, 4UL);
      const auto state = ReadState(mem_engine, file_id, 4UL, idx);
      if (EXPECT_TRUE(state.count(42L) == 1UL)) {
        EXPECT_TRUE(state.at(42L).first == optional<int64_t>(13L));
        EXPECT_TRUE(state.at(42L).second == TMutator::Assign);
      }
    }
    /* case 2: base 10 in gen 10, outside the merge; two += 1 in each input */ {
      write_file(10UL, {{11UL, 10L, TMutator::Assign}});
      write_file(11UL, {{12UL, 1L, TMutator::Add}, {13UL, 1L, TMutator::Add}});
      write_file(12UL, {{14UL, 1L, TMutator::Add}, {15UL, 1L, TMutator::Add}});
      tail_and_fold(11UL, 12UL, 13UL, 14UL);
      const auto state = ReadState(mem_engine, file_id, 14UL, idx);
      if (EXPECT_TRUE(state.count(42L) == 1UL)) {
        EXPECT_TRUE(state.at(42L).first == optional<int64_t>(4L));
        EXPECT_TRUE(state.at(42L).second == TMutator::Add);
      }
      /* the base is untouched */
      const auto base = ReadState(mem_engine, file_id, 10UL, idx);
      EXPECT_TRUE(base.at(42L).first == optional<int64_t>(10L));
    }
    GracefullShutdown();
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}
