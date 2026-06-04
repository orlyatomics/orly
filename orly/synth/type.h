/* <orly/synth/type.h>

   Abstract base for every synth-layer type expression. Subclasses
   implement `ComputeSymbolicType()` to lower the CST type-node
   into a `Type::TType`; `GetSymbolicType()` caches the result.
   `ForEachRef` exposes nested `TAnyRef`s (e.g. typedef lookups in
   `TRefType`) for the multi-pass build driver.

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

#include <cassert>
#include <functional>

#include <base/class_traits.h>
#include <orly/type.h>
#include <orly/synth/scope_and_def.h>

namespace Orly {

  namespace Synth {

    /* TODO */
    class TType {
      NO_COPY(TType);
      public:

      /* TODO */
      virtual ~TType();

      /* TODO */
      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb) = 0;

      /* TODO */
      const Type::TType &GetSymbolicType() const;

      protected:

      /* TODO */
      TType();

      /* TODO */
      virtual Type::TType ComputeSymbolicType() const = 0;

      private:

      /* TODO */
      mutable Type::TType CachedSymbolicType;

    };  // TType

  }  // Synth

}  // Orly
