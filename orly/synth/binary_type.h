/* <orly/synth/binary_type.h>

   Synth-layer node for binary type expressions: container types
   like `[T]`, `{T}`, `{K: V}`. `ComputeSymbolicType()` lowers to
   the corresponding `Type::T*` (`TList`, `TSet`, `TDict`, ...).

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

#include <orly/synth/type.h>

namespace Orly {

  namespace Synth {

    class TBinaryType
        : public TType {
      NO_COPY(TBinaryType);
      public:

      typedef Type::TType (*TGet)(const Type::TType &, const Type::TType &);

      TBinaryType(TType *lhs, TType *rhs, TGet get);

      virtual ~TBinaryType();

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      virtual Type::TType ComputeSymbolicType() const;

      TGet Get;

      TType *Lhs;

      TType *Rhs;

    };  // TType

  }  // Synth

}  // Orly
