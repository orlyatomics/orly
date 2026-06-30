/* <orly/type/ensure_empty_object.h>

   `EnsureEmptyObject(type, pos_range)`: asserts that a given type is
   either not a `TObj` at all or an empty `TObj` -- i.e. that the
   function it came from took no parameters. Used by the infix
   visitors' "TFunc unwrap" cases to ensure a parameterised function
   call isn't accidentally treated as a value.

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

#include <orly/pos_range.h>

namespace Orly {

  namespace Type {

    class TType;

    void EnsureEmptyObject(const Type::TType &type, const TPosRange &pos_range);

  }  // Type

}  // Orly
