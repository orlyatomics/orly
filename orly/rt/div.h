/* <orly/rt/div.h>

   `Div(lhs, rhs)` for `int64_t` and `double`, with `TDivisionByZeroError`
   thrown on `rhs == 0`. The opt-aware overload returns `TOpt<int64_t>`
   when either operand is unknown -- propagating optionality through
   division so that orlyscript's `/` operator composes with `?T`.

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

#include <orly/rt/opt.h>
#include <orly/rt/runtime_error.h>
#include <orly/rt/operator.h>

namespace Orly {

  namespace Rt {

    DEFINE_ERROR(TDivisionByZeroError, TRuntimeError, "Attempted to perform a division by zero");

    inline double Div(double lhs, double rhs) {
      if (rhs == 0) {
        throw TDivisionByZeroError(HERE);
      }
      return lhs / rhs;
    }

    inline int64_t Div(int64_t lhs, int64_t rhs) {
      if (rhs == 0) {
        throw TDivisionByZeroError(HERE);
      }
      return lhs / rhs;
    }

    template <typename TLhs, typename TRhs>
    TOpt<int64_t> Div(TLhs &&lhs, TRhs &&rhs) {
      return IsKnown(lhs) && IsKnown(rhs) ? Div(GetVal(lhs), GetVal(rhs)) : TOpt<int64_t>();
    }

  }  // Rt

}  // Orly
