/* <orly/type/has_optional.h>

   `HasOptional(type)`: recursive predicate that returns `true` if
   `type` itself is a `TOpt` or contains a `TOpt` anywhere in its
   structure (a `TList<TOpt<TInt>>` or a `TObj` with at least one
   optional field). Used by comparison and mutation type-checkers to
   decide whether the result type needs an outer `TOpt` wrapper.

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

    /* TODO */
    bool HasOptional(const TType &type);

  }  // Type

}  // Orly
