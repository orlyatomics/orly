/* <base/event_semaphore.h>

   `TEventSemaphore` wraps Linux `eventfd` as a counted semaphore.
   `Push(n)` increments the count, `Pop()` decrements by 1; the
   blocking / non-blocking behaviour of `Pop` is selected at
   construction. `GetFd()` returns the underlying file descriptor
   for integration with `epoll` and friends.

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
#include <cassert>

#include <base/class_traits.h>
#include <base/fd.h>

namespace Base {

  class TEventSemaphore {
    NO_COPY(TEventSemaphore);
    public:

    TEventSemaphore(uint64_t initial_count = 0, bool nonblocking = false);

    const TFd &GetFd() const {
      return Fd;
    }

    /* If the nonblocking option was passed to the constructor, this returns
       true if the pop was successful, or false if the pop failed because the
       semaphore had a count of 0 immediately before the call.  It the
       nonblocking option was not passed to the constructor, this always
       returns true. */
    bool Pop();

    void Push(uint64_t count = 1);

    private:

    TFd Fd;

  };  // TEventSemaphore

  /* RAII: pushes the semaphore once on scope exit, including exceptional
     exit.  Lets a service loop signal 'I have returned' to a joiner no
     matter how the loop unwinds (#440). */
  class TPushOnExit {
    NO_COPY(TPushOnExit);
    public:

    explicit TPushOnExit(TEventSemaphore &sem) : Sem(sem) {}

    ~TPushOnExit() {
      Sem.Push();
    }

    private:

    TEventSemaphore &Sem;

  };  // TPushOnExit

}  // Base
