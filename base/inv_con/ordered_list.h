/* <inv_con/ordered_list.h>

   Classes for maintaining an invasive, ordered, double-linked list.

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
#include <base/no_default_case.h>
#include <base/inv_con/cursor.h>

namespace InvCon {

  /* Classes for maintaining an invasive, ordered, double-linked list. */
  namespace OrderedList {

    /* How a list keeps its links (First, Last, Next and Prev). A list is one of two kinds, chosen
       by the last template parameter of TCollection and TMembership, which must agree:

       TPlainLinks (the default): plain pointers. The list belongs to whoever holds it; any
       concurrent use needs a lock around every access, reads included.

       TPublishedLinks: one writer, any number of concurrent readers, no lock on the read side.
         - Only one thread mutates the list at a time (insert, remove, set key, delete members);
           the owner serializes writers itself.
         - Readers may walk forward or backward (TryGetFirst/Last, cursors, TryGetNext/Prev) while
           the writer inserts. Every link a reader follows is an acquire load, and an insert fills
           in the new member's own links first and then publishes it with release stores: the
           predecessor's Next (or the list's First), then the successor's Prev (or the list's
           Last). So a reader that reaches a member also sees everything the writer did to it,
           and to whatever it owns, before inserting it: its key, its links, the member's fields.
         - Removing a member, moving it, or changing its key while a reader can reach it is NOT
           supported: a reader standing on it would lose its place, and nothing would keep the
           member alive. The owner must exclude readers first (or retire the whole list).
         - A reader can miss a member inserted after it passed the place where it goes, as with
           any snapshot-free walk; it never sees a half-linked one.
       Writer-side traversal (Insert finding its place, Remove) uses relaxed loads: the writer only
       ever reads links it wrote itself. With TPlainLinks every one of these is an ordinary pointer
       access, so lists that don't need publication pay nothing for it. */
    struct TPlainLinks {
      template <typename TPtr>
      class TLink {
        public:
        TLink() : Ptr(nullptr) {}
        TPtr Load() const { return Ptr; }
        TPtr LoadOwn() const { return Ptr; }
        void Store(TPtr ptr) { Ptr = ptr; }
        void Publish(TPtr ptr) { Ptr = ptr; }
        private:
        TPtr Ptr;
      };
    };
    struct TPublishedLinks {
      template <typename TPtr>
      class TLink {
        public:
        TLink() : Ptr(nullptr) {}
        /* A reader's load: everything written before the matching Publish is visible. */
        TPtr Load() const { return Ptr.load(std::memory_order_acquire); }
        /* The writer reading a link it wrote. */
        TPtr LoadOwn() const { return Ptr.load(std::memory_order_relaxed); }
        /* A link no reader can reach yet (the new member's own links, or an unlinked member's). */
        void Store(TPtr ptr) { Ptr.store(ptr, std::memory_order_relaxed); }
        /* A link a reader can follow. */
        void Publish(TPtr ptr) { Ptr.store(ptr, std::memory_order_release); }
        private:
        std::atomic<TPtr> Ptr;
      };
    };

    /* Forward declarations of base classes. */
    template <typename TCollector, typename TMember, typename TKey, typename TLinks = TPlainLinks>
    class TCollection;
    template <typename TMember, typename TCollector, typename TKey, typename TLinks = TPlainLinks>
    class TMembership;

    /* Forward declarations of final classes. */
    namespace Impl {
      template <typename TCollector, typename TMember, typename TKey, typename TLinks = TPlainLinks>
      class TCollection;
      template <typename TMember, typename TCollector, typename TKey, typename TLinks = TPlainLinks>
      class TMembership;
    }

    /* The 'one' side of the one-to-many. */
    template <typename TCollector, typename TMember, typename TKey, typename TLinks>
    class TCollection {
      NO_COPY(TCollection);
      public:

      /* Our corresponding 'many' class. */
      typedef TMembership<TMember, TCollector, TKey, TLinks> TTypedMembership;

      /* The final version of this class. */
      typedef Impl::TCollection<TCollector, TMember, TKey, TLinks> TImpl;

      /* Our cursor class. */
      typedef InvCon::Impl::TCursor<TCollector, TCollection, TMember, TTypedMembership> TCursor;

      /* The collector which owns us.  Never null. */
      TCollector *GetCollector() const {
        return Collector;
      }

      /* The first member of the collection,
         or null if the collection is empty. */
      TMember *TryGetFirstMember() const {
        TTypedMembership *membership = FirstMembership.Load();
        return membership ? membership->Member : 0;
      }

      /* The first member of the collection which matches the given key,
         or null if the collection contains no match. */
      TMember *TryGetFirstMember(const TKey &key) const {
        TTypedMembership *membership = TryGetFirstMembership(key);
        return membership ? membership->GetMember() : 0;
      }

      /* The first membership of the collection,
         or null if the collection is empty. */
      TTypedMembership *TryGetFirstMembership() const {
        return FirstMembership.Load();
      }

      /* The first membership of the collection which matches the given key,
         or null if the collection contains no match. */
      TTypedMembership *TryGetFirstMembership(const TKey &key) const {
        TTypedMembership *membership = FirstMembership.Load();
        while (membership && membership->Key < key) {
          membership = membership->NextMembership.Load();
        }
        return (membership && !(key < membership->Key)) ? membership : 0;
      }

      /* The last member of the collection,
         or null if the collection is empty. */
      TMember *TryGetLastMember() const {
        TTypedMembership *membership = LastMembership.Load();
        return membership ? membership->Member : 0;
      }

      /* The last member of the collection which matches the given key,
         or null if the collection contains no match. */
      TMember *TryGetLastMember(const TKey &key) const {
        TTypedMembership *membership = TryGetLastMembership(key);
        return membership ? membership->GetMember() : 0;
      }

      /* The last membership of the collection,
         or null if the collection is empty. */
      TTypedMembership *TryGetLastMembership() const {
        return LastMembership.Load();
      }

      /* The last membership of the collection which matches the given key,
         or null if the collection contains no match. */
      TTypedMembership *TryGetLastMembership(const TKey &key) const {
        TTypedMembership *membership = LastMembership.Load();
        while (membership && key < membership->Key) {
          membership = membership->PrevMembership.Load();
        }
        return (membership && !(membership->Key < key)) ? membership : 0;
      }

      bool IsEmpty() const {
        return FirstMembership.Load() == 0;
      }

      protected:

      /* Pass in a non-null pointer to the collector which owns us. */
      TCollection(TCollector *collector)
          : Collector(collector) {
        assert(collector);
      }

      /* Remove each member from the collection upon destruction. */
      virtual ~TCollection() {
        RemoveEachMember();
      }

      /* Delete each member. */
      void DeleteEachMember() {
        while (TTypedMembership *membership = FirstMembership.LoadOwn()) {
          TMember *member = membership->Member;
          membership->Remove();
          delete member;
        }
      }

      /* Insert the given membership at the correct position.
         If the membership is already at the correct position, do nothing.
         If the membership in a different collection, remove it from that collection before inserting. */
      void Insert(TTypedMembership *membership) {
        assert(membership);
        membership->Insert(this);
      }

      void ReverseInsert(TTypedMembership *membership) {
        assert(membership);
        membership->ReverseInsert(this);
      }

      /* Remove each member from the collection but don't delete them. */
      void RemoveEachMember() {
        while (TTypedMembership *membership = FirstMembership.LoadOwn()) {
          membership->Remove();
        }
      }

      private:

      /* See accessor. */
      TCollector *const Collector;

      /* See accessors. */
      typename TLinks::template TLink<TTypedMembership *> FirstMembership, LastMembership;

      /* For Collector, FirstMembership, and LastMembership. */
      friend class TMembership<TMember, TCollector, TKey, TLinks>;

    };  // TCollection<TCollector, TMember>

    /* The 'many' side of the one-to-many. */
    template <typename TMember, typename TCollector, typename TKey, typename TLinks>
    class TMembership {
      NO_COPY(TMembership);
      public:

      /* Our corresponding 'one' class. */
      typedef TCollection<TCollector, TMember, TKey, TLinks> TTypedCollection;

      /* The final version of this class. */
      typedef Impl::TMembership<TMember, TCollector, TKey, TLinks> TImpl;

      /* Get our key. */
      const TKey &GetKey() const {
        return Key;
      }

      /* The member which owns us.  Never null. */
      TMember *GetMember() const {
        return Member;
      }

      /* The collection we're in, if any. */
      TTypedCollection *TryGetCollection() const {
        return Collection;
      }

      /* The collector whose collection we're in, if any. */
      TCollector *TryGetCollector() const {
        return Collection ? Collection->Collector : 0;
      }

      /* The member before us in our collection.
         A null here means either we're first in our collection or we're not in a collection. */
      TMember *TryGetPrevMember() const {
        TMembership *membership = PrevMembership.Load();
        return membership ? membership->Member : 0;
      }

      /* The membership before us in our collection.
         A null here means either we're first in our collection or we're not in a collection. */
      TMembership *TryGetPrevMembership() const {
        return PrevMembership.Load();
      }

      /* The member before us in our collection.
         A null here means either we're first in our collection or we're not in a collection. */
      TMember *TryGetNextMember() const {
        TMembership *membership = NextMembership.Load();
        return membership ? membership->Member : 0;
      }

      /* The membership before us in our collection.
         A null here means either we're first in our collection or we're not in a collection. */
      TMembership *TryGetNextMembership() const {
        return NextMembership.Load();
      }

      protected:

      /* Pass in a non-null pointer to the member which owns us,
         and an an optional pointer to a collection to insert into. */
      TMembership(TMember *member)
          : Member(member) {
        assert(member);
        ZeroLinkage();
      }

      /* Pass in a non-null pointer to the member which owns us, a value for our key,
         and an an optional pointer to a collection to insert into. */
      TMembership(TMember *member, const TKey &key, TTypedCollection *collection = 0)
          : Member(member), Key(key) {
        assert(member);
        ZeroLinkage();
        if (collection) {
          Insert(collection);
        }
      }

      /* Pass in a non-null pointer to the member which owns us, a value for our key,
         and an an optional pointer to a collection to insert into. */
      TMembership(TMember *member, const TKey &key, TTypedCollection *collection, TOrient insert_from)
          : Member(member), Key(key) {
        assert(member);
        assert(collection);
        ZeroLinkage();
        switch (insert_from) {
          case TOrient::Fwd: {
            Insert(collection);
            break;
          }
          case TOrient::Rev: {
            ReverseInsert(collection);
            break;
          }
        }
      }

      /* Automatically removes us from our collection (if any) before destruction. */
      virtual ~TMembership() {
        Remove();
      }

      /* Insert us into the given collection at the correct position.
         If we're already at that position, do nothing.
         If we're already in a different collection, remove us from that collection before inserting. */
      void Insert(TTypedCollection *collection) {
        assert(collection);
        /* Skip over ourselves, if we're already in this collection. */
        auto next_of = [this](TMembership *membership) {
          TMembership *next = membership->NextMembership.LoadOwn();
          return next != this ? next : next->NextMembership.LoadOwn();
        };
        TMembership *first = collection->FirstMembership.LoadOwn();
        TMembership
            *prev_membership = 0,
            *next_membership = first != this ? first : first->NextMembership.LoadOwn();
        while (next_membership && next_membership->Key <= Key) {
          prev_membership = next_membership;
          next_membership = next_of(next_membership);
        }
        if (Collection != collection || PrevMembership.LoadOwn() != prev_membership || NextMembership.LoadOwn() != next_membership) {
          Remove();
          Link(collection, prev_membership, next_membership);
        }
      }

      void ReverseInsert(TTypedCollection *collection) {
        assert(collection);
        /* Skip over ourselves, if we're already in this collection. */
        auto prev_of = [this](TMembership *membership) {
          TMembership *prev = membership->PrevMembership.LoadOwn();
          return prev != this ? prev : prev->PrevMembership.LoadOwn();
        };
        TMembership *last = collection->LastMembership.LoadOwn();
        TMembership
            *next_membership = 0,
            *prev_membership = last != this ? last : last->PrevMembership.LoadOwn();
        while (prev_membership && prev_membership->Key > Key) {
          next_membership = prev_membership;
          prev_membership = prev_of(prev_membership);
        }
        if (Collection != collection || NextMembership.LoadOwn() != next_membership || PrevMembership.LoadOwn() != prev_membership) {
          Remove();
          Link(collection, prev_membership, next_membership);
        }
      }

      /* Remove us from our collection.
         If we're not in a collection, this function does nothing. */
      void Remove() {
        if (Collection) {
          /* Fixup the pointers on either side of us to point around us,
             then go back to the unlinked state. */
          FixupLinkage(NextMembership.LoadOwn(), PrevMembership.LoadOwn());
          ZeroLinkage();
        }
      }

      /* Set our key, relocating us within our collection, if necessary.
         If the new key is the same as the old, do nothing. */
      void SetKey(const TKey &key) {
        if (Key != key) {
          Key = key;
          if (Collection) {
            Insert(Collection);
          }
        }
      }

      private:

      /* Link us, unlinked, into the collection between prev_membership and next_membership (either
         may be null, for the ends). Our own links are filled in before anything points at us, then
         the forward link to us is published, then the backward one (see TPublishedLinks). */
      void Link(TTypedCollection *collection, TMembership *prev_membership, TMembership *next_membership) {
        Collection = collection;
        PrevMembership.Store(prev_membership);
        NextMembership.Store(next_membership);
        FixupLinkage(this, this);
      }

      /* The fixup pointers before and after us in our collection.
         We must be in a collection or this is an error. */
      void FixupLinkage(TMembership *prev_fixup, TMembership *next_fixup) {
        assert(Collection);
        TMembership *prev = PrevMembership.LoadOwn(), *next = NextMembership.LoadOwn();
        (prev ? prev->NextMembership : Collection->FirstMembership).Publish(prev_fixup);
        (next ? next->PrevMembership : Collection->LastMembership ).Publish(next_fixup);
      }

      /* Reset all pointers to null.
         This function does no unlinking so make sure we're unlinked first. */
      void ZeroLinkage() {
        Collection = 0;
        NextMembership.Store(0);
        PrevMembership.Store(0);
      }

      /* See accessor. */
      TMember *const Member;

      /* See accessor. */
      TKey Key;

      /* See accessor. */
      TTypedCollection *Collection;

      /* See accessors. */
      typename TLinks::template TLink<TMembership *> PrevMembership, NextMembership;

      /* For ~TMembership(), Insert(), Remove(), and Member. */
      friend class TCollection<TCollector, TMember, TKey, TLinks>;

    };  // TMembership<TMember, TCollector>

    /* The final versions of the 'one' and 'many' classes. */
    namespace Impl {

      /* The final 'one'. */
      template <typename TCollector, typename TMember, typename TKey, typename TLinks>
      class TCollection
          : public OrderedList::TCollection<TCollector, TMember, TKey, TLinks> {
        public:

        /* Our base type. */
        typedef OrderedList::TCollection<TCollector, TMember, TKey, TLinks> TBase;

        /* Do-little. */
        TCollection(TCollector *collector)
            : TBase(collector) {}

        /* Do-little. */
        virtual ~TCollection() {}

        /* Make our base's protected mutators public. */
        using TBase::DeleteEachMember;
        using TBase::IsEmpty;
        using TBase::Insert;
        using TBase::ReverseInsert;
        using TBase::RemoveEachMember;

      };  // TCollection

      /* The final 'many'. */
      template <typename TMember, typename TCollector, typename TKey, typename TLinks>
      class TMembership
          : public OrderedList::TMembership<TMember, TCollector, TKey, TLinks> {
        public:

        /* Our base type. */
        typedef OrderedList::TMembership<TMember, TCollector, TKey, TLinks> TBase;

        /* Do-little. */
        TMembership(TMember *member)
            : TBase(member) {}

        /* Do-little. */
        TMembership(TMember *member, const TKey &key, typename TBase::TTypedCollection *collection = 0)
            : TBase(member, key, collection) {}

        /* Do-little. */
        TMembership(TMember *member, const TKey &key, TOrient insert_from, typename TBase::TTypedCollection *collection)
            : TBase(member, key, collection, insert_from) {}

        /* Do-little. */
        virtual ~TMembership() {}

        /* Make our base's protected mutators public. */
        using TBase::Insert;
        using TBase::ReverseInsert;
        using TBase::Remove;
        using TBase::SetKey;

      };  // TMembership

    }  // Impl

  }  // OrderedList

}  // InvCon
