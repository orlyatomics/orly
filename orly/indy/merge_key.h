/* <orly/indy/merge_key.h>

   `TMergeKey` -- one input's current key in a k-way merge of present walkers, with a tuple key's top-level elements copied out
   of its arena so the merge can order it without pinning its note on every comparison (#1018).

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

#include <array>
#include <cassert>
#include <cstddef>

#include <orly/atom/comparison.h>
#include <orly/atom/kit2.h>
#include <orly/indy/present_walker.h>

namespace Orly {

  namespace Indy {

    /* The merge pins each key once, when its input moves to it, instead of pinning both keys on each of the comparisons the
       heap makes. */
    class TMergeKey {
      public:

      /* Keys with more top-level elements than this are not copied; they order through TItem::CompareKeys. */
      static constexpr size_t MaxElems = 8UL;

      /* Take `item`'s key as this input's current one. */
      void Load(const TPresentWalker::TItem &item) {
        Arena = item.KeyArena;
        if (!Arena || !item.Key.TryCopyTupleElems(Arena, Elems.data(), MaxElems, Count)) {
          Arena = nullptr;
        }
      }

      /* Forget the key, so comparisons order it through TItem::CompareKeys. */
      void Clear() {
        Arena = nullptr;
      }

      /* Order lhs's key against rhs's exactly as lhs.CompareKeys(rhs) would, where lhs_key and rhs_key were loaded from them. */
      [[nodiscard]] static Atom::TComparison Compare(const TPresentWalker::TItem &lhs, const TMergeKey &lhs_key,
                                                     const TPresentWalker::TItem &rhs, const TMergeKey &rhs_key) {
        Atom::TComparison comp;
        /* CompareKeys orders keys in one arena by offset when it can, so it orders those itself. */
        if (lhs_key.Arena && rhs_key.Arena && lhs_key.Arena != rhs_key.Arena &&
            Atom::TCore::TryQuickOrderElems(lhs_key.Arena, lhs_key.Elems.data(), lhs_key.Count,
                                            rhs_key.Arena, rhs_key.Elems.data(), rhs_key.Count, comp)) {
          assert(lhs.KeyArena == lhs_key.Arena && rhs.KeyArena == rhs_key.Arena);
          assert(comp == lhs.CompareKeys(rhs));
          return comp;
        }
        return lhs.CompareKeys(rhs);
      }

      private:

      /* The arena the key's elements point into; null when the key was not copied. */
      Atom::TCore::TArena *Arena = nullptr;

      /* The number of elements copied into Elems. */
      size_t Count = 0UL;

      /* The key's top-level elements. */
      std::array<Atom::TCore, MaxElems> Elems;

    };  // TMergeKey

  }  // Indy

}  // Orly
