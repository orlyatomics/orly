/* <orly/type/err.h>

   Error-carrying type wrapper. Unary: `TErr<T>` carries a typed
   payload (the type the error sits in for). Used by the type checker
   to model "this expression should have produced a `T` but reached
   an error first" and is auto-unwrapped by every infix visitor.

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

    class TErr : public TUnaryType<TErr> {
      NO_COPY(TErr);

      TErr(const TType &elem) : TUnaryType(elem) {}
      virtual ~TErr();

      virtual void Write(std::ostream &) const;
      friend class TInternedType;
    };  // TErr

  }  // Type

}  // Orly