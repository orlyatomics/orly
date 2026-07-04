/* <orly/synth/unary_type.h>

   Synth-layer node for unary type expressions (notably `T?` --
   optional of `T`). Carries the inner type expression.
   `ComputeSymbolicType()` lowers to `Type::TOpt` (or other unary
   type wrappers).

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

#include <functional>

#include <orly/synth/type.h>

namespace Orly {

  namespace Synth {

    class TUnaryType
        : public TType {
      NO_COPY(TUnaryType);
      public:

      using TGet = std::function<Type::TType (const Type::TType &)>;

      TUnaryType(TType *type, TGet get);

      virtual ~TUnaryType();

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      virtual Type::TType ComputeSymbolicType() const;

      TType *Type;

      TGet Get;

    };  // TType

  }  // Synth

}  // Orly
