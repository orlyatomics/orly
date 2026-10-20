/* <base/unaligned.test.cc>

   Unit test for <base/unaligned.h>.

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

#include <base/unaligned.h>

#include <cstdint>
#include <cstring>

#include <base/test/kit.h>

using namespace Base;

/* Read each width at every offset of a buffer, so the address takes every alignment. */
template <typename TVal>
static void CheckEveryOffset(TVal val) {
  alignas(16) uint8_t buf[sizeof(TVal) + 16];
  for (size_t offset = 0; offset < 16; ++offset) {
    memset(buf, 0xa5, sizeof(buf));
    memcpy(buf + offset, &val, sizeof(TVal));
    EXPECT_TRUE(ReadUnaligned<TVal>(buf + offset) == val);
  }
}

FIXTURE(EveryWidthAtEveryAlignment) {
  CheckEveryOffset<int8_t>(-101);
  CheckEveryOffset<uint16_t>(54321);
  CheckEveryOffset<int32_t>(-0x01020304);
  CheckEveryOffset<uint64_t>(0x0123456789abcdefULL);
  CheckEveryOffset<double>(-2.25);
  CheckEveryOffset<float>(1.5f);
}
