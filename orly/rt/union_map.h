/* <orly/rt/union_map.h>

   `UnionMap(gen, map_fn, start)` -- map each element of `gen` to a set
   via `map_fn` and union the results, starting from `start` (the empty
   set). Code-gen for orlyscript's `seq union_map elem` operator (#219).
   `start` is passed (rather than default-constructed inside) so the
   result type TRes is deducible at the call site, exactly as Reduce
   deduces it from its start argument.

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

#include <orly/rt/containers.h>
#include <orly/rt/generator.h>

namespace Orly {

  namespace Rt {

    template <typename TRes, typename TSrc>
    using TUnionMapFunc = std::function<TRes (const TSrc &that)>;

    template <typename TRes, typename TSrc>
    TRes UnionMap(const typename TGenerator<TSrc>::TPtr &gen,
                  const TUnionMapFunc<TRes, TSrc> &map_func,
                  TRes start) {
      for (auto it = gen->NewCursor(); it; ++it) {
        start = start | map_func(*it);
      }
      return start;
    }

  }  // Rt

}  // Orly
