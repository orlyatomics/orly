/* <orly/atom/note_interner2.h>

   Interns notes.

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

#include <cassert>
#include <functional>

#include <absl/container/flat_hash_set.h>

#include <orly/atom/kit2.h>
#include <orly/sabot/compare_states.h>
#include <orly/sabot/get_depth.h>

namespace Orly {

  namespace Atom {

    /* Interns notes. */
    class TNoteInterner {
      NO_COPY(TNoteInterner);
      public:

      /* Open addressing, so interning a note doesn't allocate a node for it as well, which a result
         of many rows did once a row (#1018). OrderNotes sorts, so the iteration order doesn't matter. */
      using TNotes = absl::flat_hash_set<const TCore::TNote *, TCore::TNote::THash, TCore::TNote::TIsEq>;

      /* Do-little. */
      inline TNoteInterner();

      /* Delete the notes when we go. */
      ~TNoteInterner();

      inline const TNotes &GetNotes() const;

      /* The number of notes interned. */
      inline size_t GetSize() const;

      /* Make room for `count` more notes, so proposing them doesn't rehash the notes already here
         as the set grows (#1018). */
      inline void Reserve(size_t count);

      /* The bytes the interned notes hold, headers included (#694). */
      inline size_t GetByteSize() const;

      /* Give up our notes without deleting them one at a time: they move to a store that lives as long as the
         process and are never freed. Deleting millions of small notes one by one took up to a second, and
         a server being stopped holds up its exit for it (#1025); the one caller is a cancelled merge, whose
         process is going away. The interner is empty afterwards. */
      void Abandon();

      /* True iff. the given note is semantically equivalent to one of our internees. */
      bool IsKnown(const TCore::TNote *note) const;

      /* True iff. the given note is itself one of our internees. */
      bool IsOwned(const TCore::TNote *note) const;

      /* Return the interned version of the proposed note.
         The interner takes responsibility for deleting the proposed note. */
      const TCore::TNote *Propose(TCore::TNote *proposed_note);

      private:

      /* Our set of unique notes. */
      TNotes Notes;

      /* See GetByteSize(). */
      size_t ByteSize = 0UL;

    };  // TNoteInterner

    /* Inline */

    inline TNoteInterner::TNoteInterner() {}

    inline const TNoteInterner::TNotes &TNoteInterner::GetNotes() const {
      return Notes;
    }

    inline size_t TNoteInterner::GetSize() const {
      return Notes.size();
    }

    inline void TNoteInterner::Reserve(size_t count) {
      Notes.reserve(Notes.size() + count);
    }

    inline size_t TNoteInterner::GetByteSize() const {
      return ByteSize;
    }

    /* comparison function for notes. */
    inline static bool NoteCmp(const TCore::TNote *lhs_note,
                               TCore::TArena *lhs_arena,
                               const TCore::TNote *rhs_note,
                               TCore::TArena *rhs_arena,
                               void *lhs_state_alloc,
                               void *rhs_state_alloc,
                               void *lhs_type_alloc,
                               void *rhs_type_alloc) {
      TCore
        lhs_core(reinterpret_cast<TCore::TOffset>(lhs_note), lhs_note),
        rhs_core(reinterpret_cast<TCore::TOffset>(rhs_note), rhs_note);
      Sabot::State::TAny::TWrapper
        lhs(lhs_core.NewState(lhs_arena, lhs_state_alloc)),
        rhs(rhs_core.NewState(rhs_arena, rhs_state_alloc));
      size_t
        lhs_depth = Sabot::GetDepth(*Sabot::Type::TAny::TWrapper(lhs->GetType(lhs_type_alloc))),
        rhs_depth = Sabot::GetDepth(*Sabot::Type::TAny::TWrapper(rhs->GetType(rhs_type_alloc)));
      return lhs_depth < rhs_depth || (lhs_depth == rhs_depth && IsLt(Sabot::CompareStates(*lhs, *rhs, lhs_type_alloc, rhs_type_alloc)));
    }

    /* Sort the notes into an our vector */
    void OrderNotes(std::vector<const TCore::TNote *> &out_vec, const TNoteInterner::TNotes &notes, TCore::TArena *arena);

  }  // Atom

}  // Orly
