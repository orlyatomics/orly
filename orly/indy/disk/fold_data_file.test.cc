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

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include <orly/indy/disk/in_file.h>
#include <orly/rt/mutate.h>
#include <orly/sabot/to_native.h>
#include <orly/var/mutation.h>
#include <orly/var/sabot_to_var.h>

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

/* #745 §9.6: the fold as it was before it streamed, kept verbatim (less the pool claim) as the
   reference for the differential test below. It reads the whole source file into groups and
   writes its output through a memory layer of updates. */
namespace Reference {

  /* A TReader that pairs the on-disk file with a disk arena, mirroring
     the same pattern TMergeDataFile uses internally. Anonymous-
     namespace local so it doesn't collide with the TReader inside
     merge_data_file.cc or the test-only one in disk_test.h. */
  class TFoldReader
      : public TReadFile<Indy::Disk::Util::LogicalBlockSize,
                          Indy::Disk::Util::LogicalBlockSize,
                          Indy::Disk::Util::PhysicalBlockSize,
                          Indy::Disk::Util::PageCheckedBlock,
                          true /* ScanAheadAllowed */> {
    NO_COPY(TFoldReader);
    public:
    static constexpr size_t PhysicalCachePageSize = Indy::Disk::Util::PhysicalBlockSize;
    using TArena = TDiskArena<Indy::Disk::Util::LogicalBlockSize,
                               Indy::Disk::Util::LogicalBlockSize,
                               Indy::Disk::Util::PhysicalBlockSize,
                               Indy::Disk::Util::PageCheckedBlock,
                               128, true>;

    TFoldReader(Indy::Disk::Util::TEngine *engine, const TUuid &file_uuid, size_t gen_id, DiskPriority priority)
        : TReadFile(HERE, Source::MergeDataFileScan, engine, file_uuid, priority, gen_id) {
      Arena = std::make_unique<TArena>(this, engine->GetCache<PhysicalCachePageSize>(), priority);
    }

    ~TFoldReader() override {
      Arena.reset();
    }

    /* Promote the protected base interface we need into public access
       (same trick TReader in merge_data_file.cc uses). */
    using TReadFile::ForEachIndex;
    using TReadFile::TIndexFile;

    std::unique_ptr<TArena> Arena;
  };

  /* In-memory rep of one source-file entry. We materialise the Op to a
     fully-typed TVar up front so the source-file arena can be torn down
     before we serialise the output. */
  struct TFoldEntry {
    TSequenceNumber SeqNum;
    TMutator Mutator;
    Var::TVar Op;        // unused for tombstones
    bool IsTombstone;
  };

