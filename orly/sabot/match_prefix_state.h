/* <orly/sabot/match_prefix_state.h>

   `MatchPrefixState(lhs, rhs)` checks whether `lhs`'s state is a
   prefix of `rhs`'s (returns one of `TMatchResult::NoMatch`,
   `PrefixMatch`, or `Unifies`). Used by the key-pattern matching path
   when the runtime evaluates `keys <[pattern]>` with `free::(T)`
   wildcards.

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

#include <orly/sabot/match_prefix_type.h>
#include <orly/sabot/state.h>

namespace Orly {

  namespace Sabot {

    TMatchResult MatchPrefixState(const State::TAny &lhs, const State::TAny &rhs);

  }  // Sabot

}  // Orly