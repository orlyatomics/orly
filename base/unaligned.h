/* <base/unaligned.h>

   Read a value of a given type from an address that need not be aligned for it.

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

#include <bit>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace Base {

  /* Return the TVal whose bytes are stored at addr.  The address may have any alignment.
     Binding a TVal reference or dereferencing a TVal pointer at a misaligned address is undefined behavior, even on a machine
     that tolerates the load; a copy of sizeof(TVal) bytes is not, and the compiler turns a fixed-size copy into the one
     load or store the machine has for that width, so this costs nothing over the typed load. */
  template <typename TVal>
  inline TVal ReadUnaligned(const void *addr) {
    static_assert(std::is_trivially_copyable_v<TVal>, "ReadUnaligned needs a trivially copyable type.");
    alignas(TVal) std::byte bytes[sizeof(TVal)];
    std::memcpy(bytes, addr, sizeof(TVal));
    return std::bit_cast<TVal>(bytes);
  }

}  // Base
