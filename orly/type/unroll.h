/* <orly/type/unroll.h>

   Self-reference substitution for recursive variant types (issue #103).

   A recursive variant's payload-type map contains `TSelfRef` leaves
   (orly/type/self_ref.h). Whenever an arm's payload type surfaces as an
   *expression* type -- the constructor's payload check, the `.<Tag>`
   accessor, a `when` arm -- it must first be unrolled: every self-
   reference bound by the variant is replaced by the variant type itself,
   one level of the mu-type expansion. E.g. for

     tree is <| Leaf(int) | Branch(<{.l: tree, .r: tree}>) |>;

   `Unroll(<{.l: self, .r: self}>, tree)` yields `<{.l: tree, .r: tree}>`
   with `tree` the interned recursive variant. This maintains the
   invariant that a TSelfRef never escapes into an expression's type.

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

#include <orly/type/impl.h>

namespace Orly {

  namespace Type {

    /* True iff the type structurally contains a TAny leaf (at any depth).
       TAny arises only from an as-yet-unresolved recursive-call result
       (TFunction::GetReturnType), so a type for which this holds is not yet
       fully resolved and must not be memoized: TExpr::GetType refuses to
       cache it, so the expression recomputes (and the recursion resolves to a
       concrete type) by codegen time. The compound generalization of the
       shallow `Is<TAny>` check (#128 / #104 recursive-variant widening). */
    bool HasAny(const TType &type);

    /* True iff the type structurally contains a TSelfRef leaf (at any
       depth, bound or free). True both for a recursive variant itself and
       for any type that embeds one (e.g. a record with a recursive-variant
       field) -- use this where recursion anywhere inside is the question
       (the sabot encoders). */
    bool HasSelfRef(const TType &type);

    /* True iff the type structurally contains a TGroupRef leaf (at any
       depth) -- a reference into a mutually-recursive group (#116). The
       mutual analogue of HasSelfRef. */
    bool HasGroupRef(const TType &type);

    /* True iff the type contains a FREE group reference: one not nested
       inside a TVariant. A group member's raw arm payload (`<{.head: ref}>`)
       has free group refs (the refs are the member's recursive edges); the
       UNROLLED payload, whose fields are complete sibling variants, has none
       (their internal group refs are owned by those variants). The mutual
       analogue of HasFreeSelfRef -- used to decide whether a payload needs
       the boxed representation vs. is a normal standalone record/type. */
    bool HasFreeGroupRef(const TType &type);

    /* True iff the type contains a FREE self-reference: one whose de
       Bruijn depth reaches past the variants nested inside `type` itself.
       A recursive variant's raw arm payload has free self-references (they
       are bound by the variant, which is outside the payload); the
       unrolled payload, or a type that merely embeds a complete recursive
       variant, has none. Use this to decide whether a payload needs the
       boxed representation. */
    bool HasFreeSelfRef(const TType &type);

    /* Rewrite every TSelfRef bound by `binder` to `binder` itself, where
       `binder` is the (interned, recursive) variant type whose payload
       contains `type`. Self-references bound by variants nested *inside*
       `type` are left alone (their binder travels with them). */
    TType Unroll(const TType &type, const TType &binder);

  }  // Type

}  // Orly
