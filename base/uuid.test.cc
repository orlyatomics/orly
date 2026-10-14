/* <base/uuid.test.cc>

   Unit test for <base/uuid.h>.

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

#include <base/uuid.h>

#include <set>
#include <sstream>
#include <string>
#include <unordered_set>

#include <base/io/binary_input_only_stream.h>
#include <base/io/binary_output_only_stream.h>
#include <base/io/recorder_and_player.h>
#include <base/test/kit.h>

using namespace std;
using namespace Base;
using namespace Io;

static const char
    *Lower = "1b4e28ba-2fa1-11d2-883f-b9a761bde3fb",
    *Upper = "1B4E28BA-2FA1-11D2-883F-B9A761BDE3FB";

FIXTURE(FormatAndParse) {
  static const size_t case_count = 2;
  static const pair<string, TUuid::TCasing> case_array[2] = {
      { Lower, TUuid::Lower },
      { Upper, TUuid::Upper }
  };
  char actual_str[TUuid::MinBufSize];
  for (size_t i = 0; i < case_count; ++i) {
    TUuid(case_array[i].first.c_str()).Format(actual_str, case_array[i].second);
    EXPECT_EQ(string(actual_str), case_array[i].first);
  }
}

FIXTURE(NullAndNotNull) {
  TUuid null, not_null(Lower);
  EXPECT_FALSE(null);
  EXPECT_TRUE(not_null);
}

FIXTURE(Uniqueness) {
  static const size_t algo_count = 3;
  static const TUuid::TAlgo algos[algo_count] = { TUuid::Best, TUuid::Random, TUuid::TimeAndMAC };
  static const size_t expected_set_size = 100;
  for (size_t algo_idx = 0; algo_idx < algo_count; ++algo_idx) {
    unordered_set<TUuid> uuid_set;
    for (size_t i = 0; i < expected_set_size; ++i) {
      uuid_set.insert(algos[algo_idx]);
    }
    EXPECT_EQ(uuid_set.size(), expected_set_size);
  }
}

FIXTURE(StdStrm) {
  const TUuid expected(Lower);
  string blob;
  /* extra */ {
    ostringstream strm;
    strm << expected;
    blob = strm.str();
  }
  TUuid actual;
  EXPECT_FALSE(actual);
  /* extra */ {
    istringstream strm(blob);
    strm >> actual;
  }
  EXPECT_EQ(actual, expected);
}

FIXTURE(BinaryStrm) {
  const TUuid expected(Lower);
  auto recorder = make_shared<TRecorder>();
  /* extra */ {
    TBinaryOutputOnlyStream strm(recorder);
    strm << expected;
  }
  TUuid actual;
  EXPECT_FALSE(actual);
  /* extra */ {
    TBinaryInputOnlyStream strm(make_shared<TPlayer>(recorder));
    strm >> actual;
  }
  EXPECT_EQ(actual, expected);
}

FIXTURE(IsValidUuid) {
  EXPECT_TRUE(TUuid::IsValidUuid(Lower));
  EXPECT_TRUE(TUuid::IsValidUuid(Upper));
  EXPECT_FALSE(TUuid::IsValidUuid(""));
  EXPECT_FALSE(TUuid::IsValidUuid("1b4e28ba-2fa1-11d2-883f"));  // too short
  /* 36 characters, but not a uuid -- the old length-only check passed this. */
  EXPECT_FALSE(TUuid::IsValidUuid("this-is-36-characters-of-not-a-uuid!"));
  /* Right shape, non-hex digit. */
  EXPECT_FALSE(TUuid::IsValidUuid("1b4e28ba-2fa1-11d2-883f-b9a761bdeZfb"));
}

FIXTURE(TimeAndMACSafe) {
  /* The safe generator either produces a uuid or throws TUnsafeError when
     the system can't guarantee uniqueness (e.g. no uuidd on a CI box);
     both are valid outcomes, anything else is a bug. */
  try {
    TUuid id(TUuid::TimeAndMACSafe);
    EXPECT_TRUE(static_cast<bool>(id));
  } catch (const TUuid::TUnsafeError &) {
    // Acceptable: this host can't make the guarantee.
  }
}

FIXTURE(ComparisonAndPairOrdering) {
  const TUuid u1("1b4e28ba-2fa1-11d2-883f-b9a761bde3fb");
  const TUuid u2("2b4e28ba-2fa1-11d2-883f-b9a761bde3fb");
  const TUuid r1("3b4e28ba-2fa1-11d2-883f-b9a761bde3fb");
  const TUuid r2("4b4e28ba-2fa1-11d2-883f-b9a761bde3fb");

  EXPECT_TRUE((u1 <=> u2) < 0);
  EXPECT_TRUE((u2 <=> u1) > 0);
  EXPECT_TRUE((u1 <=> u1) == 0);

  auto p1 = make_pair(u1, r1);
  auto p2 = make_pair(u2, r1);
  auto p3 = make_pair(u1, r2);

  EXPECT_TRUE(p1 < p2);
  EXPECT_FALSE(p2 < p1);
  EXPECT_TRUE(p1 < p3);
  EXPECT_FALSE(p3 < p1);

  set<pair<TUuid, TUuid>> s;
  s.insert(p1);
  EXPECT_EQ(s.size(), 1UL);
  EXPECT_TRUE(s.find(p1) != s.end());
  EXPECT_TRUE(s.find(p2) == s.end());
  EXPECT_TRUE(s.find(p3) == s.end());
}


FIXTURE(FormatMatchesUnparse) {
  /* Format writes the text itself; it must be uuid_unparse*'s text, byte for byte, in each casing. */
  for (int i = 0; i < 1000; ++i) {
    TUuid uuid(i % 2 ? TUuid::Random : TUuid::TimeAndMAC);
    if (i == 0) {
      uuid = TUuid();
    }
    char actual[TUuid::MinBufSize], expected[TUuid::MinBufSize];
    uuid.Format(actual);
    uuid_unparse(uuid.GetRaw(), expected);
    EXPECT_EQ(string(actual), string(expected));
    uuid.Format(actual, TUuid::Upper);
    uuid_unparse_upper(uuid.GetRaw(), expected);
    EXPECT_EQ(string(actual), string(expected));
    uuid.Format(actual, TUuid::Lower);
    uuid_unparse_lower(uuid.GetRaw(), expected);
    EXPECT_EQ(string(actual), string(expected));
    EXPECT_TRUE(TUuid(uuid) == uuid);
  }
}
