/* <mpl/not.h>

   Negates a predicate type-trait: `Not<P>` is `true` iff `P::value`
   is `false`. Useful as a building block for `EnableIf` /
   `DisableIf` and for combining with `And` / `Or`.

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

#include <type_traits>

namespace Mpl {

  template <typename Pred>
  using Not = std::integral_constant<bool, !Pred::value>;

}  // Mpl
