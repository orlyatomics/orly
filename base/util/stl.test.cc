/* <util/stl.test.cc>

   Unit test for <util/stl.h>.

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

#include <base/util/stl.h>

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Util;

FIXTURE(Contains) {
  unordered_set<int> container;
  container.insert(101);
  EXPECT_TRUE(Contains(container, 101));
  EXPECT_FALSE(Contains(container, 202));
}

FIXTURE(FindOrDefault) {
  unordered_map<int, double> container;
  const int key = 101;
  const double expected = 98.6;
  container[key] = expected;
  EXPECT_EQ(FindOrDefault(container, key, expected + 1), expected);
  EXPECT_EQ(FindOrDefault(container, key + 1, expected + 1), expected + 1);
}

FIXTURE(FindOrInsert) {
  unordered_map<int, double> container;
  const int key = 101;
  const double expected = 98.6;
  EXPECT_EQ(FindOrInsert(container, key, expected), expected);
  EXPECT_EQ(FindOrInsert(container, key, expected + 1), expected);
}

FIXTURE(RotatedLeft) {
  EXPECT_EQ(RotatedLeft<unsigned short>(0x1234, 4), 0x2341);
}

/* The rotation as the old code computed it for every count it handled on x86, where the hardware masks the shift
   count: a count that is a multiple of the width ORed the value with itself.  Hashes are persisted, so the
   replacement must agree with this bit for bit. */
template <typename TVal>
static TVal OldRotatedLeft(TVal val, int n) {
  const unsigned digits = std::numeric_limits<TVal>::digits;
  n %= digits;
  const unsigned mask = digits - 1;
  return static_cast<TVal>((val << (n & mask)) | (val >> ((digits - n) & mask)));
}

template <typename TVal>
static void CheckRotations(TVal val) {
  const int digits = std::numeric_limits<TVal>::digits;
  for (int n = 0; n <= 3 * digits + 5; ++n) {
    EXPECT_EQ(RotatedLeft(val, n), OldRotatedLeft(val, n));
    EXPECT_EQ(RotatedRight(val, n), OldRotatedLeft(val, digits - n % digits));
    EXPECT_EQ(RotatedRight(RotatedLeft(val, n), n), val);
  }
}

FIXTURE(RotationByMultipleOfWidth) {
  /* A count that is zero or a multiple of the width must return the value unchanged. */
  for (int n : {0, 16, 32}) {
    EXPECT_EQ(RotatedLeft<unsigned short>(0x1234, n), 0x1234);
    EXPECT_EQ(RotatedRight<unsigned short>(0x1234, n), 0x1234);
  }
  for (int n : {0, 64, 128}) {
    EXPECT_EQ(RotatedLeft<uint64_t>(0x0123456789abcdefULL, n), 0x0123456789abcdefULL);
    EXPECT_EQ(RotatedRight<uint64_t>(0x0123456789abcdefULL, n), 0x0123456789abcdefULL);
  }
  EXPECT_EQ(RotatedLeft<uint64_t>(0x0123456789abcdefULL, 5), 0x2468acf13579bde0ULL);
  EXPECT_EQ(RotatedRight<uint64_t>(0x0123456789abcdefULL, 5), 0x78091a2b3c4d5e6fULL);
}

FIXTURE(RotationMatchesOldResults) {
  CheckRotations<unsigned short>(0x1234);
  CheckRotations<uint32_t>(0x89abcdefU);
  CheckRotations<uint64_t>(0x0123456789abcdefULL);
  CheckRotations<uint64_t>(~0ULL);
}

FIXTURE(RotatedRight) {
  EXPECT_EQ(RotatedRight<unsigned short>(0x1234, 4), 0x4123);
}

FIXTURE(TryFind) {
  unordered_map<int, double> container;
  const int key = 101;
  const double expected = 98.6;
  container[key] = expected;
  const double *result = TryFind(container, key);
  if (EXPECT_TRUE(result)) {
    EXPECT_EQ(*result, expected);
  }
  EXPECT_FALSE(TryFind(container, key + 1));
}

FIXTURE(EqEq) {
  unordered_set<int> lhs, rhs;
  EXPECT_TRUE(eqeq(lhs,rhs));
  lhs.insert(101);
  EXPECT_FALSE(eqeq(lhs,rhs));
  EXPECT_FALSE(eqeq(rhs,lhs));
  rhs.insert(101);
  EXPECT_TRUE(eqeq(lhs,rhs));
  lhs.insert(202);
  EXPECT_FALSE(eqeq(lhs,rhs));
  rhs.insert(303);
  EXPECT_FALSE(eqeq(lhs,rhs));
  lhs.insert(303);
  EXPECT_FALSE(eqeq(lhs,rhs));
  rhs.insert(202);
  EXPECT_TRUE(eqeq(lhs,rhs));
}

FIXTURE(EqEqMap) {
  unordered_map<int,double> lhs, rhs;
  EXPECT_TRUE(eqeq_map(lhs,rhs));
  lhs[101]=98.7;
  EXPECT_FALSE(eqeq_map(lhs,rhs));
  EXPECT_FALSE(eqeq_map(rhs,lhs));
  rhs[101]=98.7;
  EXPECT_TRUE(eqeq_map(lhs,rhs));
  lhs[202]=1.23;
  EXPECT_FALSE(eqeq_map(lhs,rhs));
  rhs[303]=3.14;
  EXPECT_FALSE(eqeq_map(lhs,rhs));
  lhs[303]=3.14;
  EXPECT_FALSE(eqeq_map(lhs,rhs));
  rhs[202]=1.23;
  EXPECT_TRUE(eqeq_map(lhs,rhs));
}

FIXTURE(ForIter) {
  EXPECT_TRUE((is_same<int, ForIter<vector<int>::iterator>::TVal>::value));
  EXPECT_TRUE((is_same<int, ForIter<vector<int>::const_iterator>::TVal>::value));
  EXPECT_TRUE((is_same<string, ForIter<vector<string>::iterator>::TVal>::value));
  EXPECT_TRUE((is_same<string, ForIter<vector<string>::const_iterator>::TVal>::value));
  EXPECT_FALSE((is_same<int, ForIter<vector<string>::iterator>::TVal>::value));
  EXPECT_FALSE((is_same<string, ForIter<vector<int>::iterator>::TVal>::value));
  EXPECT_TRUE((is_same<int, ForIter<int *>::TVal>::value));
}
