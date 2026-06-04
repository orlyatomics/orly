/* <orly/type/unwrap.h>

   Free-function counterparts to the wrapper-stripping logic that
   visitors perform inline. `Unwrap(type)` removes `TMutable` and
   `TSeq` wrappers; `UnwrapMutable` / `UnwrapOptional` /
   `UnwrapSequence` strip the one specific wrapper named. Used in
   type-check code that needs to compare element types regardless of
   how they're wrapped.

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

namespace Orly {

  namespace Type {

    class TType;

    /* Returns the type with mutable and sequence unwrapped */
    TType Unwrap(const TType &type);

    /* Returns the type inside mutable */
    TType UnwrapMutable(const TType &type);

    /* Returns the type inside optional */
    TType UnwrapOptional(const Type::TType &type);

    /* Returns the type inside sequence */
    TType UnwrapSequence(const Type::TType &type);

  }  // Type

}  // Orly
