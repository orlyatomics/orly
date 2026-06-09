/* <mpl/get_difference.h>

   Type-level set difference: `GetDifference<L, R>` is a `TTypeList`
   containing every element of `L` that is not in `R`. Pairs with
   `get_union` and `get_intersection` for the full set algebra.

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

#include <base/identity.h>
#include <base/mpl/contains.h>
#include <base/mpl/type_list.h>
#include <base/mpl/type_set.h>

namespace Mpl {

  /* GetDifference. */
  template <typename TList, typename TLhs, typename TRhs>
  struct GetDifferenceRecur;

  template <typename... TElems, typename TRhs>
  struct GetDifferenceRecur<TTypeList<TElems...>, TTypeList<>, TRhs> : public Base::identity<TTypeSet<TElems...>> {};

  template <typename TList, typename TElem, typename... TMoreElems, typename TRhs>
  struct GetDifferenceRecur<TList, TTypeList<TElem, TMoreElems...>, TRhs> : public std::conditional_t<
      Contains<TRhs, TElem>::value,
      GetDifferenceRecur<TList, TTypeList<TMoreElems...>, TRhs>,
      GetDifferenceRecur<TExtend<TList, TTypeList<TElem>>, TTypeList<TMoreElems...>, TRhs>> {};

  template <typename TLhs, typename TRhs>
  struct GetDifference : public GetDifferenceRecur<TTypeList<>, Mpl::TGetList<TLhs>, TRhs> {};

  template <typename TLhs, typename TRhs>
  using TGetDifference = typename GetDifference<TLhs, TRhs>::type;

}  // Mpl
