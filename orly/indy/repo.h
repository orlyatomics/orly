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
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <optional>
#include <orly/indy/disk_layer.h>
#include <orly/indy/disk/data_file.h>
#include <orly/indy/disk/fold_data_file.h>
#include <orly/indy/disk/merge_data_file.h>
#include <orly/indy/disk/present_walk_file.h>
#include <orly/indy/disk/update_walk_file.h>
#include <orly/indy/fork_watch.h>
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

    /* The longest --retention_seconds window the server accepts (about 127 years). Far beyond any real window, and
       small enough that a window converted to nanoseconds, or subtracted from a clock reading, cannot overflow. */
    constexpr size_t MaxRetentionSeconds = 4'000'000'000UL;

    /* A retention window in seconds as a duration, capped at MaxRetentionSeconds. The server refuses a larger flag at
       startup; the cap is for any other caller of SetRetention. */
    [[nodiscard]] inline std::chrono::seconds RetentionWindow(size_t seconds) noexcept {
      return std::chrono::seconds(seconds < MaxRetentionSeconds ? seconds : MaxRetentionSeconds);
    }

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

      /* Test-only: called by StepMergeMem just after it seals the current memory layer into the
         mapping, with no lock held, so a unit test can append to the repo mid-merge (#665).
         Empty, and never set, in production: one null check per memory merge. */
      static std::function<void (TRepo *)> OnMergeMemSealedForTest;

      /* The most fiber frames point reads may hold at once, across the whole process, for
         building their per-layer sub-walkers in parallel (#762). A read past it builds inline
         instead, so the frames other work needs to start (every WebSocket statement takes one)
         stay free. orlyi sets it to half of --max_parallel_frames; unlimited by default. */
      static void SetReadPrepFrameBudget(size_t frames) {
        ReadPrepFrameBudget.store(frames, std::memory_order_relaxed);
      }

      /* An immutable, consistent read snapshot of the repo: the pinned disk
         layer set (Mapping), the pinned live memtable (CurrentMemoryLayer), the
         [lower, upper] sequence-number window, and NextId -- all captured
         atomically under Repo->DataLock. A present/update walker reads through a
         TView, so the layers it scans cannot be freed mid-walk; the TView must
         outlive any walker built from it. */
      class TView {
        NO_COPY(TView);
        public:

        explicit TView(const L0::TManager::TPtr<TRepo> &repo);

        explicit TView(TRepo *repo);

        /* Tag selecting the snapshot ctor below. */
        struct TDataLockHeld final {};

        /* Snapshot variant for callers that ALREADY hold Repo->DataLock (e.g.
           TRepo::GetLowestUpdate, which must hold DataLock across its whole
           memtable walk so the update-list linkage it reads cannot be mutated
           by a concurrent AppendUpdate -- #237). Tag-dispatched so it does not
           re-lock the non-recursive DataLock. */
        TView(TRepo *repo, TDataLockHeld);

        /* A view of the repo as of sequence number `at` (#745), or of now when unset. The upper bound is clamped to
           `at`, so walkers drop every newer entry, and disk walkers seek each key's newest entry at or below it. */
        TView(TRepo *repo, std::optional<TSequenceNumber> at);

        /* Releases (Decr) the pinned mapping and memtable under MappingLock. */
        ~TView();

        /* The pinned snapshot of the live memtable. */
        [[nodiscard]] const TMemoryLayer *GetCurMem() const noexcept;

        /* The pinned snapshot of the disk-layer set; iterate its entry
           collection to get one TDataLayer per disk file. */
        [[nodiscard]] const TMapping *GetMapping() const noexcept;

        /* Count of mapping entries (disk layers); used to size the walker merge
           structures (+1 for the memtable). */
        [[nodiscard]] inline size_t GetNumEntries() const noexcept;

        /* The mapping's disk layers and memory layers (#701). */
        inline void CountLayers(size_t &disk_layers, size_t &mem_layers) const noexcept;

        /* Call cb with each layer a walk of this view reads: the mapping's, then the memtable
           (#749). */
        template <typename TCb>
        void ForEachLayer(const TCb &cb) const {
          for (TMapping::TEntryCollection::TCursor mapping_csr(Mapping->GetEntryCollection()); mapping_csr; ++mapping_csr) {
            cb(static_cast<const TDataLayer *>(mapping_csr->GetLayer()));
          }
          cb(static_cast<const TDataLayer *>(CurrentMemoryLayer));
        }

        /* The repo's NextUpdate as of the snapshot. */
        [[nodiscard]] inline TSequenceNumber GetNextId() const noexcept {
          return NextId;
        }

        /* Oldest in-window sequence number (LowestSeqNum at snapshot), or unset
           if the repo was empty -- walkers bail when either bound is unset. */
        [[nodiscard]] const std::optional<TSequenceNumber> &GetLower() const noexcept;

        /* Newest in-window sequence number (HighestSeqNum at snapshot, clamped to the version read at, if any). */
        [[nodiscard]] const std::optional<TSequenceNumber> &GetUpper() const noexcept;

        /* The version this view reads at, or unset for a read of now (#745). */
        [[nodiscard]] const std::optional<TSequenceNumber> &GetAt() const noexcept {
          return At;
        }

        /* HighestSeqNum at snapshot, never clamped: the newest version there is to read (#745). */
        [[nodiscard]] const std::optional<TSequenceNumber> &GetNewest() const noexcept {
          return Newest;
        }

        private:

        /* Snapshot {Mapping, CurrentMemoryLayer(+Incr), bounds, NextId} from
           Repo. The caller MUST hold Repo->DataLock; both ctors funnel here. */
        void Snapshot();

        /* The repo this is a snapshot of. */
        TRepo *Repo = nullptr;

        /* Pinned (Incr'd) live memtable at snapshot time. */
        TMemoryLayer *CurrentMemoryLayer = nullptr;

        /* Pinned (Incr'd) disk-layer set at snapshot time. */
        TMapping *Mapping = nullptr;

        /* Snapshot of LowestSeqNum / HighestSeqNum -- the sequence-number
           window the walkers filter each item against. */
        std::optional<TSequenceNumber> LowerBound;

        std::optional<TSequenceNumber> UpperBound;

        /* Snapshot of NextUpdate. */
        TSequenceNumber NextId = 0UL;

        /* See GetAt and GetNewest. */
        std::optional<TSequenceNumber> At;

        std::optional<TSequenceNumber> Newest;

      };  // TView

      /* get a snapshot of this repo using the data lock */
      inline void GetSnapshot(std::optional<TSequenceNumber> &seq_num_start,
                              std::optional<TSequenceNumber> &seq_num_limit,
                              TSequenceNumber &next_seq_num);

      /* Count of un-promoted updates sitting in this repo's memtable
         (HighestSeqNum - LowestSeqNum + 1), or 0 if empty / bounds unset. This
         is the global-merge backlog depth for a Tetris child; read under
         DataLock. Used by write backpressure (#234) to pace accept to promote. */
      [[nodiscard]] inline size_t GetMemBacklogDepth();

      /* True if Tetris is promoting this repo's memtable backlog to its parent, so the backlog
         drains: the repo has a parent and is in the parent's Tetris. False for a root, and for a
         paused or failed repo, which leaves its parent's Tetris and keeps its backlog until it
         is unpaused (#626). */
      [[nodiscard]] inline bool IsBacklogDraining();

      /* How many entries the un-promoted updates of GetMemBacklogDepth hold (#628). A batch is
         one update with an entry per write, and the memory merge copies entries, so this is the
         cost the writer backpressure caps. 0 for a root (nothing promotes it) and while the count
         is unknown (see BacklogEntriesKnown). */
      [[nodiscard]] inline size_t GetMemBacklogEntries();

      /* A writer's room in this repo's backlog, held from before it commits until after its
         update is in the backlog (#721). The writer backpressure used to check the backlog cap
         only after a write had committed, so every writer that came in while the backlog was
         under the cap landed: with 8 writers of 200-write batches, the backlog of a POV capped at
         1,250 entries reached 3,000 and more. TryReserve counts the backlog plus the room other
         writers hold, under DataLock, so the cap holds however many writers arrive at once.

         Declare it before the transaction, so it outlives the transaction and is released only
         once AppendUpdate has counted the update (until then the write counts twice, which
         errs on the safe side). */
      class TBacklogReservation {
        NO_COPY(TBacklogReservation);
        public:

        explicit TBacklogReservation(TRepo *repo)
            : Repo(repo) {}

        ~TBacklogReservation();

        /* Takes room for one update of num_entries entries if the backlog's updates stay at or
           under update_cap and its entries at or under entry_cap. A write bigger than entry_cap
           alone gets room once the backlog holds no entries; while it waits, smaller writes
           get none, so they can't keep the backlog from ever emptying. Returns false, and holds
           nothing new, if there is no room. */
        [[nodiscard]] bool TryReserve(size_t update_cap, size_t entry_cap, size_t num_entries);

        [[nodiscard]] bool IsReserved() const noexcept {
          return Reserved;
        }

        /* After TryReserve found no room: true if nothing will drain the backlog until another
           write commits, because the repo is a root (nothing promotes it), or a child whose
           join to its parent's Tetris was deferred under memory pressure, which only its next
           AppendUpdate retries (#250). A paused or failed repo is not undrainable here; its
           caller refuses writes to it. */
        [[nodiscard]] bool IsUndrainable() const noexcept {
          return Undrainable;
        }

        private:

        TRepo *Repo = nullptr;

        size_t NumEntries = 0UL;

        bool Reserved = false;

        bool Undrainable = false;

        /* True while this is a write bigger than the entry cap that is waiting for room. */
        bool OversizedWaiting = false;

      };  // TBacklogReservation

      /* The most entries and updates any child repo's backlog has held since startup, after
         an AppendUpdate (#721), for the reporting port. */
      [[nodiscard]] static size_t GetPeakBacklogEntries() noexcept {
        return PeakBacklogEntries.load();
      }

      [[nodiscard]] static size_t GetPeakBacklogUpdates() noexcept {
        return PeakBacklogUpdates.load();
      }

      /* #769: the updates every child repo holds that Tetris hasn't promoted to its parent yet,
         summed over all of them, and the entries those updates hold (as far as they are known,
         see BacklogEntriesKnown). One update is one committed transaction, so this is what a
         stop right now would lose: an update promoted from a POV made `from` another POV is
         counted once, in whichever backlog holds it. A graceful stop logs it. */
      [[nodiscard]] static size_t GetUnpromotedUpdates() noexcept {
        return UnpromotedUpdates.load();
      }

      [[nodiscard]] static size_t GetUnpromotedEntries() noexcept {
        return UnpromotedEntries.load();
      }

      /* #769: the updates popped from child repos' backlogs since startup, i.e. promoted one
         level by a committed Tetris transaction. Only ever grows, so a graceful stop can tell
         whether Tetris is still making progress. */
      [[nodiscard]] static size_t GetPromotedUpdates() noexcept {
        return PromotedUpdates.load();
      }

      /* #769: the unpromoted updates that child repos took with them when they were destroyed
         (an expired POV's, or the server's teardown), since startup. */
      [[nodiscard]] static size_t GetDroppedUpdates() noexcept {
        return DroppedUpdates.load();
      }

      /* The sequence number of the oldest unpopped update. */
      [[nodiscard]] inline const std::optional<TSequenceNumber> &GetSequenceNumberStart() const noexcept;

      /* The sequence number of the newest update.
         This is also the total number of updates ever pushed to this repo. */
      [[nodiscard]] inline const std::optional<TSequenceNumber> &GetSequenceNumberLimit() const noexcept;

      /* The sequence number the next update pushed here will take (= NextUpdate). */
      [[nodiscard]] inline TSequenceNumber GetNextSequenceNumber() const noexcept;

      /* Force the NextUpdate counter. Used to seed a fresh child/test repo's
         numbering from its parent so reads fold in the right order. */
      inline void SetNextSequenceNumber(TSequenceNumber next_id) noexcept;

      /* Set the high-water mark of updates already promoted to the parent
         (entries at or below this may be dropped on the next mem/disk merge). */
      inline void SetReleasedUpTo(TSequenceNumber released_up_to) noexcept;

      /* Drop the self-pin (MakeDirty) that a write takes until its update is released.  For a
         paused repo whose writes will never be promoted -- a compile-time test pov being thrown
         away (#683) -- nothing else ever drops it, so the repo, its layers and its mappings
         outlive the pov.  The caller must hold its own pointer to the repo, which then closes
         when that pointer goes; with a zero ttl that destroys it, unmerged writes and all. */
      void ReleaseDirtyPin();

      /* Reserve `num` consecutive sequence numbers in one shot and return the
         first; the bulk counterpart of AppendUpdate's per-update bookkeeping. */
      inline TSequenceNumber UseSequenceNumbers(size_t num);

      /* Return the unused tail of a bulk reservation: roll the counters back
         so the repo advertises `next_id - 1` as its highest sequence number.
         The bulk importer over-reserves per file and nothing ever
         materializes the reserved gap, which a joining slave's
         SyncInventory rejects (#498). */
      inline void ReleaseUnusedSequenceNumbers(TSequenceNumber next_id);

      /* The high-water mark set by SetReleasedUpTo / ReleaseUpdate. */
      [[nodiscard]] inline TSequenceNumber GetReleasedUpTo() const noexcept;

      /* The highest sequence number this process knows to be in a data file on disk, the file
         entry included (#750): every update of this repo at or below it survives a crash. Unset
         when none is yet, and always for a repo that never writes disk (a fast repo, or a safe
         one that isn't the global POV's). Advances when a memory merge's file is on disk, and
         starts, on a reload, at the highest sequence number of the files found. */
      [[nodiscard]] std::optional<TSequenceNumber> GetDurableSequenceNumber() const noexcept {
        return ReadDurable(*DurablePlusOne);
      }

      /* The oldest version a read at a version may ask for (#745): every version below it may be missing entries,
         because a merge that pruned or folded history has covered it, or because this process loaded its files from
         disk and cannot tell which were. 0 while no history has been dropped. */
      [[nodiscard]] TSequenceNumber GetHistoryHorizon() const noexcept {
        return HistoryHorizon.load();
      }

      /* Moves the horizon up to `seq`, if it is below it: history below `seq` may be gone, but the state as of `seq`
         is whole (a fold of everything up to it is that state). Call it before publishing whatever drops the history,
         so a reader whose view has the new layers sees the new horizon too. */
      void RaiseHistoryHorizon(TSequenceNumber seq) {
        TSequenceNumber cur = HistoryHorizon.load();
        while (cur < seq && !HistoryHorizon.compare_exchange_weak(cur, seq)) {}
      }

      /* #745 stage 2: how long a read at a version holds that version's history, from its latest read. */
      static constexpr std::chrono::seconds VersionLeaseTtl{600};

      /* Holds the history of version `at` for a read there, while the retention window is on, until VersionLeaseTtl
         after its latest call: a merge keeps it even once the window has moved past it, so a reader paging through
         the version can finish. A merge that started before the lease may still drop it; the read then reports the
         version unavailable. No-op with the window off. */
      void LeaseVersion(TSequenceNumber at);

      /* Sets how long a lease lasts (VersionLeaseTtl by default); tests shorten it. */
      void SetVersionLeaseTtl(std::chrono::steady_clock::duration ttl) {
        std::lock_guard<std::mutex> lock(RetentionLock);
        LeaseTtl = ttl;
      }

      /* The oldest version the next merge must keep whole (see TMergeDataFile and TFoldDataFile), or nothing while
         the retention window is off or for a repo that isn't the global POV: the window's start, lowered to the
         oldest leased version, and never below the horizon, whose older versions are gone already. */
      [[nodiscard]] std::optional<TSequenceNumber> GetRetainFrom();

      /* #745 stage 3: what a merge passes as its retain-from: GetRetainFrom, held to the horizon this repo's retention
         record has on disk, so that a restart, which trusts the record, never claims a version a merge dropped. Moves
         the record along (saving it, and shipping it to the slaves from a master), and on a slave holds to the
         horizon the master ships as well. Without persisted retention (see L0::TManager::SetRetentionPersisted),
         GetRetainFrom itself. */
      [[nodiscard]] std::optional<TSequenceNumber> GetMergeRetainFrom();

      /* #745 stage 3: a named save point keeps every version from `at` on whole, through restarts and on the slaves,
         until it is deleted. Returns once the record holding it is on disk. Throws where there are no records (a
         repo that isn't a root repo, persisted retention off, no store yet, or a slave), or if `at` is a version a
         merge may already have dropped. Blocks the calling thread for the disk write of the record. The protocol's
         `save point` statement (#889) goes through this.

         On a repo with a parent (a POV under review, #746) a save point is the repo's newest own version (or `at`, if
         given), kept in memory for as long as the repo, and good only while the writes after it are still here: once
         Tetris has promoted any of them the save point has lost its meaning, and GetPovSavePoint refuses it. */
      void SetSavePoint(const std::string &name, TSequenceNumber at);
      void SetSavePoint(const std::string &name);

      /* #889: the own version a save point of a repo with a parent names, for a diff of the writes since it. Throws
         std::runtime_error if there is no such save point, or a write after it has been promoted. */
      [[nodiscard]] TSequenceNumber GetPovSavePoint(const std::string &name);

      /* #889: the version the global POV had at `when`, for `at time`. Resolves through the (time, version) samples
         of the --retention_seconds window, to the newest sample at or before `when`, so the answer is up to one
         sample spacing (the greater of a second and the window over 1024) behind the time asked for. Throws
         Server::TTimeUnavailable when `when` is in the future or before the oldest sample, and std::runtime_error
         with no --retention_seconds window, which is all the samples are taken for. */
      [[nodiscard]] TSequenceNumber ResolveTime(std::chrono::system_clock::time_point when);

      /* #889: `hold version`. Keeps the history of `at` for `ttl` (the next merge keeps it, as a lease would, but one
         that a client names and releases), and returns its id. Throws Server::TVersionUnavailable for a version
         outside what can be read, and std::runtime_error with no retention (the window or a save point), where a
         merge drops history as soon as it can and nothing could be kept. */
      [[nodiscard]] Base::TUuid HoldVersion(TSequenceNumber at, std::chrono::seconds ttl);

      /* #889: ends a hold early; false if there was none by that id (never made, expired or already released). */
      bool ReleaseHold(const Base::TUuid &id);

      /* #889: `version of` for a repo with a parent: its newest own version. */
      [[nodiscard]] TSequenceNumber GetNewestSequenceNumber();

      /* For a repo just loaded from disk: raises the horizon to `highest`, the newest version the files hold, as
         nothing says which of them dropped history; the retention record, once read, may lower it again. */
      void NoteLoadedFromDisk(TSequenceNumber highest);

      /* After something raised the history horizon, makes the retention record on disk cover it (#745). */
      void NoteHistoryHorizonRaised();

      /* Deletes a save point; returns once the record without it is on disk. False if there was none by that name. */
      bool DeleteSavePoint(const std::string &name);

      [[nodiscard]] std::map<std::string, TSequenceNumber> GetSavePoints();

      /* The ids of a repo's retention records in the durable store: the node's own (never replicated), and the one
         a master ships to its slaves. Derived from the repo id so they never meet a pov's record (its repo id). */
      static Base::TUuid GetRetentionRecordId(const Base::TUuid &repo_id, bool shipped);

      /* What a retention record holds. */
      struct TRetentionRecord {
        /* Every version from Horizon on is whole on disk: no merge has dropped it. */
        TSequenceNumber Horizon = 0UL;
        /* The window it was saved under (see L0::TManager::SetRetention), for the record. */
        size_t Versions = 0UL, Seconds = 0UL;
        /* (wall-clock time, newest version then) samples of the --retention_seconds window. */
        std::vector<std::pair<std::chrono::system_clock::time_point, TSequenceNumber>> Samples;
        std::map<std::string, TSequenceNumber> SavePoints;

        std::string Encode() const;

        /* False if 'blob' isn't one. */
        static bool TryDecode(const std::string &blob, TRetentionRecord &out);
      };

      /* Cumulative bytes written in memtable flushes (#857). */
      [[nodiscard]] size_t GetFlushBytes() const noexcept {
        return FlushBytes.load(std::memory_order_relaxed);
      }

      /* Cumulative bytes written in disk merges (#857). */
      [[nodiscard]] size_t GetMergeBytes() const noexcept {
        return MergeBytes.load(std::memory_order_relaxed);
      }

      /* Cumulative logical bytes (key + value bytes of promoted updates, #857). */
      [[nodiscard]] size_t GetLogicalBytes() const noexcept {
        return LogicalBytes.load(std::memory_order_relaxed);
      }

      void AddFlushBytes(size_t bytes) noexcept {
        FlushBytes.fetch_add(bytes, std::memory_order_relaxed);
      }

      void AddMergeBytes(size_t bytes) noexcept {
        MergeBytes.fetch_add(bytes, std::memory_order_relaxed);
      }

      void AddLogicalBytes(size_t bytes) noexcept {
        LogicalBytes.fetch_add(bytes, std::memory_order_relaxed);
      }

      /* The durable sequence number's storage, shared so that a promotion into this repo can read
         it after the repo has been deleted (see TPromotionLog). */
      using TDurableCounter = std::atomic<uint64_t>;

      [[nodiscard]] static std::optional<TSequenceNumber> ReadDurable(const TDurableCounter &counter) noexcept {
        const uint64_t value = counter.load(std::memory_order_acquire);
        return value ? std::optional<TSequenceNumber>(value - 1UL) : std::nullopt;
      }

      [[nodiscard]] const std::shared_ptr<TDurableCounter> &GetDurableCounter() const noexcept {
        return DurablePlusOne;
      }

      /* What a child repo records about its updates' promotions into its parent (#750).  It is
         shared, so a promotion can be recorded after the repo itself has been deleted by it. */
      class TPromotionLog {
        public:

        /* An update of the child's, which the parent assigned `parent_seq_num` when promoting it.
           `parent_durable` is the parent's GetDurableSequenceNumber() now; it only lets the
           record drop what it need no longer keep. */
        void Note(TSequenceNumber own_seq_num, TSequenceNumber parent_seq_num, const std::optional<TSequenceNumber> &parent_durable) {
          std::lock_guard<std::mutex> lock(Mutex);
          Promotions.emplace_back(own_seq_num, parent_seq_num);
          Drain(parent_durable);
        }

        [[nodiscard]] std::optional<TSequenceNumber> GetDurable(const std::optional<TSequenceNumber> &parent_durable) {
          std::lock_guard<std::mutex> lock(Mutex);
          Drain(parent_durable);
          return PromotedDurable;
        }

        private:

        void Drain(const std::optional<TSequenceNumber> &parent_durable) {
          while (parent_durable && !Promotions.empty() && Promotions.front().second <= *parent_durable) {
            PromotedDurable = Promotions.front().first;
            Promotions.pop_front();
          }
        }

        /* Pairs of (own, parent) sequence numbers for promotions not yet known to be durable,
           in order. */
        std::deque<std::pair<TSequenceNumber, TSequenceNumber>> Promotions;

        std::optional<TSequenceNumber> PromotedDurable;

        std::mutex Mutex;

      };  // TPromotionLog

      [[nodiscard]] const std::shared_ptr<TPromotionLog> &GetPromotionLog() const noexcept {
        return PromotionLog;
      }

      /* The highest sequence number of this repo's own updates whose promotion into the parent is
         known to be on disk, given the parent's durable sequence number, or unset if none is.
         Only promotions this process made count, so after a restart it can fall short of the
         truth but never exceeds it. */
      [[nodiscard]] std::optional<TSequenceNumber> GetPromotedDurableSequenceNumber(const std::optional<TSequenceNumber> &parent_durable) {
        return PromotionLog->GetDurable(parent_durable);
      }

      /* This repo's parent in the repo tree (mirrors the POV tree), or unset
         for the global root. */
      [[nodiscard]] virtual inline const TParentRepo &GetParentRepo() const noexcept;

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

      /* Conflict tracking for a POV under review (#746; see <orly/indy/fork_watch.h>).

         AddForkWatch registers `watch` on this repo, one of the watched POV's ancestors: from now
         on every update this repo appends is shown to it.  The repo keeps only a weak pointer,
         and forgets the watch once it has gone.

         SetOwnForkWatch makes `watch` this repo's own, the one Tetris tests this repo's updates
         against before it promotes them; the repo keeps it alive.  Set it once, before the repo
         has any updates. */
      void AddForkWatch(const std::shared_ptr<TForkWatch> &watch);

      void SetOwnForkWatch(const std::shared_ptr<TForkWatch> &watch);

      [[nodiscard]] std::shared_ptr<TForkWatch> GetOwnForkWatch() const;

      /* Join parent's Tetris merge if we have unpromoted updates and are not already in Tetris (#755). */
      void JoinTetris();

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

      ~TRepo() override;

      /* Push an update into the memtable (under DataLock): stamp it with the
         next sequence number, advance HighestSeqNum / set LowestSeqNum, insert
         into CurMemoryLayer, and join the parent's Tetris if not already in it.
         Returns the assigned sequence number. */
      std::optional<TSequenceNumber> AppendUpdate(TUpdate *update, TSequenceNumber &next_update,
                                                  const Base::TUuid &promoted_from = Base::TUuid()) noexcept;

      /* Drop the oldest unpopped update by advancing LowestSeqNum (under
         DataLock); when the memtable drains, clears the bounds and Parts Tetris.
         Returns the popped sequence number.  A paused repo may be popped too: that is how a
         discard throws its updates away (#746). */
      std::optional<TSequenceNumber> PopLowest(TSequenceNumber &next_update) noexcept;

      /* Reconstruct the single oldest-unpopped update (the one Tetris Peeks to
         promote to the parent). Holds DataLock across the whole walk (#237). */
      [[nodiscard]] std::shared_ptr<TUpdate> GetLowestUpdate();

      public:

      /* Reconstruct the unpopped update `seq`, the same way; null if the repo doesn't hold it.  How
         a paused repo's updates are copied forward in the WAL (#963). */
      [[nodiscard]] std::shared_ptr<TUpdate> GetUpdate(TSequenceNumber seq);

      private:

      /* Requires DataLock. */
      [[nodiscard]] std::shared_ptr<TUpdate> GetUpdateLocked(TSequenceNumber seq);

      protected:

      /* Flip the repo's status (Normal / Paused / Failed) and join or Part the
         parent's Tetris accordingly. */
      std::optional<TSequenceNumber> ChangeStatus(TStatus, TSequenceNumber &next_update) noexcept;

      /* Mem-merge step: seal CurMemoryLayer into the mapping and start a fresh
         one, then consolidate the trailing memtables. The parentless safe root
         flushes the result to a new disk file; child / fast repos keep it in
         memory, dropping entries already released to the parent. */
      void StepMergeMem() override;

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
      [[nodiscard]] bool HasDiskMergeCandidate(TMapping *mapping) const;

      /* A reloaded repo's first mapping is built from its files in the constructor, which is not
         one of the installs that re-run that gate. Run it here, or a repo that is only read after
         a restart keeps every disk file it came back with. */
      void OnReopened() override;

      /* Copy-on-write a new data layer into the mapping; returns the layer count. */
      size_t AddMapping(TDataLayer *layer);

      /* Pin (Incr) the current mapping so a reader can hold it across a walk. */
      [[nodiscard]] TMapping *AcquireCurrentMapping();

      /* Unpin (Decr) a mapping acquired via AcquireCurrentMapping. */
      void ReleaseMapping(TMapping *mapping);

      /* What a merge holds while it runs (#904). It starts by holding the current mapping, which
         the merge scans for its inputs. Once the merge has chosen them, Narrow takes a reference
         on each input and releases the mapping. A merge reads only its inputs, and a mapping
         references every layer in it, so a merge that held its mapping to the end kept every
         memory layer flushed meanwhile in the pools until it finished, which for a disk merge
         or a flush on a busy root is seconds. */
      class TMergeHold {
        NO_COPY(TMergeHold);
        public:

        explicit TMergeHold(TRepo *repo)
            : Repo(repo), Mapping(repo->AcquireCurrentMapping()) {}

        ~TMergeHold() {
          Release();
        }

        /* The mapping, until Narrow or Release. */
        [[nodiscard]] TMapping *GetMapping() const {
          assert(Mapping);
          return Mapping;
        }

        /* Reference 'layers', which must be in the mapping, and release the mapping. If this
           throws, nothing has changed. */
        template <typename TLayer>
        void Narrow(const std::vector<TLayer *> &layers) {
          assert(Mapping);
          assert(Layers.empty());
          Layers.assign(layers.begin(), layers.end());
          for (TDataLayer *layer : Layers) {
            layer->Incr();
          }
          Repo->ReleaseMapping(Mapping);
          Mapping = nullptr;
        }

        /* Drop whatever is held. Idempotent. */
        void Release() {
          if (Mapping) {
            Repo->ReleaseMapping(Mapping);
            Mapping = nullptr;
          }
          for (TDataLayer *layer : Layers) {
            layer->Decr();
          }
          Layers.clear();
        }

        private:

        TRepo *Repo;

        TMapping *Mapping;

        std::vector<TDataLayer *> Layers;

      };  // TMergeHold

      /* Serializes the mem-merge body of StepMergeMem (one per repo at a time). */
      std::mutex MemMergeLock;

      /* Guards the memtable + sequence-number state (LowestSeqNum,
         HighestSeqNum, NextUpdate, ReleasedUpTo, CurMemoryLayer inserts, Tetris
         join/part). Non-recursive; lock order is DataLock -> MappingLock. */
      std::mutex DataLock;

      /* Serializes appenders against each other and against the seal in StepMergeMem, so that an
         append can link its entries into CurMemoryLayer outside DataLock (readers take DataLock to
         snapshot, and an Insert of a big update held it for ~13 ms).  Lock order: InsertLock ->
         DataLock -> MappingLock.  Nothing holding DataLock may take InsertLock.

         Invariant AppendUpdate relies on (see its comment): a reader sees an update only if its
         sequence number is within [LowestSeqNum, HighestSeqNum] of its snapshot, and AppendUpdate
         advances HighestSeqNum (and NextUpdate) only after the entries are linked, in sequence
         order, because it holds InsertLock from the reservation to the publish. */
      std::mutex InsertLock;

      /* After a mem merge, drop this repo from the manager's dirty set if no
         memtable still holds data; otherwise re-enqueue another mem merge. */
      void CheckRemoveDirty();

      /* #691: a memory merge whose file is written (and in the file map) but whose new mapping
         found no pool room. The merged layers stay taken and keep serving reads until the
         publish goes through. The publish is retried at the start of this repo's next memory
         merge, after a backoff, rather than on the merge runner, which every repo's memory merge
         shares, and where another repo's merge may be what frees the room. Guarded by
         MemMergeLock. */
      std::vector<TMemoryLayer *> UnpublishedMergeLayers;
      TDiskLayer *UnpublishedMergeDisk = nullptr;
      size_t UnpublishedMergeTries = 0UL;

      /* Builds and installs the mapping that replaces 'merged_layers' with 'new_layer' (if any).
         On std::bad_alloc nothing has changed. Call with MemMergeLock held. */
      void PublishMemMerge(const std::vector<TMemoryLayer *> &merged_layers, TDataLayer *new_layer);

      /* #691: counts a failed publish of UnpublishedMergeDisk and queues the retry, or, when the
         manager is shutting down, gives it up (the file stays in the file map). */
      void DeferUnpublishedMemMerge();

      /* The live memtable that AppendUpdate inserts into. */
      TMemoryLayer *CurMemoryLayer;

      /* See GetHistoryHorizon. */
      std::atomic<TSequenceNumber> HistoryHorizon{0UL};

      /* See LeaseVersion and GetRetainFrom. Leases maps each leased version to its expiry; NewestSamples holds
         (time, newest version then) pairs, oldest first, for --retention_seconds. */
      std::mutex RetentionLock;
      std::map<TSequenceNumber, std::chrono::steady_clock::time_point> Leases;

      /* #889: the holds `hold version` made, by id, each a version and its expiry; like Leases, they keep history. And
         the save points of a repo with a parent, which are never persisted. */
      std::map<Base::TUuid, std::pair<TSequenceNumber, std::chrono::steady_clock::time_point>> Holds;
      std::map<std::string, TSequenceNumber> PovSavePoints;
      std::chrono::steady_clock::duration LeaseTtl = VersionLeaseTtl;
      std::deque<std::pair<std::chrono::steady_clock::time_point, TSequenceNumber>> NewestSamples;

      /* #745 stage 3 (see GetMergeRetainFrom), under RetentionLock but for the first. PersistRecords: whether this
         repo keeps a record, as the manager said when it was made. RestartClaim: for a repo loaded from disk, the
         horizon the load set (its newest version), which the record may lower. RecordResolved: the record has been
         looked for; RecordOn: there is one, or the window or a save point is on. DurableHorizon: the horizon of the
         newest record known to be on disk, which no merge may pass. PendingSem/Horizon/Blob: the save in flight, if
         any; PendingSem is pushed when it is on disk. MaxRetainFrom: the highest retain-from handed to a merge. */
      bool PersistRecords = false;
      std::optional<TSequenceNumber> RestartClaim;
      bool RecordResolved = false;
      bool RecordOn = false;
      TSequenceNumber DurableHorizon = 0UL;
      Base::TEventSemaphore *PendingSem = nullptr;
      TSequenceNumber PendingHorizon = 0UL;
      std::string PendingBlob;
      TSequenceNumber MaxRetainFrom = 0UL;
      std::map<std::string, TSequenceNumber> SavePoints;
      bool NotedStuckSave = false;
      bool NotedNoShipped = false;

      /* Takes a (time, newest) sample if the last is a spacing old (see ResolveTime). Call with RetentionLock held. */
      void SampleNewestLocked(std::chrono::steady_clock::time_point now, TSequenceNumber newest, std::chrono::steady_clock::duration window);

      /* GetMergeRetainFrom's parts, under RetentionLock. */
      void ResolveRecordLocked(bool found, const std::string &blob);
      void PollPendingSaveLocked();
      bool StartSaveLocked(TSequenceNumber horizon);

      /* Save the record as it now is and wait for it to reach the disk (SetSavePoint, DeleteSavePoint). */
      void SaveRecordAndWait();

      /* Look the record up, if not yet: outside RetentionLock (the read may park the fiber). */
      void ResolveRecord();

      /* Cumulative bytes written in memtable flushes (#857). */
      std::atomic<size_t> FlushBytes{0UL};

      /* Cumulative bytes written in disk merges (#857). */
      std::atomic<size_t> MergeBytes{0UL};

      /* Cumulative logical bytes (key + value bytes of promoted updates, #857). */
      std::atomic<size_t> LogicalBytes{0UL};

      /* #590: how many merges of each kind in a row have failed for lack of disk space (or, for
         disk merges, pool space: #627). A failed merge hands its inputs back and retries after a
         backoff that doubles with the streak; a merge that succeeds ends the streak. */
      std::atomic<size_t> MergeMemDiskFullStreak{0UL};
      std::atomic<size_t> MergeDiskDiskFullStreak{0UL};

      /* Counts one more failure in 'streak' and returns how long to wait before retrying.
         Logging is rate-limited across all repos. */
      static std::chrono::milliseconds NextDiskFullBackoff(std::atomic<size_t> &streak, const char *merge_kind, const char *err,
                                                           const char *what_ran_out = "disk space");

      /* Ends 'streak' after a successful merge, logging (rate-limited) how many retries it took. */
      static void EndDiskFullStreak(std::atomic<size_t> &streak, const char *merge_kind);

      private:

      /* See SetReadPrepFrameBudget(): the budget, and the prep frames point reads hold now. */
      static std::atomic<size_t> ReadPrepFrameBudget;
      static std::atomic<size_t> ReadPrepFramesInUse;

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
           at most MaxPrepFibers fibers (see TRunnablePrep), or inline when the
           frame pool is empty. exact_point enables seek. */
        TPresentWalker(const std::unique_ptr<TView> &view,
                       const TIndexKey &key,
                       bool ignore_tombstone,
                       bool exact_point);

        ~TPresentWalker() override = default;

        /* True iff. we have an item. */
        [[nodiscard]] explicit operator bool() const noexcept override;

        /* The current item. */
        [[nodiscard]] const TItem &operator*() const noexcept override;

        /* Walk to the next item, if any. */
        TPresentWalker &operator++() override;

        private:

        /* The most fibers one single-key walk fans out to build its per-layer
           sub-walkers. Each prep fiber builds every MaxPrepFibers-th layer, so a
           repo with many disk layers costs a read this many frames, not one per
           layer (#762). */
        static constexpr size_t MaxPrepFibers = 8UL;

        /* Build the sub-walkers of the layers task, task + stride, ... into
           their WalkerVec slots. Run by a prep fiber, or inline by the ctor for
           a task that couldn't get a frame. */
        void BuildLayerWalkers(const std::vector<TDataLayer *> &layers, size_t task, size_t stride);

        /* True iff a walk of the view reads `layer`: one whose every entry is newer than the version the view reads at
           cannot contribute and is passed over (#745). */
        [[nodiscard]] bool Reads(const TDataLayer *layer) const noexcept;

        /* A fiber task that builds some layers' sub-walkers (see
           BuildLayerWalkers) on a frame from the local frame pool, and signals
           the shared TSync barrier on completion -- lets the single-key ctor
           build them concurrently. Move is forbidden (it throws), so the ctor
           reserves its vector of these up front. An exception out of the build
           is kept in Error for the ctor to rethrow after the barrier, so the
           barrier always completes. */
        class TRunnablePrep
            : public Indy::Fiber::TRunnable {
          NO_COPY(TRunnablePrep);
          public:

          TRunnablePrep(TPresentWalker *walker, const std::vector<TDataLayer *> *layers, size_t task, size_t stride, Fiber::TSync *sync)
              : Frame(nullptr), Walker(walker), Layers(layers), Task(task), Stride(stride), Sync(sync) {}

          TRunnablePrep(Orly::Indy::TRepo::TPresentWalker::TRunnablePrep &&) {
            throw std::logic_error("Moving TRunnablePrep is not allowed. This means you did not pre-allocate enough space to hold them.");
          }

          virtual ~TRunnablePrep() = default;

          /* Take a frame and start this task on it, adding it to the barrier.
             False if the frame pool is empty: nothing was started, and the
             caller must build this task's layers itself (#762: the frame
             pool running dry used to unwind the ctor under running preps). */
          bool TryStart() {
            if (ReadPrepFramesInUse.fetch_add(1UL, std::memory_order_relaxed) >= ReadPrepFrameBudget.load(std::memory_order_relaxed)) {
              ReadPrepFramesInUse.fetch_sub(1UL, std::memory_order_relaxed);
              return false;
            }
            /* Not stolen from another thread's pool: those are the frames statements
               returned and are waiting to start with (TJumpRunnable). */
            Frame = Fiber::TFrame::LocalFramePool->TryAlloc(/*steal=*/false);
            if (!Frame) {
              ReadPrepFramesInUse.fetch_sub(1UL, std::memory_order_relaxed);
              return false;
            }
            try {
              Frame->Latch(this, static_cast<Fiber::TRunnable::TFunc>(&TRunnablePrep::Compute));
            } catch (...) {
              Fiber::TFrame::LocalFramePool->Free(Frame);
              ReadPrepFramesInUse.fetch_sub(1UL, std::memory_order_relaxed);
              throw;
            }
            /* Latch only queues the frame on this runner; it can't run (and
               complete the barrier) until the ctor parks in Sync. */
            Sync->WaitForMore(1UL);
            return true;
          }

          /* The exception the build threw, if any. */
          [[nodiscard]] const std::exception_ptr &GetError() const noexcept {
            return Error;
          }

          /* Fiber body: build this task's sub-walkers, signal the barrier, and
             free the frame. */
          void Compute() {
            assert(Walker);
            assert(Layers);
            assert(Sync);
            assert(Fiber::TFrame::LocalFrame == Frame);
            try {
              Walker->BuildLayerWalkers(*Layers, Task, Stride);
            } catch (...) {
              Error = std::current_exception();
            }
            ReadPrepFramesInUse.fetch_sub(1UL, std::memory_order_relaxed);
            Sync->Complete();
            Fiber::FreeMyFrame(Fiber::TFrame::LocalFramePool);
          }

          private:

          /* The fiber frame this task runs on (from the local frame pool). */
          Fiber::TFrame *Frame;

          /* The parent walker whose WalkerVec slots this task fills. */
          TPresentWalker *Walker;

          /* The view's disk layers, in mapping order (owned by the ctor). */
          const std::vector<TDataLayer *> *Layers;

          /* This task builds Layers[Task], Layers[Task + Stride], ... */
          size_t Task;
          size_t Stride;

          /* Barrier the ctor waits on for all prep tasks to finish. */
          Fiber::TSync *Sync;

          /* What the build threw, rethrown by the ctor after the barrier. */
          std::exception_ptr Error;

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

        /* One sub-walker per data layer (disk layers + memtable). The
           single-key ctor fills slot i with layer i's walker, then appends the
           memtable's. */
        std::vector<std::unique_ptr<Indy::TPresentWalker>> WalkerVec;

        /* K-way merge front: the current item of each sub-walker, keyed by its
           WalkerVec index; pops key-ascending, newest-sequence-first. */
        Util::TMinHeap<TItem, size_t> MinHeap;

        /* True while the heap still has items to yield. */
        bool Valid = false;

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

        ~TUpdateWalker() override;

        /* True iff. we have an item. */
        [[nodiscard]] explicit operator bool() const noexcept override;

        /* The current item. */
        [[nodiscard]] const Indy::TUpdateWalker::TItem &operator*() const override;

        /* Walk to the next item, if any. */
        TUpdateWalker &operator++() override;

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
        bool Valid = false;

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

      /* #628: how many entries the unpromoted updates hold, kept for a child repo (the only kind
         Tetris pops), under DataLock. AppendUpdate pushes each update's entry count onto
         BacklogEntryCounts and PopLowest pops it, since the popped update itself is gone by
         then. BacklogEntriesKnown is false while the counts don't cover the whole backlog: a repo
         reconstructed with a backlog, sequence numbers taken by the importer, or a push the
         heap refused. It turns true again when the backlog drains. */
      size_t BacklogEntries = 0UL;
      std::deque<uint32_t> BacklogEntryCounts;
      bool BacklogEntriesKnown = true;

      /* #721: the room writers hold in the backlog (see TBacklogReservation), and how many
         writes bigger than the entry cap are waiting for it; under DataLock. */
      size_t ReservedBacklogUpdates = 0UL;
      size_t ReservedBacklogEntries = 0UL;
      size_t OversizedBacklogWaiters = 0UL;

      /* #721: see GetPeakBacklogEntries. */
      static std::atomic<size_t> PeakBacklogEntries;
      static std::atomic<size_t> PeakBacklogUpdates;

      /* #746: the fork watches registered on this repo (see AddForkWatch), under DataLock. */
      std::vector<std::weak_ptr<TForkWatch>> ForkWatches;

      /* #746: see SetOwnForkWatch, under OwnForkWatchLock. */
      std::shared_ptr<TForkWatch> OwnForkWatch;
      mutable std::mutex OwnForkWatchLock;

      /* #769: brings this repo's share of UnpromotedUpdates and UnpromotedEntries up to date
         with its backlog. Call under DataLock (or before the repo is shared) after anything that
         moves LowestSeqNum, HighestSeqNum or BacklogEntries. A root counts nothing: nothing
         promotes it. */
      void RecountBacklog() noexcept;

      /* #769: this repo's share of the totals, as RecountBacklog last left it; under DataLock. */
      size_t CountedBacklogUpdates = 0UL;
      size_t CountedBacklogEntries = 0UL;

      /* #769: see GetUnpromotedUpdates and GetPromotedUpdates. */
      static std::atomic<size_t> UnpromotedUpdates;
      static std::atomic<size_t> UnpromotedEntries;
      static std::atomic<size_t> PromotedUpdates;
      static std::atomic<size_t> DroppedUpdates;

      protected:

      /* Manager GC hook: visit the parent-repo pointer so it is kept reachable. */
      bool ForEachDependentPtr(const std::function<bool (L0::TManager::TAnyPtr &)> &cb) noexcept override {
        if (ParentRepo) {
          cb(*ParentRepo);
        }
        return true;
      }

      /* High-water mark of updates already promoted to the parent; entries at or
         below it may be dropped on the next mem/disk merge. */
      TSequenceNumber ReleasedUpTo;

      /* Raise the durable sequence number (see GetDurableSequenceNumber) to at least this. */
      void NoteDurable(TSequenceNumber seq_num) {
        uint64_t seen = DurablePlusOne->load(std::memory_order_relaxed);
        while (seen < seq_num + 1UL && !DurablePlusOne->compare_exchange_weak(seen, seq_num + 1UL, std::memory_order_release)) {}
      }

      private:

      const std::shared_ptr<TPromotionLog> PromotionLog = std::make_shared<TPromotionLog>();

      /* One more than the durable sequence number, or 0 for none. */
      const std::shared_ptr<TDurableCounter> DurablePlusOne = std::make_shared<TDurableCounter>(0UL);

      /* Whether this repo is currently a registered child in its parent's Tetris
         merge. Gating Join on !InTetris makes it idempotent and lets a join that
         failed under memory pressure retry on the next AppendUpdate (#250).
         Atomic because ChangeStatus writes it without DataLock: taking DataLock there would
         hold it across TTetrisManager::Part, which waits for the player's mutex, while the
         player holds that mutex to peek this repo under DataLock (#626). */
      std::atomic<bool> InTetris;

      /* The promotion fence (#636).  A Tetris round promotes a child in two steps: it peeks
         the child's lowest update, then commits a transaction that pushes that update to the
         parent and pops it from the child.  A pause that commits in between used to leave the
         round popping a paused repo and parting it from the player a second time.  The round
         can't skip the pop once it is committing (the push to the parent is in the same
         commit), and the pause can't wait for the round from inside its own commit (all
         commits are serialized on the replication queue lock, which the round needs too).
         So the two meet here, before either commits:

           * a transaction that peeks this repo takes a promotion hold (TryHoldForPromotion),
             released when that transaction's popper is destroyed, after its commit;
           * a pause first registers itself (BeginPause), from which point no new hold is
             granted, then waits for the holds already taken to be released, then commits;
             EndPause after the commit lets Tetris peek the repo again.

         A round that has already peeked therefore finishes, pop included, before the pause
         commits, and a round that peeks later sees the pause (or, once it has committed, no
         longer has the repo as a child).  Plain atomics, not a lock: the hold is taken with
         the player's mutex held, and the pause's commit takes that mutex (Part -> OnPart). */
      [[nodiscard]] inline bool TryHoldForPromotion() noexcept;

      inline void ReleasePromotionHold() noexcept;

      inline void BeginPause() noexcept;

      [[nodiscard]] inline bool IsHeldForPromotion() const noexcept;

      inline void EndPause() noexcept;

      /* Promotion holds currently taken, and pauses registered (see above). */
      std::atomic<size_t> PromotionHoldCount;
      std::atomic<size_t> PauseCount;

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

      ~TFastRepo() override;

      private:

      /* false -- a fast repo is memory-only. */
      [[nodiscard]] bool IsSafeRepo() const noexcept override;

      /* Unreachable: a fast repo has no disk files. Asserts false. */
      void StepMergeDisk(size_t block_slots_available) override;

      /* Unreachable: a fast repo has no disk tail. Asserts false. */
      void StepTail(size_t block_slots_available) override;

      /* Unreachable: a fast repo cannot host synced files. Throws. */
      size_t AddSyncedFileToRepo(size_t starting_block_id,
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

      ~TSafeRepo() override;

      /* The most files one disk merge takes in (#968). Below 2M keys, GenSizeSet grows 8x a step, so a
         run of this many files in one set makes a file in the next, and its data is written once on
         the way rather than once per pairwise step. */
      static constexpr size_t MaxDiskMergeInputs = 8;

      /* Compact a run of adjacent disk files, each in the same or a higher generation than the one
         before it, into one new file. At the root, the
         merge also drops superseded versions (#592). */
      void StepMergeDisk(size_t block_slots_available) override;

      /* Trim the oldest disk generation below the release horizon (root only). */
      void StepTail(size_t block_slots_available) override;

      /* Rebuild a TSafeRepo from its on-disk generation files (static factory). */
      static TSafeRepo *ReConstructFromDisk(L0::TManager *manager,
                                            const Base::TUuid &repo_id,
                                            const TDeadline &deadline);

      protected:

      /* Allocate the next monotonic generation id (++NextGenId). */
      [[nodiscard]] inline size_t GetNextGenId() noexcept;

      /* Test only. When false, MergeFiles keeps a merge's output unfolded, `+=` history and all,
         so a test can read a TMergeDataFile back directly (#666). Nothing else serves one. */
      bool FoldMergedFiles = true;

      private:

      /* true -- a safe repo is disk-backed. */
      [[nodiscard]] bool IsSafeRepo() const noexcept override;

      /* Merge the given disk generations into one new file. Two-phase: a raw
         merge, then a commutative fold (TFoldDataFile) if any non-Assign entries
         are present. can_tail drops superseded versions, and can_tail_tombstone
         also drops tombstones (single-file merges only). Both apply only at the
         root. The callers check their own flags: --allow_tailing for StepTail,
         --prune_merge_history for StepMergeDisk. release_up_to plays no part in
         either. Returns the new generation id. */
      size_t MergeFiles(const std::vector<size_t> &gen_id_vec,
                        Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                        size_t max_block_cache_read_slots_allowed,
                        size_t temp_file_consol_thresh,
                        TSequenceNumber &out_saved_low_seq,
                        TSequenceNumber &out_saved_high_seq,
                        size_t &out_num_keys,
                        TSequenceNumber release_up_to,
                        bool can_tail,
                        bool can_tail_tombstone,
                        bool cancel_on_shutdown = false) override;

      /* Make the mapping with merged_layer in place of input_layers current, and mark the inputs
         for delete (#627). The inputs must be taken and in the current mapping. If an allocation
         fails, the half-built mapping is deleted before MappingLock is released, so the repo is
         as it was, and the std::bad_alloc propagates. */
      void PublishDiskMerge(const std::vector<TDiskLayer *> &input_layers, TDiskLayer *merged_layer);

      /* Undo a disk merge whose file is written but couldn't be published (#627): remove the
         file, delete merged_layer, and hand input_layers back. No reader has seen the file, and
         the inputs still hold all of its data. */
      void DiscardDiskMerge(const std::vector<TDiskLayer *> &input_layers, TDiskLayer *merged_layer);

      /* Reclaim a generation file's blocks, first dropping its per-scheduler
         caches on every runner unless the caller already has. */
      void RemoveFile(size_t gen_id, bool caches_cleared) override;

      /* Drop the calling runner's cached readers/walkers for a file. */
      void ClearLocalFileCaches(size_t gen_id) override;

      /* Serialize a memtable to a new on-disk generation file; returns its
         generation id (and, by reference, its saved sequence range / key count). */
      size_t WriteFile(TMemoryLayer *memory_layer,
                       Disk::Util::TVolume::TDesc::TStorageSpeed storage_speed,
                       TSequenceNumber &out_saved_low_seq,
                       TSequenceNumber &out_saved_high_seq,
                       size_t &out_num_keys,
                       TSequenceNumber release_up_to) override;

      /* Register a replication-synced file: allocate a gen_id, hand the blocks to
         the disk engine, then AddFileToRepo it. */
      size_t AddSyncedFileToRepo(size_t starting_block_id,
                                 size_t starting_block_offset,
                                 size_t file_length,
                                 TSequenceNumber low_saved,
                                 TSequenceNumber high_saved,
                                 size_t num_keys) override;

      /* Open a present-walker over a single on-disk generation file (range). */
      std::unique_ptr<Orly::Indy::TPresentWalker> NewPresentWalkerFile(size_t /*gen_id*/,
                                                                       const TIndexKey &/*from*/,
                                                                       const TIndexKey &/*to*/,
                                                                       std::optional<TSequenceNumber> /*at*/ = std::nullopt) const override;

      /* Open a present-walker over a single on-disk generation file (single key). */
      std::unique_ptr<Orly::Indy::TPresentWalker> NewPresentWalkerFile(size_t /*gen_id*/,
                                                                       const TIndexKey &/*key*/,
                                                                       std::optional<TSequenceNumber> /*at*/ = std::nullopt) const override;

      /* Open an update-walker over a single on-disk generation file. */
      std::unique_ptr<Orly::Indy::TUpdateWalker> NewUpdateWalkerFile(size_t /*gen_id*/, TSequenceNumber /*from*/) const override;

      /* The monotonic generation-id counter (seeded from the highest existing
         file generation at construction). */
      std::atomic<size_t> NextGenId;

      /* Serializes selection/marking of disk layers for compaction and tailing. */
      std::mutex MergeLock;

    };  // TSafeRepo

    /***************
      *** inline ***
      *************/

    inline size_t TRepo::TView::GetNumEntries() const noexcept {
      size_t count = 0UL;
      for (TMapping::TEntryCollection::TCursor mapping_csr(Mapping->GetEntryCollection()); mapping_csr; ++mapping_csr, ++count) {}
      return count;
    }

    inline void TRepo::TView::CountLayers(size_t &disk_layers, size_t &mem_layers) const noexcept {
      disk_layers = 0UL;
      mem_layers = 0UL;
      for (TMapping::TEntryCollection::TCursor mapping_csr(Mapping->GetEntryCollection()); mapping_csr; ++mapping_csr) {
        ++(mapping_csr->GetLayer()->GetKind() == TDataLayer::Disk ? disk_layers : mem_layers);
      }
    }

    inline void TRepo::GetSnapshot(std::optional<TSequenceNumber> &seq_num_start,
                                   std::optional<TSequenceNumber> &seq_num_limit,
                                   TSequenceNumber &next_seq_num) {
      std::lock_guard<std::mutex> lock(DataLock);
      seq_num_start = LowestSeqNum;
      seq_num_limit = HighestSeqNum;
      next_seq_num = NextUpdate;
    }

    inline bool TRepo::TryHoldForPromotion() noexcept {
      /* Dekker-style with BeginPause/IsHeldForPromotion, so both sides are seq_cst: either we
         see the pause and back out, or the pause sees our hold and waits for it. */
      if (PauseCount.load()) {
        return false;
      }
      ++PromotionHoldCount;
      if (PauseCount.load()) {
        --PromotionHoldCount;
        return false;
      }
      return true;
    }

    inline void TRepo::ReleasePromotionHold() noexcept {
      assert(PromotionHoldCount.load());
      --PromotionHoldCount;
    }

    inline void TRepo::BeginPause() noexcept {
      ++PauseCount;
    }

    inline bool TRepo::IsHeldForPromotion() const noexcept {
      return PromotionHoldCount.load() != 0UL;
    }

    inline void TRepo::EndPause() noexcept {
      assert(PauseCount.load());
      --PauseCount;
    }

    inline size_t TRepo::GetMemBacklogDepth() {
      std::lock_guard<std::mutex> lock(DataLock);
      if (!LowestSeqNum || !HighestSeqNum || *HighestSeqNum < *LowestSeqNum) {
        return 0UL;
      }
      return static_cast<size_t>(*HighestSeqNum - *LowestSeqNum) + 1UL;
    }

    inline bool TRepo::IsBacklogDraining() {
      /* ParentRepo is set at construction. */
      return ParentRepo && InTetris.load();
    }

    inline size_t TRepo::GetMemBacklogEntries() {
      std::lock_guard<std::mutex> lock(DataLock);
      return BacklogEntriesKnown ? BacklogEntries : 0UL;
    }

    inline const std::optional<TSequenceNumber> &TRepo::GetSequenceNumberStart() const noexcept {
      return LowestSeqNum;
    }

    inline const std::optional<TSequenceNumber> &TRepo::GetSequenceNumberLimit() const noexcept {
      return HighestSeqNum;
    }

    inline TSequenceNumber TRepo::GetNextSequenceNumber() const noexcept {
      return NextUpdate;
    }

    inline void TRepo::SetNextSequenceNumber(TSequenceNumber next_id) noexcept {
      std::lock_guard<std::mutex> insert_lock(InsertLock);
      NextUpdate = next_id;
    }

    inline void TRepo::SetReleasedUpTo(TSequenceNumber released_up_to) noexcept {
      ReleasedUpTo = released_up_to;
    }

    inline void TRepo::ReleaseUnusedSequenceNumbers(TSequenceNumber next_id) {
      std::lock_guard<std::mutex> insert_lock(InsertLock);
      std::lock_guard<std::mutex> lock(DataLock);
      assert(next_id > 0);
      assert(next_id <= NextUpdate);
      NextUpdate = next_id;
      HighestSeqNum = next_id - 1;
      RecountBacklog();
    }

    inline TSequenceNumber TRepo::UseSequenceNumbers(size_t num) {
      assert(num);
      std::lock_guard<std::mutex> insert_lock(InsertLock);
      std::lock_guard<std::mutex> lock(DataLock);
      TSequenceNumber starting = NextUpdate;
      NextUpdate += num;
      HighestSeqNum = NextUpdate - 1;
      if (!LowestSeqNum) {
        LowestSeqNum = starting;
      }
      /* The importer's updates bypass AppendUpdate (#628). */
      BacklogEntriesKnown = false;
      RecountBacklog();
      return starting;
    }

    inline TSequenceNumber TRepo::GetReleasedUpTo() const noexcept {
      return ReleasedUpTo;
    }

    inline const TRepo::TParentRepo &TRepo::GetParentRepo() const noexcept {
      return ParentRepo;
    }

    inline TRepo::TPresentWalker::operator bool() const noexcept {
      return Valid;
    }

    inline const TPresentWalker::TItem &TRepo::TPresentWalker::operator*() const noexcept {
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
        const Indy::TPresentWalker::TItem &cur_item = MinHeap.Peek(pos);
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
          MinHeap.ReplaceTop(*walker);
        } else {
          MinHeap.Pop(pos);
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
        const Indy::TPresentWalker::TItem &cur_item = MinHeap.Peek(pos);
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
          MinHeap.ReplaceTop(*walker);
        } else {
          MinHeap.Pop(pos);
        }
        if (done) {
          break;
        } else {
          Valid = static_cast<bool>(MinHeap);
        }
      }
    }

    inline size_t TSafeRepo::GetNextGenId() noexcept {
      return ++NextGenId;
    }

  }  // Indy

}  // Orly