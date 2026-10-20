/* <orly/time.test.cc>

   Unit test for <orly/time.h>.

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

#include <orly/time.h>

#include <base/test/kit.h>

using namespace std::chrono;
using namespace Orly;

FIXTURE(AddTtlOrdinary) {
  const TDeadline now = system_clock::now();
  EXPECT_TRUE(AddTtl(now, TTtl(0)) == now);
  EXPECT_TRUE(AddTtl(now, TTtl(3600)) == now + hours(1));
}

/* TTtl::max() means "never expires"; adding it to now() used to overflow the nanosecond clock. */
FIXTURE(AddTtlSaturates) {
  const TDeadline now = system_clock::now();
  EXPECT_TRUE(AddTtl(now, TTtl::max()) == TDeadline::max());
  EXPECT_TRUE(AddTtl(now, TTtl::max()) > now);
  /* A century is representable; a thousand years is not. */
  EXPECT_TRUE(AddTtl(now, hours(24 * 365 * 100)) == now + hours(24 * 365 * 100));
  EXPECT_TRUE(AddTtl(now, hours(24 * 365 * 1000)) == TDeadline::max());
}
