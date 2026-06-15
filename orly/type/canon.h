/* <orly/type/canon.h>

   Equirecursive equality and canonicalization for recursive types
   (issue #116, mutual recursion, Path A).

   A recursive type is a finite de Bruijn term with `TSelfRef` back-edges
   (orly/type/self_ref.h). The same infinite ("unfolded") type can be
   written as several distinct finite terms -- most importantly, the same
   mutually-recursive cycle entered at different members:

     a = <| X(<| Y(self@1) |>) |>     b = <| Y(<| X(self@1) |>) |>

   Here `Unroll(a.X)` yields `<| Y(a) |>`, which is the *same* type as `b`
   but a different finite term (the loop closes at a different node). Since
   the type system interns structurally and compares by pointer, the two
   would be unequal, breaking the accessor's result type.

   `Equiv(s, t)` decides equirecursive equality (same infinite unfolding),
   coinductively. `Canon(t)` rewrites a (closed) recursive type to a
   canonical minimal de Bruijn form by folding at the first equirecursively-
   equivalent enclosing binder -- so `Canon(<| Y(a) |>)` and `Canon(b)` are
   the identical interned type. A single self-recursive type is already
   minimal, so `Canon` is the identity on it (the #103/#120/#121/#125
   shapes are unaffected); only mutual cycles are rewritten.

   This is the standalone, unit-tested core (#116 Phase 1). It is not yet
   wired into interning; that is a later phase.

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

    /* True iff s and t are equirecursively equal -- i.e. they have the same
       infinite unfolding. Both should be closed (every TSelfRef bound by an
       enclosing variant within the type). Reduces to pointer equality for
       non-recursive types. */
    bool Equiv(const TType &s, const TType &t);

    /* The canonical minimal de Bruijn form of a closed recursive type:
       equirecursively-equal inputs produce the identical interned TType.
       The identity on non-recursive types and on already-minimal self-
       recursive types. */
    TType Canon(const TType &type);

  }  // Type

}  // Orly
