/* <orly/server/rfc3339.test.cc>

   Unit test for <orly/server/rfc3339.h>.

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

#include <orly/server/rfc3339.h>

#include <base/test/kit.h>

using namespace std;
using namespace chrono;
using namespace Orly::Server;

static int64_t Micros(const string &text) {
  const auto when = ParseRfc3339(text);
  return when ? duration_cast<microseconds>(when->time_since_epoch()).count() : -1;
}

FIXTURE(Zones) {
  EXPECT_EQ(Micros("1970-01-01T00:00:00Z"), 0);
  EXPECT_EQ(Micros("1970-01-01t00:00:00z"), 0);
  EXPECT_EQ(Micros("1970-01-01T01:00:00+01:00"), 0);
  EXPECT_EQ(Micros("1969-12-31T19:30:00-04:30"), 0);
  EXPECT_EQ(Micros("2026-05-17T09:30:00Z"), 1779010200LL * 1000000);
}

FIXTURE(Fractions) {
  EXPECT_EQ(Micros("1970-01-01T00:00:01.5Z"), 1500000);
  EXPECT_EQ(Micros("1970-01-01T00:00:01.000001Z"), 1000001);
  /* Finer than a microsecond is truncated. */
  EXPECT_EQ(Micros("1970-01-01T00:00:01.0000019Z"), 1000001);
}

FIXTURE(Refused) {
  for (const char *text : {"", "1970-01-01", "1970-01-01 00:00:00Z", "1970-01-01T00:00:00", "1970-01-01T00:00:00.Z",
                           "1970-01-01T00:00:00Zx", "1970-02-30T00:00:00Z", "1970-13-01T00:00:00Z",
                           "1970-01-01T24:00:00Z", "1970-01-01T00:60:00Z", "1970-01-01T00:00:00+24:00",
                           "1970-01-01T00:00:00+0100", "70-01-01T00:00:00Z", "1970-01-01T00:00:00 Z"}) {
    EXPECT_FALSE(ParseRfc3339(text).has_value());
  }
}
