/* <orly/type/list.h>

   The list type `[TElem]`. Unary: interned by element type via
   `TUnaryType<TList>`. Supports concatenation (`+` of two lists of
   the same element type), comparison (element-wise), and sequence
   operations (`reverse_of`, `sorted_by`, slicing).

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

    class TList : public TUnaryType<TList> {
      NO_COPY(TList);

      TList(const Type::TType &elem) : TUnaryType(elem) {}
      virtual ~TList();

      virtual void Write(std::ostream &strm) const;

      friend class TInternedType;
    };  // TList

  }  // Type

}  // Orly