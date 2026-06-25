/* <orly/indy/disk/fold_data_file.cc>

   Implements <orly/indy/disk/fold_data_file.h>. */

#include <orly/indy/disk/fold_data_file.h>

#include <algorithm>
#include <memory>
#include <vector>

#include <orly/indy/memory_layer.h>
#include <orly/indy/update.h>
#include <orly/rt/mutate.h>
#include <orly/sabot/to_native.h>
#include <orly/var/mutation.h>
#include <orly/var/sabot_to_var.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Atom;
using namespace Orly::Indy;
using namespace Orly::Indy::Disk;

namespace {

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

}  // namespace

namespace Orly { namespace Indy { namespace Disk {

TFoldDataFile::TFoldDataFile(Indy::Disk::Util::TEngine *engine,
                             Indy::Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                             const TUuid &file_uuid,
                             size_t source_gen_id,
                             size_t dest_gen_id,
                             DiskPriority priority,
                             size_t temp_file_consol_thresh)
    : NumKeys(0UL),
      LowestSeq(0UL),
      HighestSeq(0UL) {

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

    auto find_or_create = [&](const TCore &key_core, TCore::TArena *key_arena) -> TKeyGroup & {
      Indy::TKey probe(key_core, key_arena);
      for (auto &g : groups) {
        if (g.Key == probe) {
          return g;
        }
      }
      groups.emplace_back();
      auto &g = groups.back();
      g.Key = Indy::TKey(&out_arena, state_alloc, probe);
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
        out_layer.Insert(TUpdate::CopyUpdate(update.get(), state_alloc));

        if (LowestSeq == 0UL || e.SeqNum < LowestSeq) LowestSeq = e.SeqNum;
        if (e.SeqNum > HighestSeq) HighestSeq = e.SeqNum;
        ++NumKeys;
      }
    }
  }

  // 5. Serialise the folded memory layer at dest_gen_id.
  TDataFile data_file(engine, storage_speed, &out_layer, file_uuid,
                      dest_gen_id, temp_file_consol_thresh, 0U, priority);
}

}}}  // namespace Orly::Indy::Disk
