/* <orly/indy/disk/fold_data_file.cc>

   Implements <orly/indy/disk/fold_data_file.h>. */

#include <orly/indy/disk/fold_data_file.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include <orly/indy/disk/in_file.h>
#include <orly/indy/disk/merge_data_file.h>
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
    using TReadFile::GetNumUpdates;
    using TReadFile::GetByteOffsetOfUpdateIndex;

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

  /* #745 stage 2: one key's entries (sorted SeqNum desc) folded under a retention window. The versions newer than
     `retain_from` stay as they were written, oldest last, so a read at any version from `retain_from` on finds the
     key's state; the ones at or below it fold to their state as of `retain_from`, as FoldOneKey folds a whole run.
     Every `KeyframeInterval`-th delta of a run whose value is known (it starts at an Assign or a tombstone, in this
     file) becomes an Assign of that value, so a read replays at most that many deltas. Each output entry says
     whether it is a version in the window, whose update's metadata and id carry over. */
  std::vector<std::pair<TFoldEntry, bool>> FoldOneKeyRetaining(const std::vector<TFoldEntry> &entries, TSequenceNumber retain_from) {
    auto first_old = std::find_if(entries.begin(), entries.end(), [retain_from](const TFoldEntry &e) { return e.SeqNum <= retain_from; });
    const std::vector<TFoldEntry> old_entries(first_old, entries.end());
    const auto base = FoldOneKey(old_entries);
    /* The value as of the oldest kept entry, when this file alone determines it. */
    bool known = false;
    bool absent = false;
    Var::TVar running;
    std::optional<TMutator> run_mut;
    size_t run_len = 0UL;
    const auto set_from = [&](const TFoldEntry &e) {
      if (e.IsTombstone) {
        known = true;
        absent = true;
        run_mut.reset();
        run_len = 0UL;
      } else if (e.Mutator == TMutator::Assign) {
        known = true;
        absent = false;
        running = e.Op;
        run_mut.reset();
        run_len = 0UL;
      } else {
        known = false;
      }
    };
    std::vector<std::pair<TFoldEntry, bool>> out;
    /* The base carries empty metadata, as every folded entry does: no version at or below `retain_from` keeps its
       update's, so one sequence number never has entries of both kinds. */
    if (!base.empty()) {
      out.emplace_back(base.front(), false);
      set_from(base.front());
    }
    for (auto it = std::make_reverse_iterator(first_old); it != entries.rend(); ++it) {
      const TFoldEntry &e = *it;
      if (e.IsTombstone || e.Mutator == TMutator::Assign) {
        set_from(e);
        out.emplace_back(e, true);
        continue;
      }
      if (!known || !Var::IsDeferSafeCommutative(e.Mutator) || (run_mut && *run_mut != e.Mutator)) {
        /* A delta on a value this file can't tell, or a mixed or non-commutative run: kept as written, and the
           value stays unknown until the next Assign or tombstone. */
        known = false;
        out.emplace_back(e, true);
        continue;
      }
      running = absent ? e.Op : Rt::Mutate(running, e.Mutator, e.Op);
      absent = false;
      run_mut = e.Mutator;
      if (++run_len == TFoldDataFile::KeyframeInterval) {
        out.emplace_back(TFoldEntry{e.SeqNum, TMutator::Assign, running, /*IsTombstone=*/false}, true);
        run_mut.reset();
        run_len = 0UL;
      } else {
        out.emplace_back(e, true);
      }
    }
    /* Newest first, as FoldOneKey's caller writes them. */
    std::reverse(out.begin(), out.end());
    return out;
  }

}  // namespace

