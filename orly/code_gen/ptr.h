/* <orly/code_gen/ptr.h>

   The code-gen layer's shared_ptr aliases: `TPtr<T>` and `TPtrC<T>`
   (pointer-to-const). Promoted out of effect.h (#309) so any code-gen
   header can use them without dragging in the mutation machinery;
   prefer these over spelling out std::shared_ptr in new code-gen code.

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

#include <memory>

namespace Orly {

  namespace CodeGen {

    template <typename TTarget>
    using TPtrC = std::shared_ptr<const TTarget>;

    template <typename TTarget>
    using TPtr = std::shared_ptr<TTarget>;

  }  // CodeGen

}  // Orly
