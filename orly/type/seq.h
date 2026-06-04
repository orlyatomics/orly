/* <orly/type/seq.h>

   The sequence type `[..]T` -- a lazy stream of `T` values produced
   by `reduce` / `take` / range expressions. Unary: interned by
   element type. Auto-unwrapped by the infix visitor's "TSeq unwrap"
   cases -- once one operand is a sequence, the result is wrapped in
   `TSeq` so the lazy structure propagates through compound
   expressions.

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

#include <orly/type/managed_type.h>

namespace Orly {

  namespace Type {

    /* TODO */
    class TSeq : public TUnaryType<TSeq> {
      NO_COPY(TSeq);

      TSeq(const TType &elem) : TUnaryType(elem) {}
      virtual ~TSeq();

      virtual void Write(std::ostream &) const;

      friend class TInternedType;
    };  // TSeq

  }  // Type

}  // Orly