namespace Orly { namespace Indy { namespace Disk {

TFoldDataFile::TFoldDataFile(Indy::Disk::Util::TEngine *engine,
                             Indy::Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                             const TUuid &file_uuid,
                             size_t source_gen_id,
                             size_t dest_gen_id,
                             DiskPriority priority,
                             size_t temp_file_consol_thresh,
                             std::optional<TSequenceNumber> retain_from,
                             const std::atomic<bool> *cancel)
    : NumKeys(0UL),
      LowestSeq(0UL),
      HighestSeq(0UL) {
  const auto throw_if_cancelled = [cancel]() {
    if (cancel && cancel->load(std::memory_order_relaxed)) {
      throw TMergeCancelled();
    }
  };
  throw_if_cancelled();

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
  /* A deque, not a vector: no doubling slack and no copy of the whole list as it grows (#988). */
  std::deque<TDataFile::TEntryView> out_entries;
  /* The empty metadata every folded entry carries. */
  const TCore empty_metadata = Indy::TKey(&out_arena).GetCore();
  /* #745 stage 2: the metadata and id of each update in the retention window, which its entries keep. */
  std::map<TSequenceNumber, std::pair<TCore, TCore>> window_updates;
  if (retain_from) {
    const size_t num_updates = reader.GetNumUpdates();
    TDataFile::TDataInStream<0UL> in_stream(HERE, Source::MergeDataFileScan, priority, &reader,
                                            engine->GetCache<TDataFile::TDataInStream<0UL>::PhysicalCachePageSize>(),
                                            reader.GetByteOffsetOfUpdateIndex());
    for (size_t u = 0UL; u < num_updates; ++u) {
      throw_if_cancelled();
      TSequenceNumber seq_num;
      TCore meta_core, id_core;
      in_stream.Read(seq_num);
      in_stream.Read(&meta_core, sizeof(meta_core));
      in_stream.Read(&id_core, sizeof(id_core));
      in_stream.Skip(sizeof(size_t) * 2UL);
      if (seq_num > *retain_from) {
        const Indy::TKey meta(&out_arena, state_alloc, Indy::TKey(meta_core, reader.Arena.get()));
        const Indy::TKey id(&out_arena, state_alloc, Indy::TKey(id_core, reader.Arena.get()));
        window_updates.emplace(seq_num, std::make_pair(meta.GetCore(), id.GetCore()));
      }
    }
  }

  // 4. Per index: fold each key's current entry and history, in key order.
  for (const TUuid &index_id : index_ids) {
    TFoldReader::TIndexFile idx_file(&reader, index_id, priority);
    TFoldReader::TIndexFile::TArena idx_arena(
        &idx_file, engine->GetCache<TFoldReader::PhysicalCachePageSize>(), priority);
    const size_t index_begin = out_entries.size();
    std::vector<TFoldEntry> entries;
    for (TFoldReader::TIndexFile::TKeyCursor csr(&idx_file); csr; ++csr) {
      throw_if_cancelled();
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
      std::vector<std::pair<TFoldEntry, bool>> folded;
      if (retain_from) {
        folded = FoldOneKeyRetaining(entries, *retain_from);
      } else {
        for (auto &e : FoldOneKey(entries)) {
          folded.emplace_back(std::move(e), false);
        }
      }
      if (folded.empty()) {
        continue;
      }
      const Indy::TKey source_key(item.Key, &idx_arena);
      for (const auto &[e, in_window] : folded) {
        /* Each entry its own copy of the key, as each update in a memory layer has: TDataFile stores a current key's
           hash with it, which the same core reused for the key's history would not match. */
        const Indy::TKey key(&out_arena, state_alloc, source_key);
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
        TCore metadata = empty_metadata;
        TCore id_core;
        const auto update = in_window ? window_updates.find(e.SeqNum) : window_updates.end();
        if (update != window_updates.end()) {
          metadata = update->second.first;
          id_core = update->second.second;
        } else {
          id_core = Indy::TKey(TUuid(TUuid::Twister), &out_arena, state_alloc).GetCore();
        }
        out_entries.push_back(TDataFile::TEntryView{index_id, key, op_key.GetCore(), metadata, id_core,
                                                    &out_arena, e.SeqNum, e.Mutator});
      }
    }
    /* The source is in key order already; sort anyway (newest first within a key), so the output
       is in a memory layer's order whatever the source's. Check first: stable_sort takes a scratch buffer of up to half
       the range, which an already ordered range doesn't need. */
    const auto entry_less = [](const TDataFile::TEntryView &a, const TDataFile::TEntryView &b) {
      const auto comp = a.Key.Compare(b.Key);
      return Atom::IsLt(comp) || (Atom::IsEq(comp) && a.SequenceNumber > b.SequenceNumber);
    };
    if (!std::is_sorted(out_entries.begin() + index_begin, out_entries.end(), entry_less)) {
      std::stable_sort(out_entries.begin() + index_begin, out_entries.end(), entry_less);
    }
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
  const TDataFile::TEntrySource source = [&out_entries, &throw_if_cancelled](const std::function<void (const TDataFile::TEntryView &)> &cb) {
    for (const auto &entry : out_entries) {
      throw_if_cancelled();
      cb(entry);
    }
  };
  TDataFile data_file(engine, storage_speed, source, file_uuid,
                      dest_gen_id, temp_file_consol_thresh, 0U, priority, source_range,
                      false /* do_cache */, cancel);
  NumKeys = data_file.GetNumKeys();
  LowestSeq = data_file.GetLowestSequence();
  HighestSeq = data_file.GetHighestSequence();
  FileLength = data_file.GetFileLength();
}

}}}  // namespace Orly::Indy::Disk