  Var::TVar OpToVar(const TCore &op, TCore::TArena *arena) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    return Var::ToVar(
        *Sabot::State::TAny::TWrapper(op.NewState(arena, state_alloc)));
  }

  /* Apply the fold algorithm to a single key's entries (already sorted
     SeqNum desc). Returns the output entry list -- usually 1 entry. */
  std::vector<TFoldEntry> FoldOneKey(const std::vector<TFoldEntry> &entries) {
    if (entries.empty()) {
      return {};
    }
    const TFoldEntry &anchor = entries.front();

    // Tombstone anchor: preserve the delete; drop older entries.
    if (anchor.IsTombstone) {
      return {anchor};
    }
    // Assign anchor: keeps "latest assign wins" semantics; drop older.
    if (anchor.Mutator == TMutator::Assign) {
      return {anchor};
    }
    // Non-commutative non-Assign anchor (Sub, Div, Mod, Exp): nothing
    // in-tree currently produces these as on-disk non-Assign entries
    // (session.cc doesn't defer them). Defensively preserve as-is.
    if (!Var::IsDeferSafeCommutative(anchor.Mutator)) {
      return {anchor};
    }

    // Commutative non-Assign: fold older same-mutator entries (and any
    // Assign base) onto the accumulator.
    const TMutator mut = anchor.Mutator;
    Var::TVar acc = anchor.Op;
    bool hit_assign_base = false;  // did this run bottom out at an Assign?
    bool hit_tombstone = false;
    for (size_t i = 1; i < entries.size(); ++i) {
      const TFoldEntry &e = entries[i];
      if (e.IsTombstone) {
        hit_tombstone = true;
        break;  // wipe what's below; the post-tombstone accumulator stands
      }
      if (e.Mutator == TMutator::Assign) {
        // Base. acc-as-RHS folded onto the base value.
        acc = Rt::Mutate(e.Op, mut, acc);
        hit_assign_base = true;
        break;
      }
      if (e.Mutator != mut) {
        // Mixed-mutator chain -- TMutation::Augment rejects these at
        // compose time. Surface the accumulator as-is.
        break;
      }
      acc = Rt::Mutate(acc, mut, e.Op);
    }

    // #227: keep the output COMMUTATIVE when this run was pure commutative
    // (no Assign base, no tombstone below it). A key's increments routinely
    // span several LSM files -- each gets folded independently, and the
    // read-path fold (and later compactions) must keep SUMMING the per-file
    // partials. Promoting a pure-commutative fold to Assign breaks that: with
    // "latest-assign-wins" semantics, a partial fold from one generation
    // shadows/drops the partials from the others (a hot key written N times
    // across ~N/batch files read back as just the last batch). Emitting the
    // resolved value under the SAME commutative mutator keeps it foldable, and
    // costs nothing on the disk side: un-folded commutative entries already
    // live on disk as their mutator (Add/Or/...), so readers already handle
    // it. Only when the run actually bottomed out at an Assign base (or was
    // cut by a tombstone) does Assign correctly shadow everything below it.
    const TMutator out_mut = (hit_assign_base || hit_tombstone) ? TMutator::Assign : mut;
    TFoldEntry out{anchor.SeqNum, out_mut, acc, /*IsTombstone=*/false};
    return {out};
  }


  struct TResult {
    size_t NumKeys = 0UL;
    TSequenceNumber LowestSeq = 0UL;
    TSequenceNumber HighestSeq = 0UL;
    size_t FileLength = 0UL;
  };

  TResult Fold(Indy::Disk::Util::TEngine *engine,
               Indy::Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
               const TUuid &file_uuid,
               size_t source_gen_id,
               size_t dest_gen_id,
               DiskPriority priority,
               size_t temp_file_consol_thresh) {
  TResult result;
  size_t &NumKeys = result.NumKeys;
  // 1. Open the source file.
  TFoldReader reader(engine, file_uuid, source_gen_id, priority);

  // 2. Enumerate indices in the source.
  std::vector<TUuid> index_ids;
  reader.ForEachIndex([&](const TUuid &index_id, size_t /*offset*/) {
    index_ids.push_back(index_id);
  });

  // 3. Output memory layer + arena. We accumulate surviving entries
  //    here, then hand it to TDataFile to serialise.
  TMemoryLayer out_layer(nullptr);
  TSuprena out_arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());

  /* The surviving entries, one update each, in the order the passes below
     produce them. They go into out_layer only at the end, sorted by sequence
     number: the layer's update list is ordered by sequence number and a
     memory layer insert is cheapest in order (#754). A stable sort keeps
     equal sequence numbers in production order, which is the order the
     layer used to end up with. */
  std::vector<std::unique_ptr<TUpdate>> out_updates;

  // 4. Per-index pass: read all entries, group by key, fold each
  //    group, emit surviving entries.
  for (const TUuid &index_id : index_ids) {
    TFoldReader::TIndexFile idx_file(&reader, index_id, priority);
    TFoldReader::TIndexFile::TArena idx_arena(
        &idx_file, engine->GetCache<TFoldReader::PhysicalCachePageSize>(), priority);

    struct TKeyGroup {
      Indy::TKey Key;            // copied into out_arena
      std::vector<TFoldEntry> Entries;
    };
    std::vector<TKeyGroup> groups;
    /* Each group's index in 'groups', by its key. Looking a key up by scanning 'groups' made a
       fold O(entries x keys): a file of a few tens of thousands of keys took minutes of CPU, and a
       graceful stop waits for the merge in flight (#744). TKey hashes by value, and == agrees with
       it, so a key in the index file's arena finds its group's copy in out_arena. */
    std::unordered_map<Indy::TKey, size_t> group_by_key;

    auto find_or_create = [&](const TCore &key_core, TCore::TArena *key_arena) -> TKeyGroup & {
      Indy::TKey probe(key_core, key_arena);
      const auto iter = group_by_key.find(probe);
      if (iter != group_by_key.end()) {
        return groups[iter->second];
      }
      groups.emplace_back();
      auto &g = groups.back();
      g.Key = Indy::TKey(&out_arena, state_alloc, probe);
      group_by_key.emplace(g.Key, groups.size() - 1);
      return g;
    };

    // 4a. Walk current keys.
    for (TFoldReader::TIndexFile::TKeyCursor csr(&idx_file); csr; ++csr) {
      const TFoldReader::TIndexFile::TKeyItem &item = *csr;
      TKeyGroup &g = find_or_create(item.Key, &idx_arena);
      TFoldEntry e;
      e.SeqNum = item.SeqNum;
      e.Mutator = item.Mutator;
      e.IsTombstone = item.Value.IsTombstone();
      if (!e.IsTombstone) {
        e.Op = OpToVar(item.Value, reader.Arena.get());
      }
      g.Entries.push_back(std::move(e));
    }

    // 4b. Walk history keys.
    for (TFoldReader::TIndexFile::THistoryKeyCursor csr(&idx_file); csr; ++csr) {
      const TFoldReader::TIndexFile::THistoryKeyItem &item = *csr;
      TKeyGroup &g = find_or_create(item.Key, &idx_arena);
      TFoldEntry e;
      e.SeqNum = item.SeqNum;
      e.Mutator = item.Mutator;
      e.IsTombstone = item.Value.IsTombstone();
      if (!e.IsTombstone) {
        e.Op = OpToVar(item.Value, reader.Arena.get());
      }
      g.Entries.push_back(std::move(e));
    }

    // 4c. Per-group: sort SeqNum desc, fold, emit.
    for (auto &g : groups) {
      std::sort(g.Entries.begin(), g.Entries.end(),
                [](const TFoldEntry &a, const TFoldEntry &b) {
                  return a.SeqNum > b.SeqNum;
                });
      auto out_entries = FoldOneKey(g.Entries);
      for (const TFoldEntry &e : out_entries) {
        Indy::TKey op_key;
        if (e.IsTombstone) {
          op_key = Indy::TKey(Native::TTombstone::Tombstone, &out_arena, state_alloc);
        } else {
          op_key = Indy::TKey(
              &out_arena,
              Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc, e.Op)).get());
        }
        const TIndexKey index_key(index_id, g.Key);
        /* Build the entry under its OWN mutator. NewUpdate(op_by_key, ...)
           tags every entry Assign, which is wrong for a folded run that
           stays commutative (FoldOneKey returns out.Mutator == the source
           commutative mutator when the run never bottomed out at an Assign
           base or tombstone). Emitting such a run as Assign would make it
           cap the read-path fold (latest-assign-wins), shadowing the same
           key's partials in the other LSM files -- a hot key written N
           times across many files would read back as just one folded
           group. So Assign entries go through the TOpByKey ctor (existing
           semantics) and non-Assign entries go through AddEntry post-
           construction, mirroring TRepo::GetLowestUpdate. */
        TUpdate::TOpByKey op_by_key;
        if (e.Mutator == TMutator::Assign) {
          op_by_key.insert(std::make_pair(index_key, op_key));
        }
        auto update = TUpdate::NewUpdate(
            op_by_key,
            Indy::TKey(&out_arena),
            Indy::TKey(TUuid(TUuid::Twister), &out_arena, state_alloc));
        if (e.Mutator != TMutator::Assign) {
          update->AddEntry(index_key, op_key, e.Mutator);
        }
        update->SetSequenceNumber(e.SeqNum);
        out_updates.emplace_back(TUpdate::CopyUpdate(update.get(), state_alloc));

        ++NumKeys;
      }
    }
  }

  std::stable_sort(out_updates.begin(), out_updates.end(),
                   [](const std::unique_ptr<TUpdate> &a, const std::unique_ptr<TUpdate> &b) {
                     return a->GetSequenceNumber() < b->GetSequenceNumber();
                   });
  for (auto &update : out_updates) {
    out_layer.Insert(update.release());
  }
  out_updates.clear();

  // 5. Serialise the folded memory layer at dest_gen_id. Record the
  //    source's whole sequence range, not just that of the entries the fold
  //    kept: the output replaces the merge's inputs, and a reload drops a
  //    file only when its range lies inside another's. With the narrower
  //    range, an input that a crash left behind would load next to the
  //    output and its `+=` deltas would be counted twice (#618). The source
  //    (TMergeDataFile's output) already records its inputs' whole range.
  std::optional<std::pair<TSequenceNumber, TSequenceNumber>> source_range;
  /* find the source's file-map entry */ {
    std::vector<TFileObj> file_vec;
    engine->AppendFileGenSet(file_uuid, file_vec);
    for (const auto &file : file_vec) {
      if (file.GenId == source_gen_id) {
        source_range = std::make_pair(file.LowestSeq, file.HighestSeq);
        break;
      }
    }
  }
  TDataFile data_file(engine, storage_speed, &out_layer, file_uuid,
                      dest_gen_id, temp_file_consol_thresh, 0U, priority, source_range);
  result.LowestSeq = data_file.GetLowestSequence();
  result.HighestSeq = data_file.GetHighestSequence();
  result.FileLength = data_file.GetFileLength();
  return result;
  }

}  // Reference

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

