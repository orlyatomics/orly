/* <orly/rt/sort.h>

   `Sort(val, comp)` for `std::vector` and `TOpt<std::vector>`.
   Returns a sorted copy (the original is `const &`). Code-gen for
   orlyscript's `sorted_by lhs < rhs` operator. The general template
   is `= delete` so unsupported types fail at compile time.

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

#include <algorithm>
#include <vector>

#include <base/class_traits.h>
#include <orly/rt/opt.h>

namespace Orly {

  namespace Rt {

    template <typename TVal>
    TVal Sort(const TVal &) = delete;

    template <typename TVal>
    std::vector<TVal> Sort(const std::vector<TVal> &val,
          const std::function<bool (const TVal &, const TVal &)> &comp) {
      std::vector<TVal> ret(val);
      /* Stable, so ties keep their input order on every build (#698).
         `std::sort` left that order unspecified, and with a comparator that
         isn't a strict weak ordering (`lhs <= rhs`) it was undefined behaviour
         that could read past the end of the vector. A comparator that is true
         for an element against itself is non-strict; one check on the first
         element finds it, and we then sort by its strict part, `comp(a, b) &&
         !comp(b, a)`, which turns `<=` into `<` and keeps ties stable too. That
         costs a second call per comparison, but only for such comparators. */
      if (ret.size() > 1 && comp(ret.front(), ret.front())) {
        std::stable_sort(ret.begin(), ret.end(), [&comp](const TVal &lhs, const TVal &rhs) {
          return comp(lhs, rhs) && !comp(rhs, lhs);
        });
      } else {
        std::stable_sort(ret.begin(), ret.end(), comp);
      }
      return ret;
    }

    template <typename TVal>
    TOpt<std::vector<TVal>> Sort(const TOpt<std::vector<TVal>> &val,
          const std::function<bool (const TVal &, const TVal &)> &comp) {
      return val.IsKnown() ? TOpt<std::vector<TVal>>(Sort(val.GetVal(), comp))
                           : TOpt<std::vector<TVal>>();
    }

  }  // Rt

}  // Orly