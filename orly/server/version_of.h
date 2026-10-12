/* <orly/server/version_of.h>

   The answer to `version of {pov};` (#889): for the global POV its newest version, a number; for any other POV a
   vector with the newest version of its repo and of each repo up to the global one, `(s_pov, s_parent, ..., s_global)`,
   which can be read only until something after the first of them is promoted (docs/design/versioned-reads.md §4.2).

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

#include <cstdint>
#include <optional>
#include <string>

namespace Orly {

  namespace Server {

    struct TVersionOf {
      /* Set for the global POV. */
      std::optional<uint64_t> Number;

      /* Set for any other POV. */
      std::string Vector;
    };  // TVersionOf

  }  // Server

}  // Orly
