/* <mpl/enable_if.h>

   SFINAE helper: `EnableIf<P>` is a well-formed type only when
   `P::value` is true; otherwise the substitution fails and the
   overload is dropped. `DisableIf<P>` is `EnableIf<std::negation<P>>`.
   Used as a template default argument or return type -- the unary
   counterpart of `std::enable_if_t`.

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

  class EnableIfImpl {
    private:

    enum class Enable {};

    public:

    template <typename Pred>
    using EnableIf = std::enable_if_t<Pred::value, Enable>;

  };  // EnableIfImpl

  template <typename Pred>
  using EnableIf = EnableIfImpl::EnableIf<Pred>;

  template <typename Pred>
  using DisableIf = EnableIf<std::negation<Pred>>;

}  // Mpl
