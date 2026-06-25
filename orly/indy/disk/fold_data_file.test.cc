/* <orly/indy/disk/fold_data_file.test.cc>

   Unit test for <orly/indy/disk/fold_data_file.h>.

   Verifies phase 4 of #49: same-mutator commutative runs in a data
   file get folded into a single Assign(value) entry at compaction
   time, bringing read amplification back to O(1) per key.

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

#include <orly/indy/disk/fold_data_file.h>

#include <base/scheduler.h>
#include <orly/indy/disk/data_file.h>
#include <orly/indy/disk/disk_test.h>
#include <orly/indy/disk/read_file.h>
#include <orly/indy/disk/sim/mem_engine.h>
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

/* Insert a single-entry update with a specified mutator into a
   TMockMem. Mirrors disk_test.h's Insert<TArgs...> but lets the test
   put non-Assign entries on disk (which the existing helper doesn't
   support, since it goes through TOpByKey which always tags Assign). */
static void InsertMut(TMockMem &mem_layer, TSequenceNumber seq_num,
                      const TUuid &idx_id, int64_t key, int64_t val, TMutator mut) {
  TSuprena arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  auto update = TUpdate::NewUpdate(
      TUpdate::TOpByKey{},        // empty: no Assign entries
      TKey(&arena),
      TKey(TUuid(TUuid::Twister), &arena, state_alloc));
  update->SetSequenceNumber(seq_num);
  update->AddEntry(
      TIndexKey(idx_id, TKey(std::make_tuple(key), &arena, state_alloc)),
      TKey(val, &arena, state_alloc),
      mut);
  mem_layer.Insert(TUpdate::CopyUpdate(update.get(), state_alloc));
}

/* Read back every (key, value) pair in the file's index and return as
   a sorted vector keyed by the int64_t key (matches our test inserts
   which use int64_t keys). */
