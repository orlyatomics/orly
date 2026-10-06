/* <orly/server/write_too_large.h>

   The error a write gets when its update holds more entries than half the Update Entry pool's
   merge reserve (#607, #687). Unlike TWriteTooLarge it is not retryable: such a write could
   never be promoted, so the client must split it into smaller batches. Over WebSocket it is
   reported as `"status": "write_too_large"`; over the binary protocol, as an error whose message
   starts with "write too large".

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

    class TWriteTooLarge
        : public std::runtime_error {
      public:

      explicit TWriteTooLarge(const std::string &msg)
          : std::runtime_error(msg) {}

    };  // TWriteTooLarge

  }  // Server

}  // Orly
