/* <orly/indy/update.h>

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
#include <cassert>

#include <base/class_traits.h>
#include <base/inv_con/ordered_list.h>
#include <orly/atom/kit2.h>
#include <orly/atom/suprena.h>
#include <orly/indy/key.h>
#include <orly/indy/sequence_number.h>
#include <orly/indy/util/pool.h>
#include <orly/sabot/all.h>
#include <orly/sabot/assert_tuple.h>
#include <orly/shared_enum.h>

namespace Orly {

  namespace Server {

    /* Forward Declarations. */
    class TServer;
    class TIndyReporter;

  }

  namespace Indy {

    /* Forward Declarations. */
    class TMemoryLayer;

    namespace L1 {

      /* Forward Declarations. */
      class TTransaction;

    }  // L1

    class TUpdate {
      NO_COPY(TUpdate);
      public:

      /* Forward Declarations. */
      class TEntry;

      using TOpByKey = std::map<TIndexKey, TKey>;

      using TEntryCollection = InvCon::OrderedList::TCollection<TUpdate, TEntry, TKey>;

      ~TUpdate();

      class TPersistenceNotification {
        NO_COPY(TPersistenceNotification);
        public:

        enum TResult {
          Completed,
          Failed
        };

        explicit TPersistenceNotification(std::function<void (TResult)> cb);

        ~TPersistenceNotification();

        inline void Call(TPersistenceNotification::TResult result);

        private:

        std::function<void (TResult)> Cb;

      };  // TPersistenceNotification

      class TEntry {
        NO_COPY(TEntry);
        public:

        /* Height of the skip-list seek accelerator (#257): the number of
           express lanes over a memory layer's EntryCollection. Each TEntry
           carries one forward pointer per lane it participates in; lookups
           descend the lanes to skip most of the linear scan. See
           TMemoryLayer::LinkEntry / SeekRun. */
        static constexpr size_t SkipMaxLevel = 16;

        [[nodiscard]] inline const TKey &GetKey() const noexcept;

        [[nodiscard]] inline const TIndexKey &GetIndexKey() const noexcept;

        [[nodiscard]] inline const Atom::TCore &GetOp() const noexcept;

        /* The mutator used to produce Op. For assignments (the historical
           default and current observable behavior for all in-tree code
           paths) this is TMutator::Assign and Op is the resolved value.
           For commutative ops (Add, Mult, Or, And, Xor, Union, Intersection,
           SymmetricDiff) this is the mutator that produced the entry and
           Op is the right-hand side -- the read path will then accumulate
           same-mutator entries on the same key.

           Phase 1 of #49 added this field but every in-tree write site
           still passes Assign by default, so observable behavior is
           unchanged until phase 2 (session.cc emits the real mutator). */
        [[nodiscard]] inline TMutator GetMutator() const noexcept;

        [[nodiscard]] inline TSequenceNumber GetSequenceNumber() const;

        [[nodiscard]] inline const Atom::TCore &GetMetadata() const;

        [[nodiscard]] inline const Atom::TCore &GetId() const;

        [[nodiscard]] inline Atom::TSuprena &GetSuprena() const;

        [[nodiscard]] inline const TUpdate *GetUpdate() const noexcept;

        static void *operator new(size_t size) {
          return Pool.Alloc(size);
        }

        static void operator delete(void *ptr, size_t) {
          Pool.Free(ptr);
        }

        private:

        class TEntryKey {
          public:

          explicit TEntryKey(const TEntry *entry);

          [[nodiscard]] bool operator==(const TEntryKey &that) const;

          [[nodiscard]] bool operator!=(const TEntryKey &that) const;

          [[nodiscard]] bool operator<=(const TEntryKey &that) const;

          [[nodiscard]] bool operator>(const TEntryKey &that) const;

          private:

          const TEntry *Entry = nullptr;

        };  // TEntryKey

        using TUpdateMembership = InvCon::OrderedList::TMembership<TEntry, TUpdate, TKey>;
        /* Published links: a memory layer is read while it is written (see TMemoryLayer). */
        using TMemoryLayerMembership = InvCon::OrderedList::TMembership<TEntry, TMemoryLayer, TEntryKey, InvCon::OrderedList::TPublishedLinks>;

        /* Construct an entry. The mutator defaults to TMutator::Assign,
           which is the historical / current behavior for every in-tree
           write path. Phase 2 of #49 (session.cc) is where non-Assign
           mutators start flowing through. */
        TEntry(TUpdate *update, const TIndexKey &key, const TKey &op, void *state_alloc,
               TMutator mutator = TMutator::Assign);

        [[nodiscard]] inline const TEntryKey &GetEntryKey() const noexcept;

        TIndexKey IndexKey;

        TUpdateMembership::TImpl UpdateMembership;

        TMemoryLayerMembership::TImpl MemoryLayerMembership;

        /* Express-lane forward pointers for the layer's skip-list seek
           accelerator (#257). Index l is express lane l+1 over the layer's
           EntryCollection (level 0). Written by TMemoryLayer::LinkEntry with
           release ordering (single-writer per layer); read with acquire by
           SeekRun. Lanes above this entry's chosen height stay null.
           A best-effort accelerator only -- EntryCollection remains the
           authoritative ordered list, so a stale read just walks a little
           more of level 0, never a wrong result. */
        std::atomic<TEntry *> SkipFwd[SkipMaxLevel];

        Atom::TCore Op;

        /* See GetMutator(). Defaults to TMutator::Assign for every
           in-tree write path until phase 2 of #49 lands. */
        TMutator Mutator = TMutator::Assign;

        static Util::TPool Pool;

        friend class TMemoryLayer;
        friend class TManager;
        friend class TUpdate;
        friend class Orly::Server::TServer;
        friend class Orly::Server::TIndyReporter;

      };  // TEntry

      [[nodiscard]] inline TSequenceNumber GetSequenceNumber() const;

      [[nodiscard]] inline const Atom::TCore &GetId() const noexcept;

      [[nodiscard]] inline const Atom::TCore &GetMetadata() const noexcept;

      [[nodiscard]] inline Atom::TSuprena &GetSuprena() const noexcept;

      [[nodiscard]] inline TEntryCollection *GetEntryCollection() const noexcept;

      [[nodiscard]] inline const std::shared_ptr<TPersistenceNotification> &GetPersistenceNotification() const noexcept;

      /* The logical payload bytes in this update's entries (key + value cores, plus arena notes; #857). */
      [[nodiscard]] size_t ComputeLogicalBytes() const noexcept;

      inline void SetSequenceNumber(TSequenceNumber seq_num);

      inline void SetMetadata(const TKey &metadata);

      inline void SetId(const TKey &id);

      inline void SetPersistenceNotification(const std::function<void (TPersistenceNotification::TResult)> &cb);

      inline TEntry *AddEntry(const TIndexKey &key, const TKey &op);

      /* Add an entry that carries a mutator. The default-mutator
         overload above forwards to this with TMutator::Assign so all
         existing call sites stay behaviorally identical. */
      inline TEntry *AddEntry(const TIndexKey &key, const TKey &op, TMutator mutator);

      static std::shared_ptr<TUpdate> NewUpdate(const TOpByKey &op_by_key, const TKey &metadata, const TKey &id);

      static std::shared_ptr<TUpdate> ReconstructUpdate(TSequenceNumber seq_num);

      static TUpdate *CopyUpdate(TUpdate *that, void *state_alloc);

      static TUpdate *ShallowCopy(TUpdate *that, void *state_alloc);

      static inline void *operator new(size_t size) {
        return Pool.Alloc(size);
      }

      static inline void operator delete(void *ptr, size_t) {
        Pool.Free(ptr);
      }

      static void InitUpdatePool(size_t num_obj) {
        Pool.Init(num_obj);
      }

      static void InitEntryPool(size_t num_obj) {
        TEntry::Pool.Init(num_obj);
      }

      [[nodiscard]] static inline double GetUpdatePoolUsedPct() {
        return static_cast<double>(Pool.GetNumBlocksUsed()) / Pool.GetMaxBlocks();
      }

      [[nodiscard]] static inline size_t GetUpdatePoolMaxBlocks() noexcept {
        return Pool.GetMaxBlocks();
      }

      [[nodiscard]] static inline double GetUpdateEntryPoolUsedPct() {
        return static_cast<double>(TEntry::Pool.GetNumBlocksUsed()) / TEntry::Pool.GetMaxBlocks();
      }

      /* Writer admission on the update pools (#607). A write with num_entries entries takes two
         updates (the one it builds, and the transaction's copy that goes into the memtable) and
         twice as many entries. TryAcquire promises those blocks to the write, or returns false
         without promising anything if either pool would dip into its reserve (TPool::TryAdmit);
         the promise is released when this object goes away, once the write has committed. */
      class TWriteAdmission {
        NO_COPY(TWriteAdmission);
        public:

        TWriteAdmission() = default;

        ~TWriteAdmission();

        [[nodiscard]] bool TryAcquire(size_t num_entries);

        /* After a refused TryAcquire (#719): the pool that refused (the Update Entry pool, or
           else the Update pool), and the counts it compared. */
        [[nodiscard]] bool WasEntryPoolRefused() const noexcept {
          return EntryPoolRefused;
        }

        [[nodiscard]] const Util::TPool::TRefusal &GetRefusal() const noexcept {
          return Refusal;
        }

        /* After a refused TryAcquire (#765): arms the room watch (TPool::ArmRoomWake) of the
           pool that refused, at the level that would have admitted this write. Returns false
           if the watch was already armed at that level, so a caller that retries after arming
           knows when a further retry can't help until the watch fires. */
        [[nodiscard]] bool ArmRoomWake() const;

        /* Disarms both pools' room watches. */
        static void DisarmRoomWakes();

        private:

        size_t NumUpdates = 0UL;

        size_t NumEntries = 0UL;

        bool EntryPoolRefused = false;

        Util::TPool::TRefusal Refusal;

      };  // TWriteAdmission

      /* A copy's claim on both update pools (#607; see TPool::TryClaim): TryAcquire claims
         num_updates updates and num_entries entries together or not at all, and the claim is
         released by Release or when this object goes away. */
      class TCopyClaim {
        NO_COPY(TCopyClaim);
        public:

        TCopyClaim() = default;

        ~TCopyClaim();

        /* keep_reserve: see TPool::TryClaim. */
        [[nodiscard]] bool TryAcquire(size_t num_updates, size_t num_entries, bool keep_reserve = false);

        /* Gives back part of the claim, for blocks the copy has now allocated (they count as in
           use from here). Never more than is held. */
        void ReleaseSome(size_t num_updates, size_t num_entries);

        void Release();

        private:

        size_t NumUpdates = 0UL;

        size_t NumEntries = 0UL;

      };  // TCopyClaim

      /* Sets both pools' reserves to pct percent of their size (#607); 0 turns refusals off. */
      static void SetPoolReservePct(size_t pct);

      [[nodiscard]] static inline const Util::TPool &GetUpdatePool() noexcept {
        return Pool;
      }

      [[nodiscard]] static inline const Util::TPool &GetEntryPool() noexcept {
        return TEntry::Pool;
      }

      protected:

      TUpdate(const TOpByKey &op_by_key, const TKey &metadata, const TKey &id, void *state_alloc);

      explicit TUpdate(const TUpdate *that, void *state_alloc);

      /* Shallow Copy! */
      explicit TUpdate(void *state_alloc, const TUpdate *that);

      explicit TUpdate(TSequenceNumber);

      private:

      /* Published links: a memory layer is read while it is written (see TMemoryLayer). */
      using TMemoryLayerMembership = InvCon::OrderedList::TMembership<TUpdate, TMemoryLayer, TSequenceNumber, InvCon::OrderedList::TPublishedLinks>;

      mutable Atom::TSuprena Suprena;

      mutable TEntryCollection::TImpl EntryCollection;

      TMemoryLayerMembership::TImpl MemoryLayerMembership;

      Atom::TCore Metadata;

      Atom::TCore Id;

      std::shared_ptr<TPersistenceNotification> PersistenceNotification;

      static Util::TPool Pool;

      friend class TMemoryLayer;
      friend class TManager;
      friend class L1::TTransaction;
      friend class Orly::Server::TServer;
      friend class Orly::Server::TIndyReporter;

    };  // TUpdate

    inline void TUpdate::TPersistenceNotification::Call(TPersistenceNotification::TResult result) {
      Cb(result);
    }

    inline TSequenceNumber TUpdate::GetSequenceNumber() const {
      return MemoryLayerMembership.GetKey();
    }

    inline const Atom::TCore &TUpdate::GetId() const noexcept {
      return Id;
    }

    inline const Atom::TCore &TUpdate::GetMetadata() const noexcept {
      return Metadata;
    }

    inline Atom::TSuprena &TUpdate::GetSuprena() const noexcept {
      return Suprena;
    }

    inline void TUpdate::SetSequenceNumber(TSequenceNumber seq_num) {
      MemoryLayerMembership.SetKey(seq_num);
    }

    inline void TUpdate::SetMetadata(const TKey &metadata) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      Metadata = Atom::TCore(&Suprena, Sabot::State::TAny::TWrapper(metadata.GetCore().NewState(metadata.GetArena(), state_alloc)));
    }

    inline void TUpdate::SetId(const TKey &id) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      Id = Atom::TCore(&Suprena, Sabot::State::TAny::TWrapper(id.GetCore().NewState(id.GetArena(), state_alloc)));
    }

    inline void TUpdate::SetPersistenceNotification(const std::function<void (TPersistenceNotification::TResult)> &cb) {
      PersistenceNotification = std::make_shared<TPersistenceNotification>(cb);
    }

    inline TUpdate::TEntry *TUpdate::AddEntry(const TIndexKey &index_key, const TKey &op) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      return new TEntry(this, index_key, op, state_alloc);
    }

    inline TUpdate::TEntry *TUpdate::AddEntry(const TIndexKey &index_key, const TKey &op, TMutator mutator) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      return new TEntry(this, index_key, op, state_alloc, mutator);
    }

    inline const TKey &TUpdate::TEntry::GetKey() const noexcept {
      return IndexKey.GetKey();
    }

    inline const TIndexKey &TUpdate::TEntry::GetIndexKey() const noexcept {
      return IndexKey;
    }

    inline const Atom::TCore &TUpdate::TEntry::GetOp() const noexcept {
      return Op;
    }

    inline TMutator TUpdate::TEntry::GetMutator() const noexcept {
      return Mutator;
    }

    inline TSequenceNumber TUpdate::TEntry::GetSequenceNumber() const {
      assert(UpdateMembership.TryGetCollector());
      return UpdateMembership.TryGetCollector()->MemoryLayerMembership.GetKey();
    }

    inline const Atom::TCore &TUpdate::TEntry::GetMetadata() const {
      assert(UpdateMembership.TryGetCollector());
      return UpdateMembership.TryGetCollector()->Metadata;
    }

    inline const Atom::TCore &TUpdate::TEntry::GetId() const {
      assert(UpdateMembership.TryGetCollector());
      return UpdateMembership.TryGetCollector()->Id;
    }

    inline Atom::TSuprena &TUpdate::TEntry::GetSuprena() const {
      assert(UpdateMembership.TryGetCollector());
      return UpdateMembership.TryGetCollector()->Suprena;
    }

    inline const TUpdate *TUpdate::TEntry::GetUpdate() const noexcept {
      assert(UpdateMembership.TryGetCollector());
      return UpdateMembership.TryGetCollector();
    }

    inline const TUpdate::TEntry::TEntryKey &TUpdate::TEntry::GetEntryKey() const noexcept {
      return MemoryLayerMembership.GetKey();
    }

    inline TUpdate::TEntryCollection *TUpdate::GetEntryCollection() const noexcept {
      return &EntryCollection;
    }

    inline const std::shared_ptr<TUpdate::TPersistenceNotification> &TUpdate::GetPersistenceNotification() const noexcept {
      return PersistenceNotification;
    }

  }  // Indy

}  // Orly