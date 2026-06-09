/* <mpl/is_same.h>

   Specialises `std::is_same` for `Mpl::TTypeSet<...>` so two sets
   compare equal regardless of element order (`std::is_same` on the
   underlying tuple would require positional match). Use
   `std::is_same`/`std::is_same_v` directly to compare type sets.

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

#include <base/mpl/get_size.h>
#include <base/mpl/is_subset_of.h>
#include <base/mpl/type_set.h>

namespace std {

  /* std::is_same<> specialization for Mpl::TTypeSet<Elems...>. */
  template <typename... TElems>
  struct is_same<Mpl::TTypeSet<TElems...>, Mpl::TTypeSet<TElems...>>
      : public std::integral_constant<
                   bool,
                   (Mpl::GetSize<Mpl::TTypeSet<TElems...>>() ==
                    Mpl::GetSize<Mpl::TTypeSet<TElems...>>()) &&
                       Mpl::IsSubsetOf<Mpl::TTypeSet<TElems...>,
                                       Mpl::TTypeSet<TElems...>>()> {};

  /* std::is_same<> specialization for Mpl::TTypeSet<Elems...>. */
  template <typename... TLhsElems, typename... TRhsElems>
  struct is_same<Mpl::TTypeSet<TLhsElems...>, Mpl::TTypeSet<TRhsElems...>>
      : public std::integral_constant<
                   bool,
                   (Mpl::GetSize<Mpl::TTypeSet<TLhsElems...>>() ==
                    Mpl::GetSize<Mpl::TTypeSet<TRhsElems...>>()) &&
                       Mpl::IsSubsetOf<Mpl::TTypeSet<TLhsElems...>,
                                       Mpl::TTypeSet<TRhsElems...>>()> {};

}  // std
