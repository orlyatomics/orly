/* <orly/indy/memory_layer.h>

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
#include <orly/indy/manager_base.h>
#include <orly/indy/update.h>
#include <orly/sabot/all.h>

namespace Orly {

  namespace Server {

    /* Forward Declarations. */
    class TServer;

  }

  namespace Indy {

    /* Forward Declarations. */
    class TRepo;
    class TManager;

    /* TODO */
    class TMemoryLayer
        : public L0::TManager::TRepo::TDataLayer {
      NO_COPY(TMemoryLayer);
      public:

      /* TODO */
      typedef InvCon::OrderedList::TCollection<TMemoryLayer, TUpdate, TSequenceNumber> TUpdateCollection;
      typedef InvCon::OrderedList::TCollection<TMemoryLayer, TUpdate::TEntry, TUpdate::TEntry::TEntryKey> TEntryCollection;

      /* TODO */
      TMemoryLayer(L0::TManager *manager);

      /* TODO */
      virtual ~TMemoryLayer();

      /* TODO */
      void Insert(TUpdate *update) NO_THROW;

      /* TODO */
      void ReverseInsert(TUpdate *update) NO_THROW;

      /* TODO */
      inline TEntryCollection *GetEntryCollection() const;

      /* Link a freshly-inserted entry into the skip-list accelerator (#257).
         Called from the three EntryCollection insert sites, after the level-0
         (EntryCollection) link, so a concurrent reader that sees an upper-lane
         pointer can always resolve the node on level 0. No allocation -- the
         per-level forward pointers live inside TEntry -- so this is safe on the
         NO_THROW commit path. Single-writer per layer (DataLock / the merge
         thread), so writer-writer races are excluded; reader safety rests on
         level 0 being the authoritative EntryCollection (upper lanes are a
         best-effort accelerator). */
      void SkipInsert(TUpdate::TEntry *entry) NO_THROW;

      /* Return an EntryCollection cursor positioned at the first entry whose
         index-key is >= key (the highest-SeqNum entry of key's run, or the
         first entry of the next key, or an exhausted cursor). Uses the
         skip-list accelerator to jump most of the way, then finishes on level 0
         (EntryCollection). MUST be called only with a fully-bound concrete key
         -- the ordering comparison throws on a free-vs-concrete state (see
         TMatchPresentWalker's exact-point guard). */
      TEntryCollection::TCursor SeekRun(const TIndexKey &key) const;

      /* TODO */
      inline TUpdateCollection *GetUpdateCollection() const;

      inline bool IsEmpty() const;

      /* TODO */
      virtual std::unique_ptr<Indy::TPresentWalker> NewPresentWalker(const TIndexKey &from,
                                                                     const TIndexKey &to) const override;

      virtual std::unique_ptr<Indy::TPresentWalker> NewPresentWalker(const TIndexKey &key, bool exact_point = false) const override;

      /* TODO */
      virtual std::unique_ptr<Indy::TUpdateWalker> NewUpdateWalker(TSequenceNumber from) const override;

      /* TODO */
      inline virtual size_t GetSize() const override;

      /* TODO */
      inline virtual TSequenceNumber GetLowestSeq() const override;

      /* TODO */
      inline virtual TSequenceNumber GetHighestSeq() const override;

      private:

      /* TODO */
      class TMatchPresentWalker
          : public Indy::TPresentWalker {
        NO_COPY(TMatchPresentWalker);
        public:

        /* TODO */
        TMatchPresentWalker(const TMemoryLayer *layer,
                       const TIndexKey &key,
                       bool exact_point);

        /* TODO */
        virtual ~TMatchPresentWalker();

        /* True iff. we have an item. */
        virtual operator bool() const;

        /* The current item. */
        virtual const TItem &operator*() const;

        /* Walk to the next item, if any. */
        virtual TMatchPresentWalker &operator++();

        private:

        /* TODO */
        void Refresh() const;

        /* TODO */
        const TMemoryLayer *const Layer;

        /* TODO */
        const TIndexKey &Key;

        /* TODO */
        mutable TMemoryLayer::TEntryCollection::TCursor Csr;

        /* TODO */
        mutable bool Valid;

        /* TODO */
        mutable bool Cached;

        /* TODO */
        mutable bool PassedMatch;

        /* When true, the search key is a fully-bound point read (from
           operator[]/Exists) and the ctor may seek via the secondary index
           instead of scanning the ordered list from the head (#257). */
        const bool ExactPoint;

        /* TODO */
        mutable TItem Item;

      };  // TMatchPresentWalker

      /* TODO */
      class TRangePresentWalker
          : public Indy::TPresentWalker {
        NO_COPY(TRangePresentWalker);
        public:

        /* TODO */
        TRangePresentWalker(const TMemoryLayer *layer,
                       const TIndexKey &from,
                       const TIndexKey &to);

        /* TODO */
        virtual ~TRangePresentWalker();

        /* True iff. we have an item. */
        virtual operator bool() const;

        /* The current item. */
        virtual const TItem &operator*() const;

        /* Walk to the next item, if any. */
        virtual TRangePresentWalker &operator++();

        private:

        /* TODO */
        void Refresh() const;

        /* TODO */
        const TMemoryLayer *const Layer;

        /* TODO */
        const TIndexKey &From;

        /* TODO */
        const TIndexKey &To;

        /* TODO */
        mutable TMemoryLayer::TEntryCollection::TCursor Csr;

        /* TODO */
        mutable bool Valid;

        /* TODO */
        mutable bool Cached;

        /* TODO */
        mutable bool PassedMatch;

        /* TODO */
        mutable TItem Item;

      };  // TRangePresentWalker

      /* TODO */
      class TUpdateWalker
          : public Indy::TUpdateWalker {
        NO_COPY(TUpdateWalker);
        public:

        /* TODO */
        TUpdateWalker(const TMemoryLayer *layer, TSequenceNumber from);

        /* TODO */
        virtual ~TUpdateWalker();

        /* True iff. we have an item. */
        virtual operator bool() const;

        /* The current item. */
        virtual const Indy::TUpdateWalker::TItem &operator*() const;

        /* Walk to the next item, if any. */
        virtual TUpdateWalker &operator++();

        private:

        /* TODO */
        void Refresh();

        /* TODO */
        const TMemoryLayer *const Layer;

        /* TODO */
        TSequenceNumber From;

        /* TODO */
        TMemoryLayer::TUpdateCollection::TCursor Csr;

        /* TODO */
        bool Valid;

        /* TODO */
        mutable TItem Item;

      };  // TUpdateWalker

      /* TODO */
      inline virtual TKind GetKind() const;

      /* TODO */
      void ImporterAppendUpdate(TUpdate *update);

      /* TODO */
      void ImporterAppendEntry(TUpdate::TEntry *entry);

      /* TODO */
      mutable TUpdateCollection::TImpl UpdateCollection;

      /* TODO */
      mutable TEntryCollection::TImpl EntryCollection;

      /* Per-level head pointers for the skip-list accelerator over
         EntryCollection (#257). SkipHead[l] is the first entry present on
         express lane l (l >= 1; level 0 is EntryCollection itself). Written
         only by SkipInsert (single-writer per layer); read with acquire by
         SeekRun. SkipListLevel is the current top occupied lane. */
      std::atomic<TUpdate::TEntry *> SkipHead[TUpdate::TEntry::SkipMaxLevel];
      std::atomic<size_t> SkipListLevel;

      /* TODO */
      size_t Size;

      friend class Orly::Server::TServer;
      friend class Orly::Indy::TRepo;
      friend class Orly::Indy::TManager;

    };  // TMemoryLayer

    /* TODO */
    inline TMemoryLayer::TEntryCollection *TMemoryLayer::GetEntryCollection() const {
      return &EntryCollection;
    }

    /* TODO */
    inline TMemoryLayer::TUpdateCollection *TMemoryLayer::GetUpdateCollection() const {
      return &UpdateCollection;
    }

    /* TODO */
    inline bool TMemoryLayer::IsEmpty() const {
      return UpdateCollection.TryGetFirstMember() == nullptr;
    }

    /* TODO */
    inline L0::TManager::TRepo::TDataLayer::TKind TMemoryLayer::GetKind() const {
      return L0::TManager::TRepo::TDataLayer::Mem;
    }

    /* TODO */
    inline size_t TMemoryLayer::GetSize() const {
      return Size;
    }

    /* TODO */
    inline TSequenceNumber TMemoryLayer::GetLowestSeq() const {
      assert(Size);
      return UpdateCollection.TryGetFirstMember()->GetSequenceNumber();
    }

    /* TODO */
    inline TSequenceNumber TMemoryLayer::GetHighestSeq() const {
      assert(Size);
      return UpdateCollection.TryGetLastMember()->GetSequenceNumber();
    }

  }  // Indy

}  // Orly

