/* <orly/type/group_ref.h>

   The group-reference leaf: a reference from one member of a mutually-
   recursive type group to another member (issue #116, Path A / A2).

   A mutually-recursive SCC of variant defs -- `a is <| X(b) |>;
   b is <| Y(a) |>;` -- is represented WITHOUT inlining: each member is a
   normal `TVariant` whose arms reference sibling members through this
   leaf, `TGroupRef(group, index)`, rather than expanding them. The leaf
   breaks the otherwise-cyclic intern graph the way `TSelfRef` does for
   single-def recursion, but it preserves type *sharing* (members stay
   distinct, separately-emittable types) -- which the de Bruijn inlining
   approach destroyed.

   `Group` is the group's canonical identity (orly/type/rec_group.h): the
   ordered vector of the members' canonicalized inlined forms, so that
   structurally-isomorphic groups share one identity and their members
   intern equal. `Index` selects the member within that identity.
   Resolving a `TGroupRef` to the member `TVariant` it denotes is a
   registry lookup (rec_group.h), not a structural reconstruction.

   Like `TSelfRef`, this leaf appears only inside a group member's
   payload map; it never surfaces as an expression type (the accessor
   navigates it to the member type). Its `TVisitor` overload therefore
   inherits the throwing default, except in the structural walkers.

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
#include <vector>

#include <base/class_traits.h>
#include <orly/type/managed_type.h>

namespace Orly {

  namespace Type {

    /* The canonical identity of a recursive group: the ordered vector of
       its members' canonicalized inlined forms. */
    typedef std::vector<TType> TGroupId;

    /* The group-reference leaf. Interned by (group identity, member index). */
    class TGroupRef : public TInternedType<TGroupRef, TGroupId, size_t> {
      NO_COPY(TGroupRef);
      public:

      /* The group this reference belongs to. */
      const TGroupId &GetGroup() const {
        return std::get<0>(GetKey());
      }

      /* The referenced member's index within the group identity. */
      size_t GetIndex() const {
        return std::get<1>(GetKey());
      }

      static TType Get(const TGroupId &group, size_t index) {
        return TInternedType::Get(group, index);
      }

      private:
      TGroupRef(const TGroupId &group, size_t index) : TInternedType(group, index) {}
      virtual ~TGroupRef();

      virtual void Write(std::ostream &) const;

      friend class TInternedType;
    };  // TGroupRef

  }  // Type

}  // Orly
