/* <orly/type/self_ref.h>

   The self-reference type leaf: a de Bruijn-style back-reference to an
   enclosing variant type, making a recursive variant's intern key finite
   (issue #103). In

     tree is <| Leaf(int) | Branch(<{.l: tree, .r: tree}>) |>;

   the `tree` occurrences inside the declaration become `TSelfRef::Get(0)`,
   so the variant interns on an ordinary finite tag -> payload-type map and
   structural equality of recursive types stays pointer equality for free.

   `Depth` counts enclosing variant binders between the reference and its
   target (0 = the nearest enclosing variant). The v1 surface only accepts
   depth 0 (see the placement validation in orly/synth/type_def.cc), but
   the leaf itself is depth-general.

   Invariant: a TSelfRef appears only *inside* a variant's payload-type
   map. Expression types are always unrolled (orly/type/unroll.h) before
   they surface, so the operator type-check visitors never see this leaf
   and inherit the throwing default added to TType::TVisitor.

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

#include <cstddef>

#include <base/class_traits.h>
#include <orly/type/managed_type.h>

namespace Orly {

  namespace Type {

    /* The self-reference leaf. Interned by depth. */
    class TSelfRef : public TInternedType<TSelfRef, size_t> {
      NO_COPY(TSelfRef);
      public:

      /* The number of variant binders between the reference and its
         target; 0 is the nearest enclosing variant. */
      size_t GetDepth() const {
        return std::get<0>(GetKey());
      }

      /* TODO */
      static TType Get(size_t depth) {
        return TInternedType::Get(depth);
      }

      private:
      TSelfRef(size_t depth) : TInternedType(depth) {}
      virtual ~TSelfRef();

      virtual void Write(std::ostream &) const;

      friend class TInternedType;
    };  // TSelfRef

  }  // Type

}  // Orly
