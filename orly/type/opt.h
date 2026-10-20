/* <orly/type/opt.h>

   The optional type `T?`. Unary: interned by element type.
   Auto-unwrapped by the infix visitor's "TOpt unwrap" cases -- once
   one operand is optional, the result is wrapped in `TOpt` after
   recursing on the inner type. Powers orlyscript's `?` / `known` /
   `unknown` semantics.

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

    class TOpt : public TUnaryType<TOpt> {
      NO_COPY(TOpt);
      public:

      static TType Get(const TType &type);

      /* An optional of a type that may itself be optional. The language has no
         optional of an optional, so Get() asserts against one. Storage needs
         it: a variant is stored as a record with one optional field per arm
         (TVariant::GetLowered), so an arm whose payload is `T?` is stored as
         `T??`. The outer optional says whether the arm was taken and the inner
         one is the payload, which keeps "arm not taken" apart from "arm taken
         with an unknown payload" (#978). Only the storage lowering and its
         inverse (sabot_to_type) call this. */
      static TType GetStored(const TType &type);

      private:

      TOpt(const TType &elem) : TUnaryType(elem) {}
      virtual ~TOpt();

      virtual void Write(std::ostream &) const;

      friend class TInternedType;
    };  // TOpt

  }  // Type

}  // Orly