/* One entry of a file, as the differential test compares them. Value is meaningless for a
   tombstone. */
struct TDumpEntry {
  int64_t Key;
  TSequenceNumber SeqNum;
  TMutator Mutator;
  bool IsTombstone;
  int64_t Value;
  size_t NumHistKeys;  // 0 for a history entry
  bool operator==(const TDumpEntry &that) const {
    return Key == that.Key && SeqNum == that.SeqNum && Mutator == that.Mutator && IsTombstone == that.IsTombstone &&
           (IsTombstone || Value == that.Value) && NumHistKeys == that.NumHistKeys;
  }
};

/* Every current entry of each index, in file order, each followed by its history run. */
static std::vector<TDumpEntry>
DumpFile(Sim::TMemEngine &mem_engine, const TUuid &file_id, size_t gen_id, const std::vector<TUuid> &index_ids) {
  TReader reader(HERE, mem_engine.GetEngine(), file_id, gen_id);
  TReader::TArena main_arena(&reader,
      mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
  std::vector<TDumpEntry> out;
  for (const TUuid &index_id : index_ids) {
    bool found = false;
    reader.ForEachIndex([&](const TUuid &id, size_t) { found = found || id == index_id; });
    if (!found) {
      continue;
    }
    TReader::TIndexFile idx_file(&reader, index_id, RealTime);
    TReader::TArena idx_arena(&idx_file,
        mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
    auto read = [&](const TCore &key, const TCore &value, TSequenceNumber seq, TMutator mutator, size_t num_hist) {
      void *k_state = alloca(Sabot::State::GetMaxStateSize());
      void *v_state = alloca(Sabot::State::GetMaxStateSize());
      std::tuple<int64_t> kt;
      Sabot::ToNative(*Sabot::State::TAny::TWrapper(key.NewState(&idx_arena, k_state)), kt);
      TDumpEntry e{std::get<0>(kt), seq, mutator, value.IsTombstone(), 0L, num_hist};
      if (!e.IsTombstone) {
        Sabot::ToNative(*Sabot::State::TAny::TWrapper(value.NewState(&main_arena, v_state)), e.Value);
      }
      out.push_back(e);
    };
    for (TReader::TIndexFile::TKeyCursor csr(&idx_file); csr; ++csr) {
      const auto &item = *csr;
      read(item.Key, item.Value, item.SeqNum, item.Mutator, item.NumHistKeys);
      if (item.NumHistKeys) {
        TReader::TIndexFile::THistoryKeyCursor hist(&idx_file, item.OffsetOfHistKeys / TData::KeyHistorySize);
        for (size_t i = 0UL; i < item.NumHistKeys && hist; ++i, ++hist) {
          read((*hist).Key, (*hist).Value, (*hist).SeqNum, (*hist).Mutator, 0UL);
        }
      }
    }
  }
  return out;
}

/* #745 §9.6: the streaming fold writes what the fold it replaced wrote. Randomized source files
   (several indexes, Assigns, tombstones, `+=` and `*=` chains with and without an Assign base,
   updates of one and of two entries) are folded by both, and the outputs compared entry by entry
   along with the key count and the recorded sequence range. Only the update ids, which are
   random, may differ. */
FIXTURE(StreamingFoldMatchesReference) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
    size_t compared = 0UL, folded_runs = 0UL;
    for (uint32_t trial = 0U; trial < 60U; ++trial) {
      std::mt19937 rng(trial);
      TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
      Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
      const TUuid file_id(TUuid::Best);
      std::vector<TUuid> index_ids;
      const size_t num_indexes = 1UL + rng() % 3UL;
      for (size_t i = 0UL; i < num_indexes; ++i) {
        index_ids.emplace_back(TUuid::Twister);
      }
      std::sort(index_ids.begin(), index_ids.end());
      const int64_t num_keys = 1L + static_cast<int64_t>(rng() % (trial < 20U ? 4U : 40U));
      const size_t num_updates = 1UL + rng() % (trial < 20U ? 12UL : 400UL);
      /* the source file */ {
        TMockMem mem_layer;
        for (size_t u = 0UL; u < num_updates; ++u) {
          TSuprena arena;
          auto update = TUpdate::NewUpdate(TUpdate::TOpByKey{}, TKey(&arena), TKey(TUuid(TUuid::Twister), &arena, state_alloc));
          update->SetSequenceNumber(u + 1UL);
          const size_t num_entries = (rng() % 4U == 0U) ? 2UL : 1UL;
          std::vector<std::pair<size_t, int64_t>> used;
          for (size_t e = 0UL; e < num_entries; ++e) {
            const size_t index = rng() % num_indexes;
            const int64_t key = static_cast<int64_t>(rng() % static_cast<uint32_t>(num_keys));
            if (std::find(used.begin(), used.end(), std::make_pair(index, key)) != used.end()) {
              continue;
            }
            used.emplace_back(index, key);
            const uint32_t pick = rng() % 20U;
            /* mostly `+=`, so long chains form; `*=` by small factors so nothing overflows */
            TMutator mutator = TMutator::Add;
            int64_t value = static_cast<int64_t>(rng() % 7U) - 3L;
            bool tombstone = false;
            if (pick < 3U) {
              mutator = TMutator::Assign;
              value = static_cast<int64_t>(rng() % 100U);
            } else if (pick < 4U) {
              mutator = TMutator::Assign;
              tombstone = true;
            } else if (pick < 6U) {
              mutator = TMutator::Mult;
              value = (rng() % 2U) ? -1L : 1L;
            }
            const TIndexKey index_key(index_ids[index], TKey(std::make_tuple(key), &arena, state_alloc));
            if (tombstone) {
              update->AddEntry(index_key, TKey(Native::TTombstone::Tombstone, &arena, state_alloc), mutator);
            } else {
              update->AddEntry(index_key, TKey(value, &arena, state_alloc), mutator);
            }
          }
          mem_layer.Insert(TUpdate::CopyUpdate(update.get(), state_alloc));
        }
        TDataFile(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, 1UL, 20UL, 0U, Medium);
      }
      const Reference::TResult expected = Reference::Fold(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, 1UL, 2UL, Medium, 20UL);
      TFoldDataFile actual(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, 1UL, 3UL, Medium, 20UL);
      EXPECT_EQ(actual.GetNumKeys(), expected.NumKeys);
      EXPECT_EQ(actual.GetLowestSequence(), expected.LowestSeq);
      EXPECT_EQ(actual.GetHighestSequence(), expected.HighestSeq);
      EXPECT_EQ(actual.GetFileLength(), expected.FileLength);
      const auto source = DumpFile(mem_engine, file_id, 1UL, index_ids);
      const auto want = DumpFile(mem_engine, file_id, 2UL, index_ids);
      const auto got = DumpFile(mem_engine, file_id, 3UL, index_ids);
      folded_runs += source.size() - want.size();
      if (EXPECT_EQ(got.size(), want.size())) {
        for (size_t i = 0UL; i < got.size(); ++i) {
          if (!EXPECT_TRUE(got[i] == want[i])) {
            std::cerr << "trial " << trial << " entry " << i << ": key " << got[i].Key << " seq " << got[i].SeqNum
                      << " value " << got[i].Value << " vs key " << want[i].Key << " seq " << want[i].SeqNum
                      << " value " << want[i].Value << std::endl;
            break;
          }
          ++compared;
        }
      }
    }
    /* the inputs exercised the fold, not just copied keys through */
    EXPECT_GT(folded_runs, 1000UL);
    EXPECT_GT(compared, 500UL);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}

/* The id of each update in a file's update index, by sequence number. */
static std::map<TSequenceNumber, TUuid> ReadUpdateIds(Sim::TMemEngine &mem_engine, const TUuid &file_id, size_t gen_id) {
  struct TUpdateReader : public TReader {
    using TReader::TReader;
    using TReader::TReadFile::GetByteOffsetOfUpdateIndex;
  };
  TUpdateReader reader(HERE, mem_engine.GetEngine(), file_id, gen_id);
  TDataFile::TDataInStream<0UL> in_stream(HERE, Source::MergeDataFileOther, RealTime, &reader,
      mem_engine.GetEngine()->GetCache<TDataFile::TDataInStream<0UL>::PhysicalCachePageSize>(),
      reader.GetByteOffsetOfUpdateIndex());
  TReader::TArena main_arena(&reader, mem_engine.GetEngine()->GetCache<TReader::PhysicalCachePageSize>(), RealTime);
  std::map<TSequenceNumber, TUuid> out;
  for (size_t u = 0UL; u < reader.GetNumUpdates(); ++u) {
    TSequenceNumber seq_num;
    TCore meta_core, id_core;
    in_stream.Read(seq_num);
    in_stream.Read(&meta_core, sizeof(meta_core));
    in_stream.Read(&id_core, sizeof(id_core));
    in_stream.Skip(sizeof(size_t) * 2UL);
    void *state = alloca(Sabot::State::GetMaxStateSize());
    TUuid id;
    Sabot::ToNative(*Sabot::State::TAny::TWrapper(id_core.NewState(&main_arena, state)), id);
    out[seq_num] = id;
  }
  return out;
}

/* #745 stage 2: a fold with a retention window keeps every version newer than it, as written and with its update's
   id, and folds the rest to their state as of the window's start. A long `+=` run on a known value gets an Assign
   keyframe every KeyframeInterval deltas, so reading any version replays at most that many; replaying the output
   gives the source's value at every version in the window. */
FIXTURE(RetainingFoldKeepsTheWindowWithKeyframes) {
  TFiberTestRunner runner([](std::mutex &mut, std::condition_variable &cond, bool &fin, Fiber::TRunner::TRunnerCons &) {
    TScheduler scheduler(TScheduler::TPolicy(4, 10, milliseconds(10)));
    Sim::TMemEngine mem_engine(&scheduler, 256, 256, 16384, 1, 1024, 1);
    TUuid file_id(TUuid::Best);
    TUuid index_id(TUuid::Twister);
    const size_t src_gen = 1, dst_gen = 2;
    /* Key 42: Assign(10) at 1, then `+= 1` at 2..151. Key 7: Assign(1) at 152, Assign(2) at 153. */
    constexpr TSequenceNumber last_add = 151UL, retain_from = 50UL;
    {
      TMockMem mem_layer;
      Insert(mem_layer, 1UL, index_id, 10L, 42L);
      for (TSequenceNumber seq = 2UL; seq <= last_add; ++seq) {
        InsertMut(mem_layer, seq, index_id, 42L, 1L, TMutator::Add);
      }
      Insert(mem_layer, 152UL, index_id, 1L, 7L);
      Insert(mem_layer, 153UL, index_id, 2L, 7L);
      TDataFile(mem_engine.GetEngine(), TVolume::TDesc::Fast, &mem_layer, file_id, src_gen, 20UL, 0U, Medium);
    }
    TFoldDataFile fold(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, src_gen, dst_gen, Medium, 20UL, retain_from);
    EXPECT_EQ(fold.GetNumKeys(), 2UL);
    const auto entries = DumpFile(mem_engine, file_id, dst_gen, {index_id});
    std::map<int64_t, std::vector<TDumpEntry>> by_key;
    for (const auto &e : entries) {
      by_key[e.Key].push_back(e);
    }
    /* Key 42: the base at 50 (10 + 49), then 101 deltas, the 64th of them (at 114) a keyframe of 10 + 113. */
    const auto &hot = by_key[42L];
    EXPECT_EQ(hot.size(), 1UL + (last_add - retain_from));
    std::vector<TSequenceNumber> assigns;
    for (const auto &e : hot) {
      if (e.Mutator == TMutator::Assign) {
        assigns.push_back(e.SeqNum);
        EXPECT_EQ(e.Value, 10L + static_cast<int64_t>(e.SeqNum) - 1L);
      } else {
        EXPECT_EQ(e.Value, 1L);
      }
    }
    EXPECT_TRUE(assigns == std::vector<TSequenceNumber>({retain_from + TFoldDataFile::KeyframeInterval, retain_from}));
    EXPECT_EQ(by_key[7L].size(), 2UL);
    /* Replay key 42 at every version from the window's start, as a read does: from the newest entry at or below the
       version back to the first Assign. */
    size_t max_replay = 0UL;
    for (TSequenceNumber at = retain_from; at <= last_add; ++at) {
      int64_t value = 0L;
      size_t replayed = 0UL;
      for (const auto &e : hot) {  // newest first
        if (e.SeqNum > at) {
          continue;
        }
        value += e.Value;
        ++replayed;
        if (e.Mutator == TMutator::Assign) {
          break;
        }
      }
      max_replay = std::max(max_replay, replayed);
      if (!EXPECT_EQ(value, 10L + static_cast<int64_t>(at) - 1L)) {
        break;
      }
    }
    EXPECT_LE(max_replay, TFoldDataFile::KeyframeInterval);
    /* Every version in the window keeps its update's id; the folded base gets a fresh one. */
    const auto src_ids = ReadUpdateIds(mem_engine, file_id, src_gen);
    const auto dst_ids = ReadUpdateIds(mem_engine, file_id, dst_gen);
    size_t kept_ids = 0UL;
    for (const auto &[seq, id] : dst_ids) {
      if (seq > retain_from) {
        kept_ids += (src_ids.at(seq) == id) ? 1UL : 0UL;
      } else {
        EXPECT_FALSE(src_ids.at(seq) == id);
      }
    }
    EXPECT_EQ(kept_ids, 153UL - retain_from);
    /* Without a window the same source folds to one entry a key, as before. */
    TFoldDataFile plain(mem_engine.GetEngine(), TVolume::TDesc::Fast, file_id, src_gen, 3UL, Medium, 20UL);
    EXPECT_EQ(DumpFile(mem_engine, file_id, 3UL, {index_id}).size(), 2UL);
    std::lock_guard<std::mutex> lock(mut);
    fin = true;
    cond.notify_one();
  });
}
