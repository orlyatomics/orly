/* <orly/rt/sort.test.cc>

   Unit test for <orly/rt/sort.h>

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

#include <orly/rt/sort.h>

#include <functional>

#include <orly/rt/operator.h>
#include <orly/rt/opt.h>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Rt;

/* Empty */
vector<int64_t> empty_li;

/* Non-empties */
vector<int64_t> li({2, 1, 3});

/* Unknowns */
TOpt<vector<int64_t>> unknown_li;

/* Opt empties */
TOpt<vector<int64_t>> opt_empty_li(empty_li);

/* Opt Non-empties */
TOpt<vector<int64_t>> opt_li(li);

/* Sorted */
vector<int64_t> sorted_li({1, 2, 3});

static const std::function<bool (const int64_t &, const int64_t &)> &lt = LtStruct<int64_t, int64_t>::Do;

FIXTURE(SortOnEmpty) {
  EXPECT_TRUE(Sort(empty_li, lt) == empty_li);
}

FIXTURE(SortOnNonEmpty) {
  EXPECT_TRUE(Sort(li, lt) == sorted_li);
}

FIXTURE(SortOnUnknowns) {
  EXPECT_TRUE(Sort(unknown_li, lt).IsUnknown());
}

FIXTURE(SortOnOptEmpty) {
  EXPECT_TRUE(Sort(opt_empty_li, lt).GetVal() == empty_li);
}

FIXTURE(SortOnOptNonEmpty) {
  EXPECT_TRUE(Sort(opt_li, lt).GetVal() == sorted_li);
}

/* Ties keep their input order (#698). Each element is <[key, tag]>, compared
   by key alone; the tags record the input order. */
using tie_t = std::pair<int64_t, int64_t>;

static vector<tie_t> MakeTies(size_t n) {
  vector<tie_t> ret;
  for (size_t i = 0; i < n; ++i) {
    /* Keys cycle 2, 1, 0, so every key repeats; tags are the input positions. */
    ret.emplace_back(static_cast<int64_t>(2 - i % 3), static_cast<int64_t>(i));
  }
  return ret;
}

static bool IsStableByKey(const vector<tie_t> &sorted, size_t n) {
  if (sorted.size() != n) {
    return false;
  }
  for (size_t i = 1; i < sorted.size(); ++i) {
    const auto &a = sorted[i - 1], &b = sorted[i];
    if (a.first > b.first || (a.first == b.first && a.second > b.second)) {
      return false;
    }
  }
  return true;
}

FIXTURE(SortTiesKeepInputOrder) {
  const std::function<bool (const tie_t &, const tie_t &)> by_key =
      [](const tie_t &lhs, const tie_t &rhs) { return lhs.first < rhs.first; };
  /* 100 is past libstdc++'s insertion-sort threshold, so the partitioning
     path that left ties unordered is exercised too. */
  for (size_t n : {3UL, 10UL, 100UL, 1000UL}) {
    EXPECT_TRUE(IsStableByKey(Sort(MakeTies(n), by_key), n));
  }
}

/* A non-strict comparator (`lhs <= rhs`) still sorts, stays in bounds, and
   gives the same defined order as its strict counterpart (#698). Under
   `std::sort`, `<=` over many equal keys read past the end of the vector. */
FIXTURE(SortNonStrictComparator) {
  const std::function<bool (const tie_t &, const tie_t &)> le_key =
      [](const tie_t &lhs, const tie_t &rhs) { return lhs.first <= rhs.first; };
  const std::function<bool (const tie_t &, const tie_t &)> lt_key =
      [](const tie_t &lhs, const tie_t &rhs) { return lhs.first < rhs.first; };
  for (size_t n : {3UL, 10UL, 100UL, 1000UL}) {
    const auto ties = MakeTies(n);
    const auto by_le = Sort(ties, le_key);
    EXPECT_TRUE(IsStableByKey(by_le, n));
    EXPECT_TRUE(by_le == Sort(ties, lt_key));
  }
  /* All keys equal: the case that overran under std::sort. */
  vector<tie_t> same;
  for (int64_t i = 0; i < 1000; ++i) {
    same.emplace_back(7, i);
  }
  EXPECT_TRUE(Sort(same, le_key) == same);
  /* A comparator that is always true (lang_test's funccall uses one) keeps
     the input as it is. */
  const std::function<bool (const tie_t &, const tie_t &)> always =
      [](const tie_t &, const tie_t &) { return true; };
  const auto ties = MakeTies(100);
  EXPECT_TRUE(Sort(ties, always) == ties);
}
