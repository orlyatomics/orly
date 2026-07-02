/* <orly/rt/slice.test.cc>

   Unit test for <orly/rt/slice.h>.

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

#include <orly/rt/slice.h>

#include <string>
#include <tuple>
#include <vector>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Rt;

using TIntVec = vector<int64_t>;
using TKeyAddr = tuple<int64_t>;
using TMutVec = TMutable<TKeyAddr, TIntVec>;

static const TIntVec Vec{10, 20, 30, 40, 50};

FIXTURE(Single) {
  EXPECT_EQ(SliceSingle(Vec, 0L), 10);
  EXPECT_EQ(SliceSingle(Vec, 4L), 50);
  EXPECT_EQ(SliceSingle(string("abc"), 1L), "b");
  EXPECT_THROW(TSystemError, []() { SliceSingle(Vec, 5L); });
  EXPECT_THROW(TSystemError, []() { SliceSingle(Vec, -1L); });
}

FIXTURE(Range) {
  EXPECT_TRUE((SliceRange(Vec, true, 2L) == TIntVec{10, 20}));
  EXPECT_TRUE((SliceRange(Vec, false, 2L) == TIntVec{30, 40, 50}));
  EXPECT_EQ(SliceRange(string("hello"), true, 2L), "he");
  EXPECT_THROW(TSystemError, []() { SliceRange(Vec, true, 6L); });
}

FIXTURE(RangeBoth) {
  EXPECT_TRUE((SliceRangeBoth(Vec, 1L, 3L) == TIntVec{20, 30}));
  EXPECT_EQ(SliceRangeBoth(string("hello"), 1L, 3L), "el");
  /* Reverse slices are rejected, not silently emptied. */
  EXPECT_THROW(TSystemError, []() { SliceRangeBoth(Vec, 3L, 1L); });
}

/* The mutable overloads (#361): the sliced value keeps the mutable's
   identity -- same address, same parts (a range is not an addressable
   sub-part, unlike SliceSingle's per-element part). */
FIXTURE(MutableRange) {
  const TMutVec m(make_tuple(7L), Vec);
  const auto head = SliceRange(m, true, 2L);
  EXPECT_TRUE((head.GetVal() == TIntVec{10, 20}));
  EXPECT_TRUE(head.GetAddr() == make_tuple(7L));
  EXPECT_TRUE(head.GetParts().empty());
  const auto tail = SliceRange(m, false, 3L);
  EXPECT_TRUE((tail.GetVal() == TIntVec{40, 50}));
}

FIXTURE(MutableRangeBoth) {
  const TMutVec m(make_tuple(7L), Vec);
  const auto mid = SliceRangeBoth(m, 1L, 4L);
  EXPECT_TRUE((mid.GetVal() == TIntVec{20, 30, 40}));
  EXPECT_TRUE(mid.GetAddr() == make_tuple(7L));
  EXPECT_TRUE(mid.GetParts().empty());
}
