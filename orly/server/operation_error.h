/* <orly/server/operation_error.h>

   The refusals an operation id (#733) can earn. None applied the write; over WebSocket each is a
   status of its own ("operation_expired", "operation_id_reused", "operation_in_flight",
   "outcome_too_large"), and over the binary protocol an error whose message starts with the
   status's words.

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
#include <stdexcept>
#include <string>

namespace Orly {

  namespace Server {

    /* The id is older than the server keeps outcomes, so it can't say whether the write ran.
       Not retryable with the same id: read the state, or send the write under a new id. */
    class TOperationExpired
        : public std::runtime_error {
      public:

      explicit TOperationExpired(const std::string &msg)
          : std::runtime_error(msg) {}

    };  // TOperationExpired

    /* The id already names a different request. A client bug: ids are one per request. */
    class TOperationIdReused
        : public std::runtime_error {
      public:

      explicit TOperationIdReused(const std::string &msg)
          : std::runtime_error(msg) {}

    };  // TOperationIdReused

    /* The request is still running under this id. Retryable: try again after a short wait. */
    class TOperationInFlight
        : public std::runtime_error {
      public:

      explicit TOperationInFlight(const std::string &msg)
          : std::runtime_error(msg) {}

    };  // TOperationInFlight

    /* The write ran exactly once, but its result was over the cap and wasn't kept. Version is the
       sequence number it committed at, so the client can read its effect as of then. */
    class TOutcomeTooLarge
        : public std::runtime_error {
      public:

      TOutcomeTooLarge(const std::string &msg, uint64_t version)
          : std::runtime_error(msg), Version(version) {}

      uint64_t GetVersion() const {
        return Version;
      }

      private:

      uint64_t Version;

    };  // TOutcomeTooLarge

  }  // Server

}  // Orly
