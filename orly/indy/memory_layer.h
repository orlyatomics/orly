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
#include <vector>

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

    /* An in-memory data layer: an update list in sequence order, an entry list in (index id, key,
       newest first) order, and skip-list express lanes over the entry list (#257).

       One writer, many readers (#770). A layer is written by one thread at a time: Tetris or a
       session appending to the repo's current layer under the repo's DataLock, or a merge or an
       import filling a layer nobody else can see yet. Readers (the present walkers, the update
       walker, SeekRun, anything holding a cursor) take no lock and walk the layer while the writer
       inserts. Both lists use InvCon::OrderedList::TPublishedLinks and the express lanes are
       atomics, so every link a reader follows is an acquire load paired with the writer's release
       store, and an entry (with its update, keys and arena) is complete before any link to it is
       published. A reader may miss an entry inserted behind it during its walk, never sees a
       partial one. Entries and updates are removed only when the layer is deleted, after every
       reader is gone (the repo's views pin the layers they read). Size is not published: it is the
       writer's count, for the writer and for a layer that is no longer written. */
    class TMemoryLayer
        : public L0::TManager::TRepo::TDataLayer {
      NO_COPY(TMemoryLayer);
      public:

      using TUpdateCollection = InvCon::OrderedList::TCollection<TMemoryLayer, TUpdate, TSequenceNumber, InvCon::OrderedList::TPublishedLinks>;
      using TEntryCollection = InvCon::OrderedList::TCollection<TMemoryLayer, TUpdate::TEntry, TUpdate::TEntry::TEntryKey, InvCon::OrderedList::TPublishedLinks>;

      explicit TMemoryLayer(L0::TManager *manager);

      ~TMemoryLayer() override;

      void Insert(TUpdate *update) NO_THROW;

      void ReverseInsert(TUpdate *update) NO_THROW;

      [[nodiscard]] inline TEntryCollection *GetEntryCollection() const noexcept;

      /* Link an entry into EntryCollection (level 0) and the skip-list
         accelerator (#257) in O(log n) whatever order keys arrive in (#754).
         The position is the one OrderedList's ReverseInsert would pick, found
         with the same comparison, so the order of level 0 is unchanged: an
         entry that sorts at or after the tail is appended in O(1) (the lane
         predecessors are the lane tails), anything else descends the express
         lanes and finishes on level 0 a step or two from its place. Level 0 is
         linked first and the lanes after it, bottom-up with release stores, so
         a concurrent reader that sees an upper-lane pointer can always resolve
         the node on level 0. No allocation -- the per-level forward pointers
         live inside TEntry -- so this is safe on the NO_THROW commit path.
         Single-writer per layer (DataLock / the merge thread). */
      void LinkEntry(TUpdate::TEntry *entry) NO_THROW;

      /* Return an EntryCollection cursor positioned at the first entry whose
         index-key is >= key (the highest-SeqNum entry of key's run, or the
         first entry of the next key, or an exhausted cursor). Uses the
         skip-list accelerator to jump most of the way, then finishes on level 0
         (EntryCollection). MUST be called only with a fully-bound concrete key
         -- the ordering comparison throws on a free-vs-concrete state (see
         TMatchPresentWalker's exact-point guard). */
      [[nodiscard]] TEntryCollection::TCursor SeekRun(const TIndexKey &key) const;

      /* Return an EntryCollection cursor positioned at the first entry that
         could unify with pattern, a key whose members may be or hold frees:
         past every entry of a lower index id and every entry of pattern's
         index whose members before the pattern's first not fully bound one
         order before the pattern's. Like SeekRun it jumps through the
         skip-list accelerator and finishes on level 0; unlike it, it never
         compares a free with a concrete member, so it takes any key. The
         entry it lands on need not unify -- the caller still matches. */
      [[nodiscard]] TEntryCollection::TCursor SeekPattern(const TIndexKey &pattern) const;

      [[nodiscard]] inline TUpdateCollection *GetUpdateCollection() const noexcept;

      [[nodiscard]] inline bool IsEmpty() const noexcept;

      std::unique_ptr<Indy::TPresentWalker> NewPresentWalker(const TIndexKey &from,
                                                             const TIndexKey &to) const override;

      std::unique_ptr<Indy::TPresentWalker> NewPresentWalker(const TIndexKey &key, bool exact_point = false) const override;

      std::unique_ptr<Indy::TUpdateWalker> NewUpdateWalker(TSequenceNumber from) const override;

      [[nodiscard]] inline TKind GetKind() const noexcept override;

      [[nodiscard]] inline size_t GetSize() const noexcept override;

      [[nodiscard]] inline TSequenceNumber GetLowestSeq() const noexcept override;

      [[nodiscard]] inline TSequenceNumber GetHighestSeq() const noexcept override;

      private:

      class TMatchPresentWalker
          : public Indy::TPresentWalker {
        NO_COPY(TMatchPresentWalker);
        public:

        TMatchPresentWalker(const TMemoryLayer *layer,
                            const TIndexKey &key,
                            bool exact_point);

        ~TMatchPresentWalker() override = default;

        /* True iff. we have an item. */
        [[nodiscard]] explicit operator bool() const override;

        /* The current item. */
        [[nodiscard]] const TItem &operator*() const override;

        /* Walk to the next item, if any. */
        TMatchPresentWalker &operator++() override;

        private:

        void Refresh() const;

        const TMemoryLayer *const Layer;

        const TIndexKey &Key;

        mutable TMemoryLayer::TEntryCollection::TCursor Csr;

        mutable bool Valid = true;

        mutable bool Cached = false;

        mutable bool PassedMatch = false;

        /* When true, the search key is a fully-bound point read (from
           operator[]/Exists) and the ctor may seek via the secondary index
           instead of scanning the ordered list from the head (#257). */
        const bool ExactPoint;

        mutable TItem Item;

      };  // TMatchPresentWalker

      class TRangePresentWalker
          : public Indy::TPresentWalker {
        NO_COPY(TRangePresentWalker);
        public:

        TRangePresentWalker(const TMemoryLayer *layer,
                            const TIndexKey &from,
                            const TIndexKey &to);

        ~TRangePresentWalker() override = default;

        /* True iff. we have an item. */
        [[nodiscard]] explicit operator bool() const override;

        /* The current item. */
        [[nodiscard]] const TItem &operator*() const override;

        /* Walk to the next item, if any. */
        TRangePresentWalker &operator++() override;

        private:

        void Refresh() const;

        const TMemoryLayer *const Layer;

        const TIndexKey &From;

        const TIndexKey &To;

        mutable TMemoryLayer::TEntryCollection::TCursor Csr;

        mutable bool Valid = true;

        mutable bool Cached = false;

        mutable TItem Item;

      };  // TRangePresentWalker

      class TUpdateWalker
          : public Indy::TUpdateWalker {
        NO_COPY(TUpdateWalker);
        public:

        TUpdateWalker(const TMemoryLayer *layer, TSequenceNumber from);

        ~TUpdateWalker() override = default;

        /* True iff. we have an item. */
        [[nodiscard]] explicit operator bool() const override;

        /* The current item. */
        [[nodiscard]] const Indy::TUpdateWalker::TItem &operator*() const override;

        /* Walk to the next item, if any. */
        TUpdateWalker &operator++() override;

        private:

        void Refresh();

        const TMemoryLayer *const Layer;

        TSequenceNumber From;

        TMemoryLayer::TUpdateCollection::TCursor Csr;

        bool Valid = false;

        mutable TItem Item;

      };  // TUpdateWalker

      /* True iff. entry a sorts strictly after entry b on level 0
         (EntryCollection): (IndexId asc, key asc, SeqNum desc). This is the
         very comparison OrderedList's ReverseInsert uses to place an entry, so
         LinkEntry, which links an entry after the last one that does not sort
         after it, puts it exactly where ReverseInsert would, ties included
         (#754). Stored keys are concrete, so it never throws. */
      /* The descent SeekRun and SeekPattern share: a cursor at the first
         entry before(entry) is false of. before must be true of a head of
         level 0 and false of the rest. */
      template <typename TBefore>
      [[nodiscard]] TEntryCollection::TCursor SeekPast(const TBefore &before) const;

      [[nodiscard]] static bool EntryAfter(const TUpdate::TEntry *a, const TUpdate::TEntry *b) noexcept;

    public:

      void ImporterAppendUpdate(TUpdate *update);

      void ImporterAppendEntry(TUpdate::TEntry *entry);

      /* #892: a source entry and the update it is being copied into. */
      struct TCopiedEntry {
        const TUpdate::TEntry *Source;
        TUpdate *Target;
      };

      /* Copy each source entry into its target update, then link the copies into this layer.
         The targets must already be in this layer's UpdateCollection (or about to be).
         copies must be in this layer's entry order, (index id, key, seq), which is also the order the
         copies are linked in. See the definition for the order they are added to the updates in. */
      void ImporterAppendCopies(const std::vector<TCopiedEntry> &copies, void *state_alloc);

    private:

      mutable TUpdateCollection::TImpl UpdateCollection;

      mutable TEntryCollection::TImpl EntryCollection;

      /* Per-level head pointers for the skip-list accelerator over
         EntryCollection (#257). SkipHead[l] is the first entry present on
         express lane l (l >= 1; level 0 is EntryCollection itself). Written
         only by LinkEntry (single-writer per layer); read with acquire by
         SeekRun. SkipListLevel is the current top occupied lane. */
      std::atomic<TUpdate::TEntry *> SkipHead[TUpdate::TEntry::SkipMaxLevel];
      std::atomic<size_t> SkipListLevel = 0UL;

      /* SkipTail[l] is the last entry on express lane l, or null while the
         lane is empty: the lane predecessors of an entry appended at the end
         of level 0, which makes an in-order insert O(1) (#754). The writer's
         own bookkeeping; readers never look at it. */
      TUpdate::TEntry *SkipTail[TUpdate::TEntry::SkipMaxLevel] = {};

      size_t Size = 0UL;

      friend class Orly::Server::TServer;
      friend class Orly::Indy::TRepo;
      friend class Orly::Indy::TManager;

    };  // TMemoryLayer

    inline TMemoryLayer::TEntryCollection *TMemoryLayer::GetEntryCollection() const noexcept {
      return &EntryCollection;
    }

    inline TMemoryLayer::TUpdateCollection *TMemoryLayer::GetUpdateCollection() const noexcept {
      return &UpdateCollection;
    }

    inline bool TMemoryLayer::IsEmpty() const noexcept {
      return UpdateCollection.TryGetFirstMember() == nullptr;
    }

    inline L0::TManager::TRepo::TDataLayer::TKind TMemoryLayer::GetKind() const noexcept {
      return L0::TManager::TRepo::TDataLayer::Mem;
    }

    inline size_t TMemoryLayer::GetSize() const noexcept {
      return Size;
    }

    inline TSequenceNumber TMemoryLayer::GetLowestSeq() const noexcept {
      assert(Size);
      return UpdateCollection.TryGetFirstMember()->GetSequenceNumber();
    }

    inline TSequenceNumber TMemoryLayer::GetHighestSeq() const noexcept {
      assert(Size);
      return UpdateCollection.TryGetLastMember()->GetSequenceNumber();
    }

  }  // Indy

}  // Orly

