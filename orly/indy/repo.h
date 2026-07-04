/* <orly/indy/repo.h>

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

#pragma once

#include <atomic>
#include <unordered_map>
#include <vector>

#include <optional>
#include <orly/indy/disk_layer.h>
#include <orly/indy/disk/data_file.h>
#include <orly/indy/disk/fold_data_file.h>
#include <orly/indy/disk/merge_data_file.h>
#include <orly/indy/disk/present_walk_file.h>
#include <orly/indy/disk/update_walk_file.h>
#include <orly/indy/manager_base.h>
#include <orly/indy/memory_layer.h>
#include <orly/indy/present_walker.h>
#include <orly/indy/sequence_number.h>
#include <orly/indy/status.h>
#include <orly/indy/update.h>
#include <orly/indy/update_walker.h>
#include <orly/indy/util/merge_sorter.h>
#include <orly/indy/util/min_heap.h>

namespace Orly {

  namespace Indy {

    namespace L1 {

      class TTransaction;

    }  // L1

    /* An indy storage repo: the LSM-backed store for one point-of-view (POV).
       Derives from the L0 manager's TRepo, which supplies identity, status, and
       the copy-on-write set of data layers (a TMapping over TDataLayers). A repo
       holds an in-memory memtable (CurMemoryLayer), zero or more disk layers,
       and a per-repo sequence-number counter (NextUpdate). Each repo optionally
       has a ParentRepo; while it holds un-promoted updates it joins the parent's
       Tetris merge, which promotes those updates upward -- so a child's durable
       home is its parent, ultimately the parentless safe root (= the global
       POV). See docs/architecture.md for the POV / sequence-number / merge
       model. Abstract: instantiated as TFastRepo (memory-only) or TSafeRepo
       (disk-backed). */
    class TRepo
        : public L0::TManager::TRepo {
      NO_COPY(TRepo);
      public:

      using TParentRepo = L0::TManager::TRepo::TParentRepo;

      /* An immutable, consistent read snapshot of the repo: the pinned disk
         layer set (Mapping), the pinned live memtable (CurrentMemoryLayer), the
         [lower, upper] sequence-number window, and NextId -- all captured
         atomically under Repo->DataLock. A present/update walker reads through a
         TView, so the layers it scans cannot be freed mid-walk; the TView must
         outlive any walker built from it. */
      class TView {
        NO_COPY(TView);
        public:

        TView(const L0::TManager::TPtr<TRepo> &repo);

        TView(TRepo *repo);

        /* Tag selecting the snapshot ctor below. */
        struct TDataLockHeld final {};

        /* Snapshot variant for callers that ALREADY hold Repo->DataLock (e.g.
           TRepo::GetLowestUpdate, which must hold DataLock across its whole
           memtable walk so the update-list linkage it reads cannot be mutated
           by a concurrent AppendUpdate -- #237). Tag-dispatched so it does not
           re-lock the non-recursive DataLock. */
        TView(TRepo *repo, TDataLockHeld);

        /* Releases (Decr) the pinned mapping and memtable under MappingLock. */
        ~TView();

        /* The pinned snapshot of the live memtable. */
        const TMemoryLayer *GetCurMem() const;

        /* The pinned snapshot of the disk-layer set; iterate its entry
           collection to get one TDataLayer per disk file. */
        const TMapping *GetMapping() const;

        /* Count of mapping entries (disk layers); used to size the walker merge
           structures (+1 for the memtable). */
        inline size_t GetNumEntries() const;

        /* The repo's NextUpdate as of the snapshot. */
        inline TSequenceNumber GetNextId() const {
          return NextId;
        }

        /* Oldest in-window sequence number (LowestSeqNum at snapshot), or unset
           if the repo was empty -- walkers bail when either bound is unset. */
        const std::optional<TSequenceNumber> &GetLower() const;

        /* Newest in-window sequence number (HighestSeqNum at snapshot). */
        const std::optional<TSequenceNumber> &GetUpper() const;

        private:

        /* Snapshot {Mapping, CurrentMemoryLayer(+Incr), bounds, NextId} from
           Repo. The caller MUST hold Repo->DataLock; both ctors funnel here. */
        void Snapshot();

        /* The repo this is a snapshot of. */
        TRepo *Repo;

        /* Pinned (Incr'd) live memtable at snapshot time. */
        TMemoryLayer *CurrentMemoryLayer;

        /* Pinned (Incr'd) disk-layer set at snapshot time. */
        TMapping *Mapping;

        /* Snapshot of LowestSeqNum / HighestSeqNum -- the sequence-number
           window the walkers filter each item against. */
        std::optional<TSequenceNumber> LowerBound;

        std::optional<TSequenceNumber> UpperBound;

        /* Snapshot of NextUpdate. */
        TSequenceNumber NextId;

      };  // TView

      /* get a snapshot of this repo using the data lock */
      inline void GetSnapshot(std::optional<TSequenceNumber> &seq_num_start,
                              std::optional<TSequenceNumber> &seq_num_limit,
                              TSequenceNumber &next_seq_num);

      /* Count of un-promoted updates sitting in this repo's memtable
         (HighestSeqNum - LowestSeqNum + 1), or 0 if empty / bounds unset. This
         is the global-merge backlog depth for a Tetris child; read under
         DataLock. Used by write backpressure (#234) to pace accept to promote. */
      inline size_t GetMemBacklogDepth();

      /* The sequence number of the oldest unpopped update. */
      inline const std::optional<TSequenceNumber> &GetSequenceNumberStart() const;

      /* The sequence number of the newest update.
         This is also the total number of updates ever pushed to this repo. */
      inline const std::optional<TSequenceNumber> &GetSequenceNumberLimit() const;

      /* The sequence number the next update pushed here will take (= NextUpdate). */
      inline TSequenceNumber GetNextSequenceNumber() const;

      /* Force the NextUpdate counter. Used to seed a fresh child/test repo's
         numbering from its parent so reads fold in the right order. */
      inline void SetNextSequenceNumber(TSequenceNumber next_id);

      /* Set the high-water mark of updates already promoted to the parent
         (entries at or below this may be dropped on the next mem/disk merge). */
      inline void SetReleasedUpTo(TSequenceNumber released_up_to);

      /* Reserve `num` consecutive sequence numbers in one shot and return the
         first; the bulk counterpart of AppendUpdate's per-update bookkeeping. */
      inline TSequenceNumber UseSequenceNumbers(size_t num);

      /* The high-water mark set by SetReleasedUpTo / ReleaseUpdate. */
      inline TSequenceNumber GetReleasedUpTo() const;

      /* This repo's parent in the repo tree (mirrors the POV tree), or unset
         for the global root. */
      virtual inline const TParentRepo &GetParentRepo() const;

      /* Ingest an already-prepared memtable as a new data layer. A safe repo
         first flushes it to a disk file (wrapped as a TDiskLayer); a fast repo
         keeps the memory layer. Builds a new mapping including it and enqueues a
         disk merge once enough layers accumulate. */
      void AddImportLayer(TMemoryLayer *mem_layer, Base::TEventSemaphore &sem, Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed);

      /* Wrap an existing on-disk file (named by gen_id, with its saved sequence
         range and key count) as a new disk layer in a new mapping. */
      void AddFileToRepo(size_t gen_id, TSequenceNumber low_saved, TSequenceNumber high_saved, size_t num_keys);

      /* Register a replication-synced file: allocate a generation id, hand the
         blocks to the disk engine, then AddFileToRepo it. Returns the gen_id.
         TSafeRepo implements this; TFastRepo (no disk) throws. */
      virtual size_t AddSyncedFileToRepo(size_t starting_block_id,
                                         size_t starting_block_offset,
                                         size_t file_length,
                                         TSequenceNumber low_saved,
                                         TSequenceNumber high_saved,
                                         size_t num_keys) = 0;

      /* Mark the given update for background deletion.
         The sequence number must be older than (that is, less than) that of the oldest unpopped update.
         Marking a non-existent update for deletion is not an error.  It simply does nothing.
         There is no notification when the update is actually deleted. */
      void ReleaseUpdate(TSequenceNumber sequence_number, bool ensure_or_discard);

      /* Open a present-walker over the key range [from, to]: yields the merged
         current value per key across all layers in the view, in key order. With
         ignore_tombstone, deleted keys are skipped. */
      virtual std::unique_ptr<Indy::TPresentWalker> NewPresentWalker(const std::unique_ptr<TView> &view,
                                                                     const TIndexKey &from,
                                                                     const TIndexKey &to,
                                                                     bool ignore_tombstone = false);

      /* Open a present-walker for a single key. exact_point marks a fully-bound
         point read so each layer may seek straight to the key instead of
         head-scanning its line (#257). */
      virtual std::unique_ptr<Indy::TPresentWalker> NewPresentWalker(const std::unique_ptr<TView> &view,
                                                                     const TIndexKey &key,
                                                                     bool ignore_tombstone = false,
                                                                     bool exact_point = false);

      /* Open an update-walker over the sequence-number range [from, to]: yields
         raw updates (the change log) in sequence order, merged across layers.
         Used by the promote-to-parent and replication paths. */
      virtual std::unique_ptr<Indy::TUpdateWalker> NewUpdateWalker(const std::unique_ptr<TView> &view,
                                                                   TSequenceNumber from,
                                                                   const std::optional<TSequenceNumber> &to);

      /* Open-ended update-walker from `from` to the newest update. */
      virtual std::unique_ptr<Indy::TUpdateWalker> NewUpdateWalker(const std::unique_ptr<TView> &view,
                                                                   TSequenceNumber from);

      /* Trim the oldest on-disk generation below the release horizon (root repo
         only). Memory-only TFastRepo has nothing to tail. */
      virtual void StepTail(size_t block_slots_available) = 0;

      protected:

      /* Construct a fresh, empty repo under `manager` (id, ttl, optional parent). */
      TRepo(L0::TManager *manager,
            const Base::TUuid &repo_id,
            const TTtl &ttl,
            const std::optional<L0::TManager::TPtr<L0::TManager::TRepo>> &parent_repo);

      /* Reconstruct a repo with pre-existing sequence bounds, next-update
         counter, and status (e.g. reloading saved state). */
      TRepo(L0::TManager *manager,
            const Base::TUuid &repo_id,
            const TTtl &ttl,
            const std::optional<L0::TManager::TPtr<L0::TManager::TRepo>> &parent_repo,
            const std::optional<TSequenceNumber> &lowest,
            const std::optional<TSequenceNumber> &highest,
            TSequenceNumber next_update,
            TStatus status);

      virtual ~TRepo();

      /* Push an update into the memtable (under DataLock): stamp it with the
         next sequence number, advance HighestSeqNum / set LowestSeqNum, insert
         into CurMemoryLayer, and join the parent's Tetris if not already in it.
         Returns the assigned sequence number. */
      std::optional<TSequenceNumber> AppendUpdate(TUpdate *update, TSequenceNumber &next_update) NO_THROW;

      /* Drop the oldest unpopped update by advancing LowestSeqNum (under
         DataLock); when the memtable drains, clears the bounds and Parts Tetris.
         Returns the popped sequence number. */
      std::optional<TSequenceNumber> PopLowest(TSequenceNumber &next_update) NO_THROW;

      /* Reconstruct the single oldest-unpopped update (the one Tetris Peeks to
         promote to the parent). Holds DataLock across the whole walk (#237). */
      std::shared_ptr<TUpdate> GetLowestUpdate();

      /* Flip the repo's status (Normal / Paused / Failed) and join or Part the
         parent's Tetris accordingly. */
      std::optional<TSequenceNumber> ChangeStatus(TStatus, TSequenceNumber &next_update) NO_THROW;

      /* Mem-merge step: seal CurMemoryLayer into the mapping and start a fresh
         one, then consolidate the trailing memtables. The parentless safe root
         flushes the result to a new disk file; child / fast repos keep it in
         memory, dropping entries already released to the parent. */
      virtual void StepMergeMem() override;

      /* Disk-merge step: compact adjacent same-generation disk files. Only
         TSafeRepo has disk files; TFastRepo asserts it is never called. */
      virtual void StepMergeDisk(size_t block_slots_available) = 0;

      /* True iff 'mapping' holds an adjacent pair of disk layers the disk-merge step's scan
         (TSafeRepo::StepMergeDisk) could select -- same adjacency and generation condition,
         minus the MarkedTaken check (#325).  Taken-ness is written under MergeLock, which the
         enqueue-time callers don't hold; skipping it means this reads only immutable layer
         fields (kind, size), and can only over-approximate -- an occasional pass that finds
         nothing, which is exactly what every enqueue risked before this gate.  Exactness the
         other way is what matters: a false here means the step's scan would provably find
         nothing (taken-ness only removes candidates), and every state change that could make a
         pair mergeable again is a mapping install whose site re-runs this gate, so skipping the
         enqueue can never strand a mergeable mapping. */
      bool HasDiskMergeCandidate(TMapping *mapping) const;

      /* Copy-on-write a new data layer into the mapping; returns the layer count. */
      size_t AddMapping(TDataLayer *layer);

      /* Pin (Incr) the current mapping so a reader can hold it across a walk. */
      TMapping *AcquireCurrentMapping();

      /* Unpin (Decr) a mapping acquired via AcquireCurrentMapping. */
      void ReleaseMapping(TMapping *mapping);

      /* Serializes the mem-merge body of StepMergeMem (one per repo at a time). */
      std::mutex MemMergeLock;

      /* Guards the memtable + sequence-number state (LowestSeqNum,
         HighestSeqNum, NextUpdate, ReleasedUpTo, CurMemoryLayer inserts, Tetris
         join/part). Non-recursive; lock order is DataLock -> MappingLock. */
      std::mutex DataLock;

      /* After a mem merge, drop this repo from the manager's dirty set if no
         memtable still holds data; otherwise re-enqueue another mem merge. */
      void CheckRemoveDirty();

      /* The live memtable that AppendUpdate inserts into. */
      TMemoryLayer *CurMemoryLayer;

      private:

      /* A repo's concrete present-walker: builds one sub-walker per data layer
         (each disk layer + the memtable) and k-way-merges them through MinHeap,
         yielding the newest entry per key in key order. (Same-key entries are
         all yielded, not shadowed, so the context-level fold can compose
         commutative writes -- see Refresh.) */
      class TPresentWalker
          : public Indy::TPresentWalker {
        NO_COPY(TPresentWalker);
        public:

        /* Range walk [from, to]; builds the per-layer sub-walkers serially. */
        TPresentWalker(const std::unique_ptr<TView> &view,
                       const TIndexKey &from,
                       const TIndexKey &to,
                       bool ignore_tombstone);

        /* Single-key walk; builds the per-layer sub-walkers in parallel across
           fibers (see PrepVec / TRunnablePrep). exact_point enables seek. */
        TPresentWalker(const std::unique_ptr<TView> &view,
                       const TIndexKey &key,
                       bool ignore_tombstone,
                       bool exact_point);

        inline virtual ~TPresentWalker() {}

        /* True iff. we have an item. */
        inline virtual operator bool() const;

        /* The current item. */
        inline virtual const TItem &operator*() const;

        /* Walk to the next item, if any. */
        inline virtual TPresentWalker &operator++();

        private:

        /* A fiber task that builds one layer's sub-walker off the local frame
           pool and signals the shared TSync barrier on completion -- lets the
           single-key ctor construct all per-layer walkers concurrently. Move is
           forbidden (it throws), so PrepVec must be reserved to its final size
           up front. */
        class TRunnablePrep
            : public Indy::Fiber::TRunnable {
          NO_COPY(TRunnablePrep);
          public:

          TRunnablePrep(TDataLayer *layer, TPresentWalker *walker, Fiber::TSync *sync)
              : Layer(layer), Walker(walker), Sync(sync) {
            Frame = Fiber::TFrame::LocalFramePool->Alloc();
            try {
              Frame->Latch(this, static_cast<Fiber::TRunnable::TFunc>(&TRunnablePrep::Compute));
            } catch (...) {
              Fiber::TFrame::LocalFramePool->Free(Frame);
              throw;
            }
          }

          TRunnablePrep(Orly::Indy::TRepo::TPresentWalker::TRunnablePrep &&) {
            throw std::logic_error("Moving TRunnablePrep is not allowed. This means you did not pre-allocate enough space to hold them.");
          }

          virtual ~TRunnablePrep() {
          }

          /* Fiber body: build this layer's sub-walker, append it to the parent
             walker's WalkerVec, signal the barrier, and free the frame. */
          void Compute() {
            //printf("TRunnablePrep::Compute()\n");
            assert(Walker);
            assert(Layer);
            assert(Sync);
            assert(Fiber::TFrame::LocalFrame == Frame);
            Walker->WalkerVec.emplace_back(Layer->NewPresentWalker(Walker->From, Walker->ExactPoint));
            Sync->Complete();
            Fiber::FreeMyFrame(Fiber::TFrame::LocalFramePool);
          }

          private:

          /* The fiber frame this task runs on (from the local frame pool). */
          Fiber::TFrame *Frame;

          /* The data layer to build a sub-walker for. */
          TDataLayer *Layer;

          /* The parent walker to append the sub-walker into. */
          TPresentWalker *Walker;

          /* Barrier the ctor waits on for all prep tasks to finish. */
          Fiber::TSync *Sync;

        };  // TRunnablePrep

        /* Prime the first item (asserts none emitted yet). */
        inline void Init();

        /* Advance to the next item on each ++ (asserts an item was emitted). */
        inline void Refresh();

        /* Range start key (also the lookup key for the single-key ctor). */
        TIndexKey From;

        /* Range end key (unset for the single-key ctor). */
        TIndexKey To;

        /* The snapshot read through; held by reference, so it must outlive this
           walker. Lower/Upper copy the view's sequence-number window for the
           per-item filter. */
        const std::unique_ptr<TView> &View;
        const TSequenceNumber Lower;
        const TSequenceNumber Upper;

        /* One sub-walker per data layer (disk layers + memtable); PrepVec holds
           the fiber tasks that build them for the single-key ctor. */
        std::vector<std::unique_ptr<Indy::TPresentWalker>> WalkerVec;
        std::vector<TRunnablePrep> PrepVec;

        /* K-way merge front: the current item of each sub-walker, keyed by its
           WalkerVec index; pops key-ascending, newest-sequence-first. */
        Util::TMinHeap<TItem, size_t> MinHeap;

        /* True while the heap still has items to yield. */
        bool Valid;

        /* The current item (lazily refreshed; mutable for the const accessors). */
        mutable TItem Item;

        /* When set, deleted keys (tombstones) are skipped. */
        const bool IgnoreTombstone;

        /* Fully-bound point read: layers may seek instead of head-scan (#257).
           Only the single-key ctor sets this true; the range ctor leaves it
           false. */
        bool ExactPoint = false;

      };  // TPresentWalker

      /* A repo's concrete update-walker: walks raw updates (the change log) in
         sequence-number order, merged across layers via MergeSorter. Unlike
         TPresentWalker (the current value per key), it yields whole updates by
         sequence number -- the form the promote-to-parent and replication paths
         consume. */
      class TUpdateWalker
          : public Indy::TUpdateWalker {
        NO_COPY(TUpdateWalker);
        public:

        /* Walk updates with sequence number in [from, to]. */
        TUpdateWalker(const std::unique_ptr<TView> &view,
                      TSequenceNumber from,
                      const std::optional<TSequenceNumber> &to);

        virtual ~TUpdateWalker();

        /* True iff. we have an item. */
        virtual operator bool() const;

        /* The current item. */
        virtual const Indy::TUpdateWalker::TItem &operator*() const;

        /* Walk to the next item, if any. */
        virtual TUpdateWalker &operator++();

        private:

        /* Advance to the next update in sequence order. */
        void Refresh();

        /* Sequence-number window being walked. */
        TSequenceNumber From;
        std::optional<TSequenceNumber> To;

        /* The snapshot read through; held by reference, must outlive the walker. */
        const std::unique_ptr<TView> &View;

        /* Per-layer sub-walker plus its element index in the merge sorter. */
        std::unordered_map<const TDataLayer *, std::pair<std::unique_ptr<Indy::TUpdateWalker>, size_t>> WalkerMap;

        /* Orders the per-layer sub-walkers by lowest current sequence number,
           backed by the SorterAlloc element array. */
        Util::TMergeSorter<TSequenceNumber, const TDataLayer *> MergeSorter;
        Util::TMergeSorter<TSequenceNumber, const TDataLayer *>::TMergeElement *SorterAlloc;

        /* True while the sorter still has updates to yield. */
        bool Valid;

        /* The current update (mutable for the const accessors). */
        mutable TItem Item;

      };  // TUpdateWalker

      /* This repo's parent in the repo tree (unset for the global root). */
      TParentRepo ParentRepo;

      /* Sequence number of the oldest unpopped update; unset when the memtable
         is empty (paired set/unset with HighestSeqNum). */
      std::optional<TSequenceNumber> LowestSeqNum;

      /* Sequence number of the newest update (also the total ever pushed). */
      std::optional<TSequenceNumber> HighestSeqNum;

      /* The sequence number the next pushed update will take. */
      TSequenceNumber NextUpdate;

      protected:

      /* Manager GC hook: visit the parent-repo pointer so it is kept reachable. */
      virtual bool ForEachDependentPtr(const std::function<bool (L0::TManager::TAnyPtr &)> &cb) noexcept override {
        if (ParentRepo) {
          cb(*ParentRepo);
        }
        return true;
      }

      /* High-water mark of updates already promoted to the parent; entries at or
         below it may be dropped on the next mem/disk merge. */
      TSequenceNumber ReleasedUpTo;

      private:

      /* Whether this repo is currently a registered child in its parent's Tetris
         merge. Gating Join on !InTetris makes it idempotent and lets a join that
         failed under memory pressure retry on the next AppendUpdate (#250). */
      bool InTetris;

      /* Transactions drive AppendUpdate / PopLowest / promotion, so they reach
         the protected mutators. */
      friend class L1::TTransaction;

    };  // TRepo

    /* A memory-only repo (Fast policy): its data lives entirely in memtables and
       is promoted to the parent -- it never writes disk files. The disk-only
       operations (StepMergeDisk, StepTail, AddSyncedFileToRepo) are therefore
       unreachable here and assert/throw if called. */
    class TFastRepo
        : public Orly::Indy::TRepo {
      NO_COPY(TFastRepo);
      public:

      TFastRepo(L0::TManager *manager,
                const Base::TUuid &repo_id,
                const TTtl &ttl,
                const std::optional<L0::TManager::TPtr<L0::TManager::TRepo>> &parent_repo);

      TFastRepo(L0::TManager *manager,
                const Base::TUuid &repo_id,
                const TTtl &ttl,
                const std::optional<L0::TManager::TPtr<L0::TManager::TRepo>> &parent_repo,
                const std::optional<TSequenceNumber> &lowest,
                const std::optional<TSequenceNumber> &highest,
                TSequenceNumber next_update,
                TStatus status);

      virtual ~TFastRepo();

      private:

      /* false -- a fast repo is memory-only. */
      virtual bool IsSafeRepo() const override;

      /* Unreachable: a fast repo has no disk files. Asserts false. */
      virtual void StepMergeDisk(size_t block_slots_available) override;

      /* Unreachable: a fast repo has no disk tail. Asserts false. */
      virtual void StepTail(size_t block_slots_available) override;

      /* Unreachable: a fast repo cannot host synced files. Throws. */
      virtual size_t AddSyncedFileToRepo(size_t starting_block_id,
                                         size_t starting_block_offset,
                                         size_t file_length,
                                         TSequenceNumber low_saved,
                                         TSequenceNumber high_saved,
                                         size_t num_keys) override;

    };  // TFastRepo

    /* A disk-backed repo (Safe policy): memtables are flushed to on-disk
       generation files, which are then compacted (StepMergeDisk) and tailed
       (StepTail). It implements the disk write/merge/remove operations, the
       per-file walkers, and reload-from-disk. Owns the generation-id counter and
       the disk-merge lock. */
    class TSafeRepo
        : public Orly::Indy::TRepo {
      NO_COPY(TSafeRepo);
      public:

      TSafeRepo(L0::TManager *manager,
                const Base::TUuid &repo_id,
                const TTtl &ttl,
                const std::optional<L0::TManager::TPtr<L0::TManager::TRepo>> &parent_repo);

      TSafeRepo(L0::TManager *manager,
                const Base::TUuid &repo_id,
                const TTtl &ttl,
                const std::optional<L0::TManager::TPtr<L0::TManager::TRepo>> &parent_repo,
                const std::optional<TSequenceNumber> &lowest,
                const std::optional<TSequenceNumber> &highest,
                TSequenceNumber next_update,
                TStatus status);

      virtual ~TSafeRepo();

      /* Compact adjacent same-generation disk files into one new file. */
      virtual void StepMergeDisk(size_t block_slots_available) override;

      /* Trim the oldest disk generation below the release horizon (root only). */
      virtual void StepTail(size_t block_slots_available) override;

      /* Rebuild a TSafeRepo from its on-disk generation files (static factory). */
      static TSafeRepo *ReConstructFromDisk(L0::TManager *manager,
                                            const Base::TUuid &repo_id,
                                            const TDeadline &deadline);

      protected:

      /* Allocate the next monotonic generation id (++NextGenId). */
      inline size_t GetNextGenId();

      private:

      /* true -- a safe repo is disk-backed. */
      virtual bool IsSafeRepo() const override;

      /* Merge the given disk generations into one new file. Two-phase: a raw
         merge, then a commutative fold (TFoldDataFile) if any non-Assign entries
         are present. can_tail/can_tail_tombstone allow dropping data below the
         release horizon at the root. Returns the new generation id. */
      virtual size_t MergeFiles(const std::vector<size_t> &gen_id_vec,
                                Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                                size_t max_block_cache_read_slots_allowed,
                                size_t temp_file_consol_thresh,
                                TSequenceNumber &out_saved_low_seq,
                                TSequenceNumber &out_saved_high_seq,
                                size_t &out_num_keys,
                                TSequenceNumber release_up_to,
                                bool can_tail,
                                bool can_tail_tombstone) override;

      /* Reclaim a generation file's blocks and drop its per-scheduler caches. */
      virtual void RemoveFile(size_t gen_id) override;

      /* Serialize a memtable to a new on-disk generation file; returns its
         generation id (and, by reference, its saved sequence range / key count). */
      virtual size_t WriteFile(TMemoryLayer *memory_layer,
                               Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                               TSequenceNumber &out_saved_low_seq,
                               TSequenceNumber &out_saved_high_seq,
                               size_t &out_num_keys,
                               TSequenceNumber release_up_to) override;

      /* Register a replication-synced file: allocate a gen_id, hand the blocks to
         the disk engine, then AddFileToRepo it. */
      virtual size_t AddSyncedFileToRepo(size_t starting_block_id,
                                         size_t starting_block_offset,
                                         size_t file_length,
                                         TSequenceNumber low_saved,
                                         TSequenceNumber high_saved,
                                         size_t num_keys) override;

      /* Open a present-walker over a single on-disk generation file (range). */
      virtual std::unique_ptr<Orly::Indy::TPresentWalker> NewPresentWalkerFile(size_t /*gen_id*/,
                                                                               const TIndexKey &/*from*/,
                                                                               const TIndexKey &/*to*/) const override;

      /* Open a present-walker over a single on-disk generation file (single key). */
      virtual std::unique_ptr<Orly::Indy::TPresentWalker> NewPresentWalkerFile(size_t /*gen_id*/,
                                                                               const TIndexKey &/*key*/) const override;

      /* Open an update-walker over a single on-disk generation file. */
      virtual std::unique_ptr<Orly::Indy::TUpdateWalker> NewUpdateWalkerFile(size_t /*gen_id*/, TSequenceNumber /*from*/) const override;

      /* The monotonic generation-id counter (seeded from the highest existing
         file generation at construction). */
      std::atomic<size_t> NextGenId;

      /* Serializes selection/marking of disk layers for compaction and tailing. */
      std::mutex MergeLock;

    };  // TSafeRepo

    /***************
      *** inline ***
      *************/

    inline size_t TRepo::TView::GetNumEntries() const {
      size_t count = 0UL;
      for (TMapping::TEntryCollection::TCursor mapping_csr(Mapping->GetEntryCollection()); mapping_csr; ++mapping_csr, ++count) {}
      return count;
    }

    inline void TRepo::GetSnapshot(std::optional<TSequenceNumber> &seq_num_start,
                                   std::optional<TSequenceNumber> &seq_num_limit,
                                   TSequenceNumber &next_seq_num) {
      std::lock_guard<std::mutex> lock(DataLock);
      seq_num_start = LowestSeqNum;
      seq_num_limit = HighestSeqNum;
      next_seq_num = NextUpdate;
    }

    inline size_t TRepo::GetMemBacklogDepth() {
      std::lock_guard<std::mutex> lock(DataLock);
      if (!LowestSeqNum || !HighestSeqNum || *HighestSeqNum < *LowestSeqNum) {
        return 0UL;
      }
      return static_cast<size_t>(*HighestSeqNum - *LowestSeqNum) + 1UL;
    }

    inline const std::optional<TSequenceNumber> &TRepo::GetSequenceNumberStart() const {
      return LowestSeqNum;
    }

    inline const std::optional<TSequenceNumber> &TRepo::GetSequenceNumberLimit() const {
      return HighestSeqNum;
    }

    inline TSequenceNumber TRepo::GetNextSequenceNumber() const {
      return NextUpdate;
    }

    inline void TRepo::SetNextSequenceNumber(TSequenceNumber next_id) {
      NextUpdate = next_id;
    }

    inline void TRepo::SetReleasedUpTo(TSequenceNumber released_up_to) {
      ReleasedUpTo = released_up_to;
    }

    inline TSequenceNumber TRepo::UseSequenceNumbers(size_t num) {
      assert(num);
      std::lock_guard<std::mutex> lock(DataLock);
      TSequenceNumber starting = NextUpdate;
      NextUpdate += num;
      HighestSeqNum = NextUpdate - 1;
      if (!LowestSeqNum) {
        LowestSeqNum = starting;
      }
      return starting;
    }

    inline TSequenceNumber TRepo::GetReleasedUpTo() const {
      return ReleasedUpTo;
    }

    inline const TRepo::TParentRepo &TRepo::GetParentRepo() const {
      return ParentRepo;
    }

    inline TRepo::TPresentWalker::operator bool() const {
      return Valid;
    }

    inline const TPresentWalker::TItem &TRepo::TPresentWalker::operator*() const {
      assert(Valid);
      return Item;
    }

    inline TRepo::TPresentWalker &TRepo::TPresentWalker::operator++() {
      assert(Valid);
      Valid = static_cast<bool>(MinHeap);
      Refresh();
      return *this;
    }

    inline void TRepo::TPresentWalker::Init() {
      bool done = false;
      while (Valid) {
        size_t pos;
        const Indy::TPresentWalker::TItem &cur_item = MinHeap.Pop(pos);
        Indy::TPresentWalker &walker = *WalkerVec[pos];
        assert(cur_item.Key.IsTuple());
        assert((*walker).KeyArena == cur_item.KeyArena);
        assert((*walker).OpArena == cur_item.OpArena);
        assert((*walker).SequenceNumber == cur_item.SequenceNumber);
        assert(Item.KeyArena == nullptr);
        if (cur_item.SequenceNumber >= Lower && cur_item.SequenceNumber <= Upper && (!IgnoreTombstone || !cur_item.Op.IsTombstone())) {
          done = true;
          Item = cur_item;
        }
        ++walker;
        if (walker) {
          MinHeap.Insert(*walker, pos);
        }
        if (done) {
          break;
        } else {
          Valid = static_cast<bool>(MinHeap);
        }
      }
    }

    inline void TRepo::TPresentWalker::Refresh() {
      bool done = false;
      while (Valid) {
        size_t pos;
        const Indy::TPresentWalker::TItem &cur_item = MinHeap.Pop(pos);
        Indy::TPresentWalker &walker = *WalkerVec[pos];
        assert(cur_item.Key.IsTuple());
        assert((*walker).KeyArena == cur_item.KeyArena);
        assert((*walker).OpArena == cur_item.OpArena);
        assert((*walker).SequenceNumber == cur_item.SequenceNumber);
        assert(Item.KeyArena != nullptr);
        /* #49: the historical filter dropped same-key entries on the
           grounds that the newest one shadowed older ones (Assign
           overwrite semantics). After phase 2 introduced commutative
           non-Assign entries, the older same-key entries are NOT shadowed
           -- the read-path fold in TContext::TPresentWalker needs them
           to compose multiple `+= n` writes. We yield all entries here
           and rely on the context-level fold + dedup for correctness;
           Assign-only workloads keep the same observable behavior, they
           just pop a few extra items at the context layer. */
        if (cur_item.SequenceNumber >= Lower && cur_item.SequenceNumber <= Upper && (!IgnoreTombstone || !cur_item.Op.IsTombstone())) {
          done = true;
          Item = cur_item;
        }
        ++walker;
        if (walker) {
          MinHeap.Insert(*walker, pos);
        }
        if (done) {
          break;
        } else {
          Valid = static_cast<bool>(MinHeap);
        }
      }
    }

    inline size_t TSafeRepo::GetNextGenId() {
      return ++NextGenId;
    }

  }  // Indy

}  // Orly