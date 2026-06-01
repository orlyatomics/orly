/* <orly/desc.h>

   A wrapper providing descending ordering.

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

#include <cassert>
#include <type_traits>
#include <utility>

namespace Orly {

  /* A wrapper providing descending ordering. */
  template <typename TVal>
  class TDesc {
    public:

    /* Do-little. */
    TDesc() {}

    /* Construct from any value convertible to TVal.  SFINAE-excludes
       TDesc itself so this template doesn't shadow the implicit
       copy/move ctors -- without the exclusion, `TDesc<T> b(a)` with
       `a` a non-const lvalue TDesc<T> picks this template (it's a
       better overload than the implicit copy ctor's const-adjusting
       reference bind), then tries to instantiate `TVal::TVal(TDesc<T>)`
       which fails.  Latent in C++17, exposed by C++23 overload-
       resolution tightening. */
    template <typename TArg,
              typename = std::enable_if_t<
                  !std::is_same_v<std::decay_t<TArg>, TDesc>>>
    TDesc(TArg &&arg)
        : Val(std::forward<TArg>(arg)) {}

    /* Behave like a smart pointer. */
    const TVal &operator*() const {
      return Val;
    }

    /* Behave like a smart pointer. */
    TVal &operator*() {
      return Val;
    }

    /* Behave like a smart pointer. */
    const TVal *operator->() const {
      return &Val;
    }

    /* Behave like a smart pointer. */
    TVal *operator->() {
      return &Val;
    }

    #if 0
    /* TODO */
    operator const TVal &() const {
      return Val;
    }
    #endif

    /* Provide the opposite ordering of TVal. */
    bool operator==(const TDesc &that) const {
      return Val == that.Val;
    }

    /* Provide the opposite ordering of TVal. */
    bool operator!=(const TDesc &that) const {
      return Val != that.Val;
    }

    /* Provide the opposite ordering of TVal. */
    bool operator<(const TDesc &that) const {
      return Val > that.Val;
    }

    /* Provide the opposite ordering of TVal. */
    bool operator<=(const TDesc &that) const {
      return Val >= that.Val;
    }

    /* Provide the opposite ordering of TVal. */
    bool operator>(const TDesc &that) const {
      return Val < that.Val;
    }

    /* Provide the opposite ordering of TVal. */
    bool operator>=(const TDesc &that) const {
      return Val <= that.Val;
    }

    private:

    /* The value we wrap.*/
    TVal Val;

  };  // TDesc

}  // Orly
