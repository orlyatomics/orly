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

/* The nanosecond system clock cannot name a time past 2262 or before 1677; those clamp instead of overflowing, so a
   client that sends a far year gets "later than everything" rather than a wrapped time. */
FIXTURE(OutOfClockRange) {
  const auto latest = ParseRfc3339("2262-04-11T23:47:16Z");
  const auto too_late = ParseRfc3339("2262-04-11T23:47:17Z");
  const auto far_future = ParseRfc3339("9999-12-31T23:59:59Z");
  if (EXPECT_TRUE(latest && too_late && far_future)) {
    EXPECT_EQ(duration_cast<seconds>(latest->time_since_epoch()).count(), 9223372036LL);
    EXPECT_TRUE(*too_late == system_clock::time_point::max());
    EXPECT_TRUE(*far_future == system_clock::time_point::max());
  }
  const auto earliest = ParseRfc3339("1677-09-21T00:12:44Z");
  const auto too_early = ParseRfc3339("1677-09-21T00:12:43Z");
  const auto year_zero = ParseRfc3339("0000-01-01T00:00:00Z");
  if (EXPECT_TRUE(earliest && too_early && year_zero)) {
    EXPECT_EQ(duration_cast<seconds>(earliest->time_since_epoch()).count(), -9223372036LL);
    EXPECT_TRUE(*too_early == system_clock::time_point::min());
    EXPECT_TRUE(*year_zero == system_clock::time_point::min());
  }
  EXPECT_TRUE(*far_future > *latest);
}
