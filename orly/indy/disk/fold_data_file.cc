/* <orly/indy/disk/fold_data_file.cc>

   Implements <orly/indy/disk/fold_data_file.h>. */

#include <orly/indy/disk/fold_data_file.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <functional>
#include <utility>
#include <vector>

#include <orly/indy/disk/in_file.h>
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

  /* 2. Enumerate indices in the source, in the order a memory layer keeps them (by index id), which
        is the order TDataFile expects its entries in. */
  std::vector<TUuid> index_ids;
  reader.ForEachIndex([&](const TUuid &index_id, size_t /*offset*/) {
    index_ids.push_back(index_id);
  });
  std::sort(index_ids.begin(), index_ids.end());

  /* 3. The surviving entries, one per key, and the arena holding their keys, values and update
        ids. #745 §9.6: the fold used to build a TUpdate and a TEntry per key, from the update pools,
        and insert them into a memory layer for TDataFile to write. A big enough file could never
        fold (#627), and a fold that could claimed pool space a POV might need (#858). Now it walks
        the source key by key, holding only one key's versions at a time, and hands TDataFile its
        output directly; nothing comes from the update pools. */
  TSuprena out_arena;
  void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
  std::vector<TDataFile::TEntryView> out_entries;
  /* The empty metadata every folded entry carries. */
  const TCore empty_metadata = Indy::TKey(&out_arena).GetCore();

  // 4. Per index: fold each key's current entry and history, in key order.
  for (const TUuid &index_id : index_ids) {
    TFoldReader::TIndexFile idx_file(&reader, index_id, priority);
    TFoldReader::TIndexFile::TArena idx_arena(
        &idx_file, engine->GetCache<TFoldReader::PhysicalCachePageSize>(), priority);
    const size_t index_begin = out_entries.size();
    std::vector<TFoldEntry> entries;
    for (TFoldReader::TIndexFile::TKeyCursor csr(&idx_file); csr; ++csr) {
      const TFoldReader::TIndexFile::TKeyItem &item = *csr;
      entries.clear();
      /* the current entry */ {
        TFoldEntry e;
        e.SeqNum = item.SeqNum;
        e.Mutator = item.Mutator;
        e.IsTombstone = item.Value.IsTombstone();
        if (!e.IsTombstone) {
          e.Op = OpToVar(item.Value, reader.Arena.get());
        }
        entries.push_back(std::move(e));
      }
      /* and its history: TDataFile writes a key's history as one run */
      if (item.NumHistKeys > 0UL) {
        TFoldReader::TIndexFile::THistoryKeyCursor hist_csr(&idx_file, item.OffsetOfHistKeys / TData::KeyHistorySize);
        for (size_t i = 0UL; i < item.NumHistKeys && hist_csr; ++i, ++hist_csr) {
          const TFoldReader::TIndexFile::THistoryKeyItem &hist_item = *hist_csr;
          TFoldEntry e;
          e.SeqNum = hist_item.SeqNum;
          e.Mutator = hist_item.Mutator;
          e.IsTombstone = hist_item.Value.IsTombstone();
          if (!e.IsTombstone) {
            e.Op = OpToVar(hist_item.Value, reader.Arena.get());
          }
          entries.push_back(std::move(e));
        }
      }
      std::stable_sort(entries.begin(), entries.end(),
                       [](const TFoldEntry &a, const TFoldEntry &b) {
                         return a.SeqNum > b.SeqNum;
                       });
      const auto folded = FoldOneKey(entries);
      if (folded.empty()) {
        continue;
      }
      const Indy::TKey key(&out_arena, state_alloc, Indy::TKey(item.Key, &idx_arena));
      for (const TFoldEntry &e : folded) {
        Indy::TKey op_key;
        if (e.IsTombstone) {
          op_key = Indy::TKey(Native::TTombstone::Tombstone, &out_arena, state_alloc);
        } else {
          op_key = Indy::TKey(
              &out_arena,
              Sabot::State::TAny::TWrapper(Var::NewSabot(state_alloc, e.Op)).get());
        }
        /* Each entry keeps its own mutator: a folded run that stays commutative (#227) must not
           turn into an Assign that shadows the key's partials in other files. */
        const Indy::TKey id(TUuid(TUuid::Twister), &out_arena, state_alloc);
        out_entries.push_back(TDataFile::TEntryView{index_id, key, op_key.GetCore(), empty_metadata, id.GetCore(),
                                                    &out_arena, e.SeqNum, e.Mutator});
        ++NumKeys;
      }
    }
    /* The source is in key order already; sort anyway (newest first within a key), so the output
       is in a memory layer's order whatever the source's. */
    std::stable_sort(out_entries.begin() + index_begin, out_entries.end(),
                     [](const TDataFile::TEntryView &a, const TDataFile::TEntryView &b) {
                       const auto comp = a.Key.Compare(b.Key);
                       return Atom::IsLt(comp) || (Atom::IsEq(comp) && a.SequenceNumber > b.SequenceNumber);
                     });
  }

  // 5. Serialise the folded entries at dest_gen_id. Record the
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
  const TDataFile::TEntrySource source = [&out_entries](const std::function<void (const TDataFile::TEntryView &)> &cb) {
    for (const auto &entry : out_entries) {
      cb(entry);
    }
  };
  TDataFile data_file(engine, storage_speed, source, file_uuid,
                      dest_gen_id, temp_file_consol_thresh, 0U, priority, source_range);
  LowestSeq = data_file.GetLowestSequence();
  HighestSeq = data_file.GetHighestSequence();
  FileLength = data_file.GetFileLength();
}

}}}  // namespace Orly::Indy::Disk
