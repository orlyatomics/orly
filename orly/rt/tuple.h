/* <orly/rt/tuple.h>

   The 'tuple' runtime value.

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

#include <cstddef>
#include <tuple>
#include <utility>

#include <base/class_traits.h>
#include <orly/desc.h>
#include <orly/rt/operator.h>

namespace Orly {

  namespace Rt {

    /* Match two tuples: structural, elementwise Match (so element types with
       their own Match semantics -- nested tuples, optionals, containers --
       compare through it, not through raw operator==). */
    template <typename... TArgs>
    bool Match(const std::tuple<TArgs...> &lhs, const std::tuple<TArgs...> &rhs) {
      return [&]<std::size_t... Idx>(std::index_sequence<Idx...>) {
        return (Match(std::get<Idx>(lhs), std::get<Idx>(rhs)) && ...);
      }(std::index_sequence_for<TArgs...>{});
    }

    /* MatchLess two tuples: lexicographic -- MatchLess of the first
       elementwise non-Match; false when every element Matches. */
    template <typename... TArgs>
    bool MatchLess(const std::tuple<TArgs...> &lhs, const std::tuple<TArgs...> &rhs) {
      bool less = false;
      [&]<std::size_t... Idx>(std::index_sequence<Idx...>) {
        /* The fold short-circuits at the first non-Match element, whose
           MatchLess decides the order. */
        ((Match(std::get<Idx>(lhs), std::get<Idx>(rhs))
              ? true
              : (less = MatchLess(std::get<Idx>(lhs), std::get<Idx>(rhs)), false)) && ...);
      }(std::index_sequence_for<TArgs...>{});
      return less;
    }

    template <typename TVal>
    inline size_t GetTupleSize(const TVal &) {
      return std::tuple_size<TVal>::value;
    }

  }  // Rt

}  // Orly