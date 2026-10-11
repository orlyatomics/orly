/* <orly/server/version_unavailable.h>

   The errors a read at a version gets (#745). TVersionUnavailable: the version asked for is older than the oldest the
   server can still answer exactly (merges have pruned or folded the history below it), or newer than the newest it has
   committed. Over WebSocket it is reported as `"status": "version_unavailable"`, with the version asked for and the
   oldest and newest there are; over the binary protocol, as an error whose message starts with "version unavailable".
   An older version will not come back, so the client must read a newer one; a newer one may, once it commits.
   TVersionReadOnly: a call read at a version tried to write, which it cannot, since it reads a past state; reported as
   `"status": "version_read_only"`, or an error whose message starts with "version read only".

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

    class TVersionUnavailable
        : public std::runtime_error {
      public:

      /* `oldest` and `newest` bound the versions that can be read now; when nothing is committed, newest is 0 and
         oldest is 1. */
      TVersionUnavailable(uint64_t requested, uint64_t oldest, uint64_t newest)
          : std::runtime_error(Describe(requested, oldest, newest)), Requested(requested), Oldest(oldest), Newest(newest) {}

      uint64_t GetRequested() const noexcept {
        return Requested;
      }

      uint64_t GetOldest() const noexcept {
        return Oldest;
      }

      uint64_t GetNewest() const noexcept {
        return Newest;
      }

      private:

      static std::string Describe(uint64_t requested, uint64_t oldest, uint64_t newest) {
        if (requested > newest) {
          return "version unavailable: version " + std::to_string(requested) + " is not committed yet (the newest is " +
                 std::to_string(newest) + ")";
        }
        return "version unavailable: version " + std::to_string(requested) + " is older than the oldest that can be read (" +
               std::to_string(oldest) + "); merges have dropped the history below it";
      }

      uint64_t Requested, Oldest, Newest;

    };  // TVersionUnavailable

    class TVersionReadOnly
        : public std::runtime_error {
      public:

      TVersionReadOnly()
          : std::runtime_error("version read only: a call that reads at a version cannot write") {}

    };  // TVersionReadOnly

  }  // Server

}  // Orly
