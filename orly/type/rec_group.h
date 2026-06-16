/* <orly/type/rec_group.h>

   Construction and navigation of mutually-recursive type groups
   (issue #116, Path A / A2). The type-layer core, independent of synth
   and codegen.

   `MakeRecGroup` takes a strongly-connected group of variant defs as
   member "equations" -- a `TVariant`'s arm map per member, with a
   reference to sibling member `k` written as the placeholder
   `TGroupRef::Get({}, k)` (an empty group identity). It returns the
   members' interned `TType`s (normal `TVariant`s whose sibling references
   are real `TGroupRef`s into the group). Structurally-isomorphic groups
   produce equal member types: the group identity is canonical -- the
   members' inlined forms, canonicalized via orly/type/canon.h and ordered
   by mangled name, independent of the order the members are supplied in.

   `ResolveGroupRef` maps a `TGroupRef` back to the member `TType` it
   denotes -- a registry lookup, used by the accessor.

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

#include <vector>

#include <orly/type/group_ref.h>
#include <orly/type/impl.h>
#include <orly/type/variant.h>

namespace Orly {

  namespace Type {

    /* Build a mutually-recursive group from member equations (each a
       variant arm map; sibling refs are TGroupRef::Get({}, k) placeholders
       keyed only by member index). Returns the interned member types, in
       the same order as the input equations. */
    std::vector<TType> MakeRecGroup(const std::vector<TVariantElems> &members);

    /* The member type a group reference denotes (registry lookup). The
       group must have been built by MakeRecGroup. */
    TType ResolveGroupRef(const TGroupRef *group_ref);

    /* If `member` is a type built by MakeRecGroup, fills `members` with the
       whole group's member types (canonical order) and `index` with
       `member`'s position among them, and returns true. Otherwise returns
       false. Lets codegen enumerate a group's sibling classes starting from
       any one member. */
    bool TryGetGroupMembers(const TType &member, std::vector<TType> &members,
                            size_t &index);

    /* A group member's canonical inlined de Bruijn form: the self-recursive
       TVariant (sibling refs expanded, cycles minted as Type::TSelfRef) that
       MakeRecGroup already computed as the group identity. This is how a
       mutual-group value is lowered for STORAGE (issue #115): inlining turns a
       member into an ordinary self-recursive variant, so the sabot TSelfRef
       leaf and the rest of the single-recursion machinery carry it. `member`
       must be a type built by MakeRecGroup. */
    TType InlinedMemberType(const TType &member);

  }  // Type

}  // Orly
