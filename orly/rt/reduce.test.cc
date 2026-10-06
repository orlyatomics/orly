/* <orly/rt/reduce.test.cc>

   Unit test for <orly/rt/reduce.h>.

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

#include <orly/rt/reduce.h>

#include <cstdint>
#include <vector>

#include <orly/rt/containers.h>
#include <orly/rt/generator.h>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Rt;

/* An element that counts how often it is copied. */
static size_t Copies = 0;

struct TCounted {
  TCounted(int64_t val) : Val(val) {}
  TCounted(const TCounted &that) : Val(that.Val) { ++Copies; }
  TCounted(TCounted &&) = default;
  TCounted &operator=(const TCounted &that) { Val = that.Val; ++Copies; return *this; }
  TCounted &operator=(TCounted &&) = default;
  int64_t Val;
};

static TGenerator<int64_t>::TPtr Upto(int64_t n) {
  vector<int64_t> src;
  for (int64_t i = 0; i < n; ++i) {
    src.push_back(i);
  }
  return TStlGenerator<vector<int64_t>>::New(src);
}

/* `[0..n) reduce (start empty [T]) + [that]`, in the shape orlyc emits when
   the body can move from its carry: the carry by value, moved into its one
   use. Returns the element copies it made. */
static size_t CollectCopies(int64_t n) {
  const TMovingReduceFunc<vector<TCounted>, int64_t> collect =
      [](vector<TCounted> a1/* carry */, const int64_t &a2/* that */) -> vector<TCounted> {
        return (std::move(a1) + (vector<TCounted>{a2}));
      };
  Copies = 0;
  const auto res = Reduce(Upto(n), collect, (vector<TCounted>{}));
  EXPECT_EQ(res.size(), static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    EXPECT_EQ(res[i].Val, i);
  }
  return Copies;
}

/* #697: collecting with reduce copied the whole carry on every step, N(N+3)/2
   element copies for N elements (501,500 at N = 1,000 and 32,012,000 at 8N).
   Moving the carry leaves two copies per element: out of the `[that]`
   initializer list, and from that list into the carry. */
FIXTURE(CollectIntoListIsLinear) {
  const size_t at_n = CollectCopies(1000), at_8n = CollectCopies(8000);
  EXPECT_EQ(at_n, 2000u);
  EXPECT_EQ(at_8n, 16000u);
}

/* The copying form, which orlyc still emits when `start` may run more than
   once per call, gives the same result. */
FIXTURE(CopyingReduce) {
  const TReduceFunc<vector<int64_t>, int64_t> collect =
      [](const vector<int64_t> &a1, const int64_t &a2) -> vector<int64_t> {
        return (a1 + (vector<int64_t>{a2}));
      };
  EXPECT_TRUE(Reduce(Upto(5), collect, (vector<int64_t>{})) == vector<int64_t>({0, 1, 2, 3, 4}));
}

/* The in-place operators give what the copying ones do. */
FIXTURE(MovingOperators) {
  const TMovingReduceFunc<TSet<int64_t>, int64_t> to_set =
      [](TSet<int64_t> a1, const int64_t &a2) -> TSet<int64_t> {
        return (std::move(a1) | (TSet<int64_t>{a2 % 3}));
      };
  EXPECT_TRUE(Reduce(Upto(10), to_set, (TSet<int64_t>{})) == TSet<int64_t>({0, 1, 2}));
  const TMovingReduceFunc<TDict<int64_t, int64_t>, int64_t> to_dict =
      [](TDict<int64_t, int64_t> a1, const int64_t &a2) -> TDict<int64_t, int64_t> {
        return (std::move(a1) + (TDict<int64_t, int64_t>{{a2 % 3, a2}}));
      };
  /* A later key overrides an earlier one, as with the copying `+`. */
  EXPECT_TRUE(Reduce(Upto(10), to_dict, (TDict<int64_t, int64_t>{})) ==
              (TDict<int64_t, int64_t>{{0, 9}, {1, 7}, {2, 8}}));
  vector<int64_t> list{1, 2};
  EXPECT_TRUE(std::move(list) + vector<int64_t>{3} == vector<int64_t>({1, 2, 3}));
  TSet<int64_t> set{1, 2, 3};
  EXPECT_TRUE(std::move(set) - TSet<int64_t>{2} == TSet<int64_t>({1, 3}));
  TDict<int64_t, int64_t> dict{{1, 1}, {2, 2}};
  EXPECT_TRUE(std::move(dict) - TSet<int64_t>{1} == (TDict<int64_t, int64_t>{{2, 2}}));
  /* An rhs that is the lhs itself. */
  vector<int64_t> self{1, 2};
  EXPECT_TRUE(std::move(self) + self == vector<int64_t>({1, 2, 1, 2}));
}
