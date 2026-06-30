/* <server/half_latch.h>

   A base class for creating active objects which react to events.

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

#include <base/class_traits.h>
#include <base/fd.h>

namespace Server {

  class THalfLatch {
    NO_COPY(THalfLatch);
    public:

    THalfLatch();

    int GetWaitHandle() const {
      return RecvFd;
    }

    void Recv();

    void Send();

    private:

    bool IsSet;

    Base::TFd RecvFd, SendFd;

  };  // THalfLatch

}  // Server
