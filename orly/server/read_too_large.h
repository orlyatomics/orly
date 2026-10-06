/* <orly/server/read_too_large.h>

   The error a method call gets when it walks more rows, or builds more result memory, than the
   per-read budget allows (--read_budget_rows, --read_budget_mb; #694). The read-side twin of
   TWriteTooLarge (#687), and likewise not retryable: the same call would pass the same budget
   again, so the client must read a narrower range. Over WebSocket it is reported as
   `"status": "read_too_large"`; over the binary protocol, as an error whose message starts with
   "read too large".

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

    class TReadTooLarge
        : public std::runtime_error {
      public:

      explicit TReadTooLarge(const std::string &msg)
          : std::runtime_error(msg) {}

    };  // TReadTooLarge

  }  // Server

}  // Orly
