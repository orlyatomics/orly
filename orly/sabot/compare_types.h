/* <orly/sabot/compare_types.h>

   `CompareTypes(lhs, rhs)` returns three-way comparison
   (`Atom::TComparison`) over two `Type::TAny`s; `CompareStates`
   (in `compare_states.h`) uses this for the dispatch step before
   recursing into element-wise state comparison.

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
#include <ostream>

#include <orly/atom/comparison.h>
#include <orly/sabot/state.h>
#include <orly/sabot/type.h>

namespace Orly {

  namespace Sabot {

    /* Forward Declaration. */
    Atom::TComparison CompareStates(const State::TAny &lhs, const State::TAny &rhs, void *lhs_type_alloc, void *rhs_type_alloc);

    Atom::TComparison CompareTypes(const Type::TAny &lhs, const Type::TAny &rhs);

  }  // Sabot

}  // Orly
