/* <orly/time.h>

   Two tiny typedefs over `std::chrono`:
   `TTtl = std::chrono::seconds` for time-to-live values, and
   `TDeadline = std::chrono::time_point<system_clock>` for absolute
   deadlines. Used wherever orly schedules a delayed action.

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
#include <chrono>

namespace Orly {

  /* We characterize time-to-live in seconds. */
  using TTtl = std::chrono::seconds;

  /* We characterize a deadline as a point in time on the system clock. */
  using TDeadline = std::chrono::time_point<std::chrono::system_clock>;

  /* Returns the deadline `ttl` after `now`.  The clock counts nanoseconds, so converting a large `ttl` (the
     maximum, which means "never expires") overflows; a `ttl` that would run past the latest representable
     deadline gives that deadline instead.  `ttl` must not be negative. */
  inline TDeadline AddTtl(const TDeadline &now, const TTtl &ttl) {
    assert(ttl.count() >= 0);
    if (ttl >= std::chrono::duration_cast<TTtl>(TDeadline::max() - now)) {
      return TDeadline::max();
    }
    return now + ttl;
  }

}  // Orly

