/* <orly/indy/present_walker.h>

   Walk the keys in a given line and, for each which exists or is tombstoned, return its most recent op.

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

#include <base/class_traits.h>
#include <base/no_throw.h>
#include <base/uuid.h>
#include <orly/atom/kit2.h>
#include <orly/indy/fiber/fiber.h>
#include <orly/indy/sequence_number.h>
#include <orly/shared_enum.h>

namespace Orly {

  namespace Indy {

    /* Walk the keys in a given line and, for each which exists or is tombstoned, return its most recent op.
       Ignore any updates which occur on or after a given limiting sequence number. */
    class TPresentWalker {
      NO_COPY(TPresentWalker);
      public:

      /* An item returned by a walker, describing the relevant portions an update which was previously pushed to the repo. */
      class TItem {
        public:

        /* Do-little. */
        TItem() : SequenceNumber(0UL), KeyArena(nullptr), OpArena(nullptr), Mutator(TMutator::Assign), UpdateId() {}

        /* TODO */
        bool operator<(const TItem &that) const {
          Atom::TComparison comp;
          if (KeyArena && that.KeyArena && Key.TryQuickOrderComparison(KeyArena, that.Key, that.KeyArena, comp)) {
          } else {
            void *lhs_state_alloc = alloca(Sabot::State::GetMaxStateSize() * 2);
            void *rhs_state_alloc = reinterpret_cast<uint8_t *>(lhs_state_alloc) + Sabot::State::GetMaxStateSize();
            comp = Sabot::OrderStates(*Sabot::State::TAny::TWrapper(Key.NewState(KeyArena, lhs_state_alloc)),
                                      *Sabot::State::TAny::TWrapper(that.Key.NewState(that.KeyArena, rhs_state_alloc)));
          }
          return Atom::IsLt(comp) || (Atom::IsEq(comp) && SequenceNumber >= that.SequenceNumber);
        }

        /* The sequence number of the update which contained the key-op pair. */
        TSequenceNumber SequenceNumber;

        /* The key being operated on. */
        Atom::TCore Key;

        /* The operation stored for the key. */
        Atom::TCore Op;

        /* The arena to look the key core up in. */
        Atom::TCore::TArena *KeyArena;

        /* The arena to look the op core up in. */
        Atom::TCore::TArena *OpArena;

        /* The mutator that produced Op. For TMutator::Assign (every
           in-tree entry pre-#49-phase-2), Op is the resolved value of
           the key. For commutative non-Assign mutators (Add, Mult, Or,
           ...), Op is the RHS of that mutation and the context-level
           walker (orly/indy/context.cc) folds same-mutator runs to
           produce the actual resolved value. */
        TMutator Mutator;

        /* Logical update id (the per-transaction TUuid from session.cc's
           TUpdate::NewUpdate(...)). Same logical update committed to a
           child POV and then Tetris-pushed to a parent POV ends up
           living in TWO TRepos with two different SequenceNumbers but
           the SAME UpdateId. The fold in context.cc uses this to dedup:
           without it, the brief window between Tetris push-to-parent
           and pop-from-child causes a deferred {Add, n} to be visible
           in both repos and double-counted. Populated by the in-memory
           walker; defaults to a zero Uuid for disk-walker entries
           (which can't hit the cross-repo duplication issue). */
        Base::TUuid UpdateId;

      };  // TItem

      /* True iff. we have an item. */
      virtual operator bool() const = 0;

      /* The current item. */
      virtual const TItem &operator*() const = 0;

      /* Walk to the next item, if any. */
      virtual TPresentWalker &operator++() = 0;

      /* TODO */
      virtual ~TPresentWalker() {}

      protected:

      enum TSearchKind {
        Match,
        Range
      };

      /* Prepare to walk. */
      TPresentWalker(TSearchKind search_kind) : SearchKind(search_kind) {}

      /* TODO */
      TSearchKind SearchKind;

    };  // TPresentWalker

  }  // Indy

}  // Orly
