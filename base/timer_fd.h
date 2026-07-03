/* <base/timer_fd.h>

   An wrapper around the Linux timerfd functions.

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
#include <cstdint>

#include <sys/timerfd.h>

#include <base/class_traits.h>
#include <base/fd.h>

namespace Base {

  class TTimerFd {
    NO_COPY(TTimerFd);
    public:

    TTimerFd(std::chrono::milliseconds milliseconds);

    const TFd &GetFd() const {
      return Fd;
    }

    uint64_t Pop();

    /* Force the next Pop() to return immediately by re-arming the timer as
       a one-shot that fires at once.  This replaces the periodic schedule --
       the timer will not tick again -- so it is only for waking a loop that
       is about to exit (#440). */
    void FireNow();

    private:

    TFd Fd;

  };  // TTimerFd

}  // Base
