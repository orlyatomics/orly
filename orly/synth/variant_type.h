/* <orly/synth/variant_type.h>

   Synth-layer node for variant / tagged-union type expressions
   (`<| Tag(T) | Tag2 | ... |>`, #95 Phase 3). Holds the parsed
   tag-name to payload type-expression map. `ComputeSymbolicType()`
   lowers to `Type::TVariant`. A tag-only arm's payload is the empty
   object (the unit type), matching `orly/type/variant.h`.

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

#include <map>
#include <string>

#include <orly/orly.package.cst.h>
#include <orly/synth/type.h>

namespace Orly {

  namespace Synth {

    class TVariantType
        : public TType {
      NO_COPY(TVariantType);
      public:

      TVariantType(const Package::Syntax::TVariantType *variant_type);

      virtual ~TVariantType();

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      void Cleanup();

      virtual Type::TType ComputeSymbolicType() const;

      /* tag name -> payload type (nullptr-free; a tag-only arm gets a
         synthesized empty-object type node). */
      std::map<std::string, TType *> Arms;

    };  // TVariantType

  }  // Synth

}  // Orly