static std::vector<std::pair<int64_t, int64_t>>
ReadInts(Sim::TMemEngine &mem_engine, const TUuid &file_id, size_t gen_id, const TUuid &index_id) {
  TReader reader(HERE, mem_engine.GetEngine(), file_id, gen_id);
  TReader::TArena main_arena(&reader,
      mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
  TReader::TIndexFile idx_file(&reader, index_id, RealTime);
  TReader::TArena idx_arena(&idx_file,
      mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
  std::vector<std::pair<int64_t, int64_t>> out;
  for (TReader::TIndexFile::TKeyCursor csr(&idx_file); csr; ++csr) {
    const auto &item = *csr;
    void *k_state = alloca(Sabot::State::GetMaxStateSize());
    void *v_state = alloca(Sabot::State::GetMaxStateSize());
    std::tuple<int64_t> kt;
    int64_t val_int = 0;
    Sabot::ToNative(*Sabot::State::TAny::TWrapper(item.Key.NewState(&idx_arena, k_state)), kt);
    Sabot::ToNative(*Sabot::State::TAny::TWrapper(item.Value.NewState(&main_arena, v_state)), val_int);
    out.emplace_back(std::get<0>(kt), val_int);
  }
  std::sort(out.begin(), out.end());
  return out;
}

/* Read the per-current-key mutators out of a folded file (#227: a
   pure-commutative run must stay under its own mutator, not collapse to
   Assign, or it would cap the read-path fold across LSM files). */
static std::vector<TMutator>
ReadMutators(Sim::TMemEngine &mem_engine, const TUuid &file_id, size_t gen_id, const TUuid &index_id) {
  TReader reader(HERE, mem_engine.GetEngine(), file_id, gen_id);
  TReader::TArena main_arena(&reader,
      mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
  TReader::TIndexFile idx_file(&reader, index_id, RealTime);
  TReader::TArena idx_arena(&idx_file,
      mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
  std::vector<TMutator> out;
  for (TReader::TIndexFile::TKeyCursor csr(&idx_file); csr; ++csr) {
    out.push_back((*csr).Mutator);
  }
  return out;
}

FIXTURE(FoldsAddsOntoAssignBase) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);

    TUuid file_id(TUuid::Best);
    TUuid index_id(TUuid::Twister);
    const size_t src_gen = 1, dst_gen = 2;

    /* Build a source file with: Assign(10), Add(5), Add(3) on key 42. */
    {
      TMockMem mem_layer;
      TSequenceNumber seq = 0;
      Insert(mem_layer, ++seq, index_id, /*val*/ 10L, /*key tuple*/ 42L);  // Assign(10)
      InsertMut(mem_layer, ++seq, index_id, /*key*/ 42L, /*val*/ 5L, TMutator::Add);
      InsertMut(mem_layer, ++seq, index_id, /*key*/ 42L, /*val*/ 3L, TMutator::Add);
      TDataFile(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, src_gen, 20UL, 0U, Medium);
    }

    /* Fold pass. */
    TFoldDataFile fold(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, src_gen, dst_gen, Medium, 20UL);
    /* Exactly one resulting key per group. */
    if (EXPECT_EQ(fold.GetNumKeys(), 1UL)) {}

    /* The folded file should have a single Assign(18) entry on key 42. */
    auto pairs = ReadInts(mem_engine, file_id, dst_gen, index_id);
    if (EXPECT_EQ(pairs.size(), 1UL)) {
      EXPECT_EQ(pairs[0].first, 42L);
      EXPECT_EQ(pairs[0].second, 18L);   /* 10 + 5 + 3 */
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(FoldsAddsWithoutAssignBase) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);

    TUuid file_id(TUuid::Best);
    TUuid index_id(TUuid::Twister);
    const size_t src_gen = 1, dst_gen = 2;

    /* Build a source file with three Adds on key 7, no Assign base. */
    {
      TMockMem mem_layer;
      TSequenceNumber seq = 0;
      InsertMut(mem_layer, ++seq, index_id, 7L, 1L, TMutator::Add);
      InsertMut(mem_layer, ++seq, index_id, 7L, 2L, TMutator::Add);
      InsertMut(mem_layer, ++seq, index_id, 7L, 4L, TMutator::Add);
      TDataFile(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, src_gen, 20UL, 0U, Medium);
    }

    TFoldDataFile fold(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, src_gen, dst_gen, Medium, 20UL);
    EXPECT_EQ(fold.GetNumKeys(), 1UL);

    /* No Assign base + commutative-mutator identity = 0 for Add, so the
       folded value should be 1+2+4 = 7. #227: a pure-Add run must be
       materialised back under its OWN mutator (Add), NOT Assign -- a hot
       key's increments span several LSM files, each folded independently,
       and an Assign(partial) here would cap the read-path fold and shadow
       the other files' partials. */
    auto pairs = ReadInts(mem_engine, file_id, dst_gen, index_id);
    if (EXPECT_EQ(pairs.size(), 1UL)) {
      EXPECT_EQ(pairs[0].first, 7L);
      EXPECT_EQ(pairs[0].second, 7L);
    }
    auto muts = ReadMutators(mem_engine, file_id, dst_gen, index_id);
    if (EXPECT_EQ(muts.size(), 1UL)) {
      EXPECT_TRUE(muts[0] == TMutator::Add);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(PureAssignDropsOlder) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);

    TUuid file_id(TUuid::Best);
    TUuid index_id(TUuid::Twister);
    const size_t src_gen = 1, dst_gen = 2;

    /* Three Assigns on key 99: the newest one (100) shadows the others. */
    {
      TMockMem mem_layer;
      TSequenceNumber seq = 0;
      Insert(mem_layer, ++seq, index_id, /*val*/ 10L, /*key*/ 99L);
      Insert(mem_layer, ++seq, index_id, /*val*/ 50L, /*key*/ 99L);
      Insert(mem_layer, ++seq, index_id, /*val*/ 100L, /*key*/ 99L);
      TDataFile(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, src_gen, 20UL, 0U, Medium);
    }

    TFoldDataFile fold(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, src_gen, dst_gen, Medium, 20UL);
    EXPECT_EQ(fold.GetNumKeys(), 1UL);

    auto pairs = ReadInts(mem_engine, file_id, dst_gen, index_id);
    if (EXPECT_EQ(pairs.size(), 1UL)) {
      EXPECT_EQ(pairs[0].first, 99L);
      EXPECT_EQ(pairs[0].second, 100L);   /* newest assign wins */
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

FIXTURE(MixedKeysIndependent) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);

    TUuid file_id(TUuid::Best);
    TUuid index_id(TUuid::Twister);
    const size_t src_gen = 1, dst_gen = 2;

    /* Mix: key 1 has Assign+Adds, key 2 only Adds, key 3 only one Assign. */
    {
      TMockMem mem_layer;
      TSequenceNumber seq = 0;
      Insert(mem_layer, ++seq, index_id, /*val*/ 100L, /*key*/ 1L);
      InsertMut(mem_layer, ++seq, index_id, 1L, 5L, TMutator::Add);
      InsertMut(mem_layer, ++seq, index_id, 2L, 10L, TMutator::Add);
      InsertMut(mem_layer, ++seq, index_id, 2L, 20L, TMutator::Add);
      Insert(mem_layer, ++seq, index_id, /*val*/ 42L, /*key*/ 3L);
      InsertMut(mem_layer, ++seq, index_id, 1L, 7L, TMutator::Add);
      TDataFile(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, src_gen, 20UL, 0U, Medium);
    }

    TFoldDataFile fold(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, src_gen, dst_gen, Medium, 20UL);
    EXPECT_EQ(fold.GetNumKeys(), 3UL);

    auto pairs = ReadInts(mem_engine, file_id, dst_gen, index_id);
    if (EXPECT_EQ(pairs.size(), 3UL)) {
      EXPECT_EQ(pairs[0].first, 1L);
      EXPECT_EQ(pairs[0].second, 112L);   /* 100 + 5 + 7 */
      EXPECT_EQ(pairs[1].first, 2L);
      EXPECT_EQ(pairs[1].second, 30L);    /* 0 + 10 + 20 */
      EXPECT_EQ(pairs[2].first, 3L);
      EXPECT_EQ(pairs[2].second, 42L);
    }
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}
