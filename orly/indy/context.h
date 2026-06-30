/* <orly/indy/context.h>

   `TContext` (subclass of `TContextBase`) -- the per-call execution
   context inside an `orlyi` method invocation. Holds the POV / repo
   tree the method walks, the key-cursor collection (for iterators
   that survive across statements), the sabot arena, and the current
   sequence numbers. Constructed per RPC and torn down at end-of-call.

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

#include <unordered_set>

#include <base/chrono.h>
#include <base/class_traits.h>
#include <base/no_throw.h>
#include <base/uuid.h>
#include <orly/atom/suprena.h>
#include <orly/context_base.h>
#include <orly/indy/fiber/fiber.h>
#include <orly/indy/manager.h>
#include <orly/indy/sequence_number.h>
#include <orly/key_generator.h>
#include <orly/package/rt.h>
#include <orly/sabot/all.h>
#include <orly/var/sabot_to_var.h>

namespace Orly {

  namespace Indy {

    class TContext
        : public TContextBase {
      NO_COPY(TContext);
      public:

      /* Forward Declarations. */
      class TKeyCursor;
      struct TKeyCursorCollector;

      typedef InvCon::UnorderedList::TCollection<TKeyCursorCollector, TKeyCursor> TKeyCursorCollection;

      private:

      typedef std::vector<std::pair<Indy::L0::TManager::TPtr<TRepo>, std::unique_ptr<TRepo::TView>>> TRepoTree;

      class TPresentWalker {
        NO_COPY(TPresentWalker);
        public:

        using TItem = Orly::Indy::TPresentWalker::TItem;

        /* exact_point true => fully-bound point read (operator[]/Exists), so
           layers may seek instead of head-scanning (#257). The TKeyCursor path
           leaves it false because its pattern may be a prefix. */
        TPresentWalker(TContext *context, const TRepoTree &repo_tree, const TIndexKey &key, bool exact_point = false);

        TPresentWalker(TContext *context, const TRepoTree &repo_tree, const TIndexKey &from, const TIndexKey &to);

        ~TPresentWalker();

        inline operator bool() const;

        inline const TItem &operator*() const;

        inline TPresentWalker &operator++();

        private:

        void Refresh();

        /* Phase 3 of #49: fold consecutive same-mutator commutative
           entries on the current key into a single resolved value.
           No-op when Item.Mutator is TMutator::Assign (the only case
           pre-#49-phase-2 in-tree code ever produced). */
        void ApplyDeferredFold();

        std::vector<std::shared_ptr<Indy::TPresentWalker>> WalkerVec;

        Util::TMinHeap<Indy::TPresentWalker::TItem, size_t> MinHeap;

        bool Valid;

        mutable TItem Item;

        /* Arena that owns folded Op TCores. Borrowed from the enclosing
           TContext, so the folded TCore outlives the walker. */
        Atom::TCore::TExtensibleArena *FoldArena;

      };  // TPresentWalker

      public:

      class TKeyCursor
          : public Orly::TKeyCursor {
        NO_COPY(TKeyCursor);
        public:

        typedef InvCon::UnorderedList::TMembership<TKeyCursor, TKeyCursorCollector> TContextMembership;

        TKeyCursor(TContext *context, const Indy::TIndexKey &pattern);

        TKeyCursor(TContext *context, const Indy::TIndexKey &from, const Indy::TIndexKey &to);

        virtual ~TKeyCursor();

        virtual operator bool() const override;

        virtual const Indy::TKey &operator*() const override;

        virtual const Indy::TKey *operator->() const override;

        virtual TKeyCursor &operator++() override;

        const TContext::TPresentWalker::TItem &GetVal() const;

        private:

        void Refresh() const;

        Indy::TIndexKey Key;

        Indy::TIndexKey To;

        mutable bool Valid;

        mutable bool Cached;

        mutable TPresentWalker Csr;

        mutable Indy::TKey Item;

        mutable TContextMembership::TImpl ContextMembership;

      };  // TKeyCursor

      TContext(const Indy::L0::TManager::TPtr<TRepo> &private_repo, Atom::TCore::TExtensibleArena *arena);

      virtual ~TContext();

      virtual Indy::TKey operator[](const Indy::TIndexKey &key) override;

      virtual bool Exists(const Indy::TIndexKey &key) override;

      inline size_t GetWalkerCount() const {
        return WalkerCount;
      }

      const Base::TTimer &GetPresentWalkConsTimer() const {
        return PresentWalkConsTimer;
      }

      struct TKeyCursorCollector {
        NO_COPY(TKeyCursorCollector);

        TKeyCursorCollector() : KeyCursorCollection(this) {}

        TKeyCursorCollection::TImpl KeyCursorCollection;

      };  // TKeyCursorCollector

      private:

      TRepoTree RepoTree;

      size_t WalkerCount;

      Base::TTimer PresentWalkConsTimer;

      friend class TIndyContext;

    };  // TContext

    class TIndyContext
          : public Orly::Package::TContext {
      NO_COPY(TIndyContext);
      public:

      TIndyContext(
          const Rt::TOpt<Base::TUuid> &user_id,
          const Base::TUuid &session_id,
          Indy::TContext &context,
          Atom::TCore::TExtensibleArena *arena,
          Base::TScheduler *scheduler,
          Rt::TOpt<Base::Chrono::TTimePnt> now,
          Rt::TOpt<uint32_t> seed)
          : Orly::Package::TContext(user_id, session_id, arena, scheduler, now, seed), DataContext(context) {}

      virtual Orly::TContextBase &GetFlux() override{
        return DataContext;
      }

      virtual TKeyCursor *NewKeyCursor(TContextBase *context, const Indy::TIndexKey &pattern) const override {
        auto data_context = dynamic_cast<Indy::TContext *>(context);

        /*
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        std::cout << "NewKeyCursor(";
        pattern.GetState(state_alloc)->Accept(Sabot::TStateDumper(std::cout));
        std::cout << ")" << std::endl;
        */
        return new Indy::TContext::TKeyCursor(data_context, pattern);
      }

      virtual TKeyCursor *NewKeyCursor(TContextBase *context, const Indy::TIndexKey &from, const Indy::TIndexKey &to) const override {
        auto data_context = dynamic_cast<Indy::TContext *>(context);

        /*
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        std::cout << "NewKeyCursor(";
        pattern.GetState(state_alloc)->Accept(Sabot::TStateDumper(std::cout));
        std::cout << ")" << std::endl;
        */
        return new Indy::TContext::TKeyCursor(data_context, from, to);
      }

      private:

      Indy::TContext &DataContext;

    };  // TIndyContext

    /********************
      ***** inline ******
      ******************/

    inline TContext::TPresentWalker::operator bool() const {
      return Valid;
    }

    inline const TContext::TPresentWalker::TItem &TContext::TPresentWalker::operator*() const {
      assert(Valid);
      return Item;
    }

    inline TContext::TPresentWalker &TContext::TPresentWalker::operator++() {
      assert(Valid);
      Valid = static_cast<bool>(MinHeap);
      Refresh();
      return *this;
    }

  }  // Indy

}  // Orly
