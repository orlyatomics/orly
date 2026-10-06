/* <orly/server/insufficient_memory.h>

   The error a write gets when the server refuses it because the update pools are down to the
   reserve kept for merges (#607). Reads keep working. Over WebSocket it is reported as
   `"status": "insufficient_memory"`; over the binary protocol, as an error whose message starts
   with "insufficient memory".

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

#include <stdexcept>
#include <string>

namespace Orly {

  namespace Server {

    class TInsufficientMemory
        : public std::runtime_error {
      public:

      explicit TInsufficientMemory(const std::string &msg)
          : std::runtime_error(msg) {}

    };  // TInsufficientMemory

  }  // Server

}  // Orly
