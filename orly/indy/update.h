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

    /* TODO */
    class TUpdate {
      NO_COPY(TUpdate);
      public:

      /* Forward Declarations. */
      class TEntry;

      /* TODO */
      typedef std::map<TIndexKey, TKey> TOpByKey;

      /* TODO */
      typedef InvCon::OrderedList::TCollection<TUpdate, TEntry, TKey> TEntryCollection;

      /* TODO */
      ~TUpdate();

      /* TODO */
      class TPersistenceNotification {
        NO_COPY(TPersistenceNotification);
        public:

        enum TResult {
          Completed,
          Failed
        };

        /* TODO */
        TPersistenceNotification(const std::function<void (TResult)> &cb);

        /* TODO */
        ~TPersistenceNotification();

        /* TODO */
        inline void Call(TUpdate::TPersistenceNotification::TResult result);

        private:

        /* TODO */
        std::function<void (TResult)> Cb;

      };  // TPersistenceNotification

      /* TODO */
      class TEntry {
        NO_COPY(TEntry);
        public:

        /* Height of the skip-list seek accelerator (#257): the number of
           express lanes over a memory layer's EntryCollection. Each TEntry
           carries one forward pointer per lane it participates in; lookups
           descend the lanes to skip most of the linear scan. See
           TMemoryLayer::SkipInsert / SeekRun. */
        static constexpr size_t SkipMaxLevel = 16;

        /* TODO */
        inline const TKey &GetKey() const;

        /* TODO */
        inline const TIndexKey &GetIndexKey() const;

        /* TODO */
        inline const Atom::TCore &GetOp() const;

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
        inline TMutator GetMutator() const;

        /* TODO */
        inline TSequenceNumber GetSequenceNumber() const;

        /* TODO */
        inline const Atom::TCore &GetMetadata() const;

        /* TODO */
        inline const Atom::TCore &GetId() const;

        /* TODO */
        inline Atom::TSuprena &GetSuprena() const;

        /* TODO */
        inline const TUpdate *GetUpdate() const;

        /* TODO */
        static void *operator new(size_t size) {
          return Pool.Alloc(size);
        }

        /* TODO */
        static void operator delete(void *ptr, size_t) {
          Pool.Free(ptr);
        }

        private:

        /* TODO */
        class TEntryKey {
          public:

          /* TODO */
          TEntryKey(const TEntry *entry);

          /* TODO */
          bool operator==(const TEntryKey &that) const;

          /* TODO */
          bool operator!=(const TEntryKey &that) const;

          /* TODO */
          bool operator<=(const TEntryKey &that) const;

          /* TODO */
          bool operator>(const TEntryKey &that) const;

          private:

          /* TODO */
          const TEntry *Entry;

        };  // TKey

        /* TODO */
        typedef InvCon::OrderedList::TMembership<TEntry, TUpdate, TKey> TUpdateMembership;
        typedef InvCon::OrderedList::TMembership<TEntry, TMemoryLayer, TEntryKey> TMemoryLayerMembership;

        /* Construct an entry. The mutator defaults to TMutator::Assign,
           which is the historical / current behavior for every in-tree
           write path. Phase 2 of #49 (session.cc) is where non-Assign
           mutators start flowing through. */
        TEntry(TUpdate *update, const TIndexKey &key, const TKey &op, void *state_alloc,
               TMutator mutator = TMutator::Assign);

        /* TODO */
        inline const TEntryKey &GetEntryKey() const;

        /* TODO */
        TIndexKey IndexKey;

        /* TODO */
        TUpdateMembership::TImpl UpdateMembership;

        /* TODO */
        TMemoryLayerMembership::TImpl MemoryLayerMembership;

        /* Express-lane forward pointers for the layer's skip-list seek
           accelerator (#257). Index l is express lane l+1 over the layer's
           EntryCollection (level 0). Written by TMemoryLayer::SkipInsert with
           release ordering (single-writer per layer); read with acquire by
           SkipFindFirstGE. Lanes above this entry's chosen height stay null.
           A best-effort accelerator only -- EntryCollection remains the
           authoritative ordered list, so a stale read just walks a little
           more of level 0, never a wrong result. */
        std::atomic<TEntry *> SkipFwd[SkipMaxLevel];

        /* TODO */
        Atom::TCore Op;

        /* See GetMutator(). Defaults to TMutator::Assign for every
           in-tree write path until phase 2 of #49 lands. */
        TMutator Mutator;

        /* TODO */
        static Util::TPool Pool;

        /* TODO */
        friend class TMemoryLayer;
        friend class TManager;
        friend class TUpdate;
        friend class Orly::Server::TServer;
        friend class Orly::Server::TIndyReporter;

      };  // TEntry

      /* TODO */
      inline TSequenceNumber GetSequenceNumber() const;

      /* TODO */
      inline const Atom::TCore &GetId() const;

      /* TODO */
      inline const Atom::TCore &GetMetadata() const;

      /* TODO */
      inline Atom::TSuprena &GetSuprena() const;

      /* TODO */
      inline TEntryCollection *GetEntryCollection() const;

      /* TODO */
      inline const std::shared_ptr<TPersistenceNotification> &GetPersistenceNotification() const;

      /* TODO */
      inline void SetSequenceNumber(TSequenceNumber seq_num);

      /* TODO */
      inline void SetMetadata(const TKey &metadata);

      /* TODO */
      inline void SetId(const TKey &id);

      /* TODO */
      inline void SetPersistenceNotification(const std::function<void (TPersistenceNotification::TResult)> &cb);

      /* TODO */
      inline TEntry *AddEntry(const TIndexKey &key, const TKey &op);

      /* Add an entry that carries a mutator. The default-mutator
         overload above forwards to this with TMutator::Assign so all
         existing call sites stay behaviorally identical. */
      inline TEntry *AddEntry(const TIndexKey &key, const TKey &op, TMutator mutator);

      /* TODO */
      static std::shared_ptr<TUpdate> NewUpdate(const TOpByKey &op_by_key, const TKey &metadata, const TKey &id);

      /* TODO */
      static std::shared_ptr<TUpdate> ReconstructUpdate(TSequenceNumber seq_num);

      /* TODO */
      static TUpdate *CopyUpdate(TUpdate *that, void *state_alloc);

      /* TODO */
      static TUpdate *ShallowCopy(TUpdate *that, void *state_alloc);

      /* TODO */
      static inline void *operator new(size_t size) {
        return Pool.Alloc(size);
      }

      /* TODO */
      static inline void operator delete(void *ptr, size_t) {
        Pool.Free(ptr);
      }

      /* TODO */
      static void InitUpdatePool(size_t num_obj) {
        Pool.Init(num_obj);
      }

      /* TODO */
      static void InitEntryPool(size_t num_obj) {
        TEntry::Pool.Init(num_obj);
      }

      /* TODO */
      static inline double GetUpdatePoolUsedPct() {
        return static_cast<double>(Pool.GetNumBlocksUsed()) / Pool.GetMaxBlocks();
      }

      /* TODO */
      static inline double GetUpdateEntryPoolUsedPct() {
        return static_cast<double>(TEntry::Pool.GetNumBlocksUsed()) / TEntry::Pool.GetMaxBlocks();
      }

      protected:

      /* TODO */
      TUpdate(const TOpByKey &op_by_key, const TKey &metadata, const TKey &id, void *state_alloc);

      /* TODO */
      explicit TUpdate(const TUpdate *that, void *state_alloc);

      /* Shallow Copy! */
      explicit TUpdate(void *state_alloc, const TUpdate *that);

      /* TODO */
      explicit TUpdate(TSequenceNumber);

      private:

      /* TODO */
      typedef InvCon::OrderedList::TMembership<TUpdate, TMemoryLayer, TSequenceNumber> TMemoryLayerMembership;

      /* TODO */
      mutable Atom::TSuprena Suprena;

      /* TODO */
      mutable TEntryCollection::TImpl EntryCollection;

      /* TODO */
      TMemoryLayerMembership::TImpl MemoryLayerMembership;

      /* TODO */
      Atom::TCore Metadata;

      /* TODO */
      Atom::TCore Id;

      /* TODO */
      std::shared_ptr<TPersistenceNotification> PersistenceNotification;

      /* TODO */
      static Util::TPool Pool;

      /* TODO */
      friend class TMemoryLayer;
      friend class TManager;
      friend class L1::TTransaction;
      friend class Orly::Server::TServer;
      friend class Orly::Server::TIndyReporter;

    };  // TUpdate

    /* TODO */
    inline void TUpdate::TPersistenceNotification::Call(TUpdate::TPersistenceNotification::TResult result) {
      Cb(result);
    }

    /* TODO */
    inline TSequenceNumber TUpdate::GetSequenceNumber() const {
      return MemoryLayerMembership.GetKey();
    }

    /* TODO */
    inline const Atom::TCore &TUpdate::GetId() const {
      return Id;
    }

    /* TODO */
    inline const Atom::TCore &TUpdate::GetMetadata() const {
      return Metadata;
    }

    /* TODO */
    inline Atom::TSuprena &TUpdate::GetSuprena() const {
      return Suprena;
    }

    /* TODO */
    inline void TUpdate::SetSequenceNumber(TSequenceNumber seq_num) {
      MemoryLayerMembership.SetKey(seq_num);
    }

    /* TODO */
    inline void TUpdate::SetMetadata(const TKey &metadata) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      Metadata = Atom::TCore(&Suprena, Sabot::State::TAny::TWrapper(metadata.GetCore().NewState(metadata.GetArena(), state_alloc)));
    }

    /* TODO */
    inline void TUpdate::SetId(const TKey &id) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      Id = Atom::TCore(&Suprena, Sabot::State::TAny::TWrapper(id.GetCore().NewState(id.GetArena(), state_alloc)));
    }

    /* TODO */
    inline void TUpdate::SetPersistenceNotification(const std::function<void (TPersistenceNotification::TResult)> &cb) {
      PersistenceNotification = std::make_shared<TPersistenceNotification>(cb);
    }

    /* TODO */
    inline TUpdate::TEntry *TUpdate::AddEntry(const TIndexKey &index_key, const TKey &op) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      return new TEntry(this, index_key, op, state_alloc);
    }

    inline TUpdate::TEntry *TUpdate::AddEntry(const TIndexKey &index_key, const TKey &op, TMutator mutator) {
      void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
      return new TEntry(this, index_key, op, state_alloc, mutator);
    }

    /* TODO */
    inline const TKey &TUpdate::TEntry::GetKey() const {
      return IndexKey.GetKey();
    }

    /* TODO */
    inline const TIndexKey &TUpdate::TEntry::GetIndexKey() const {
      return IndexKey;
    }

    /* TODO */
    inline const Atom::TCore &TUpdate::TEntry::GetOp() const {
      return Op;
    }

    inline TMutator TUpdate::TEntry::GetMutator() const {
      return Mutator;
    }

    /* TODO */
    inline TSequenceNumber TUpdate::TEntry::GetSequenceNumber() const {
      assert(UpdateMembership.TryGetCollector());
      return UpdateMembership.TryGetCollector()->MemoryLayerMembership.GetKey();
    }

    /* TODO */
    inline const Atom::TCore &TUpdate::TEntry::GetMetadata() const {
      assert(UpdateMembership.TryGetCollector());
      return UpdateMembership.TryGetCollector()->Metadata;
    }

    /* TODO */
    inline const Atom::TCore &TUpdate::TEntry::GetId() const {
      assert(UpdateMembership.TryGetCollector());
      return UpdateMembership.TryGetCollector()->Id;
    }

    /* TODO */
    inline Atom::TSuprena &TUpdate::TEntry::GetSuprena() const {
      assert(UpdateMembership.TryGetCollector());
      return UpdateMembership.TryGetCollector()->Suprena;
    }

    /* TODO */
    inline const TUpdate *TUpdate::TEntry::GetUpdate() const {
      assert(UpdateMembership.TryGetCollector());
      return UpdateMembership.TryGetCollector();
    }

    inline const TUpdate::TEntry::TEntryKey &TUpdate::TEntry::GetEntryKey() const {
      return MemoryLayerMembership.GetKey();
    }

    /* TODO */
    inline TUpdate::TEntryCollection *TUpdate::GetEntryCollection() const {
      return &EntryCollection;
    }

    /* TODO */
    inline const std::shared_ptr<TUpdate::TPersistenceNotification> &TUpdate::GetPersistenceNotification() const {
      return PersistenceNotification;
    }

  }  // Indy

}  // Orly