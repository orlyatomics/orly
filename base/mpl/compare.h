/* <mpl/compare.h>

   Type-level comparison operators on `::value` of integral-constant
   types: `Lt`, `Gt`, `LtEq`, `GtEq`, `EqEq`, `Neq`. Composes with
   `Mpl::And` / `Mpl::Not` so you can write `Or<Lt<A, B>, EqEq<A, B>>`
   the way you'd write `a < b || a == b` at runtime.

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

#include <base/mpl/and.h>
#include <base/mpl/not.h>

namespace Mpl {

  template <typename Lhs, typename Rhs>
  using Lt = std::integral_constant<bool, Lhs::value < Rhs::value>;

  template <typename Lhs, typename Rhs>
  using Gt = Lt<Rhs, Lhs>;

  template <typename Lhs, typename Rhs>
  using LtEq = Not<Gt<Lhs, Rhs>>;

  template <typename Lhs, typename Rhs>
  using GtEq = LtEq<Rhs, Lhs>;

  template <typename Lhs, typename Rhs>
  using EqEq = And<LtEq<Lhs, Rhs>, GtEq<Lhs, Rhs>>;

  template <typename Lhs, typename Rhs>
  using Neq = Not<EqEq<Lhs, Rhs>>;

}  // Mpl
