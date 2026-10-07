/* <orly/rt/string.h>

   Built in library functions for strings.

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

#include <algorithm>
#include <string>
#include <utility>

#include <orly/rt/read_budget.h>

namespace Orly {

  namespace Rt {

  	std::string ToUpper (const std::string &str);

  	std::string ToLower (const std::string &str);

    /* `lhs + rhs` for two strings, charged to the read budget (#729) as much as the shorter of
       them: what an append grows a string by, without charging a carry copied on every step of
       a reduce its whole length each time. */
    inline std::string AddStr(const std::string &lhs, const std::string &rhs) {
      ChargeReadBudget(0UL, std::min(lhs.size(), rhs.size()));
      return lhs + rhs;
    }

    /* The same with an expiring lhs (a moved reduce carry, #697), appended to in place. Both
       sides may be the same string (`start + start`), which append handles. */
    inline std::string AddStr(std::string &&lhs, const std::string &rhs) {
      ChargeReadBudget(0UL, std::min(lhs.size(), rhs.size()));
      lhs += rhs;
      return std::move(lhs);
    }
  }
}