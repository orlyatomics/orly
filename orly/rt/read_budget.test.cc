/* <orly/rt/read_budget.test.cc>

   Unit test for <orly/rt/read_budget.h>.

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

#include <orly/rt/read_budget.h>

#include <cstdint>
#include <string>
#include <vector>

#include <orly/rt/generator.h>
#include <orly/rt/reduce.h>
#include <orly/server/read_too_large.h>

#include <base/test/kit.h>

using namespace Orly::Rt;
using Orly::Server::TReadTooLarge;

/* The message a charge throws, or empty if it doesn't. */
static std::string ChargeMessage(size_t steps, size_t bytes) {
  try {
    ChargeReadBudget(steps, bytes);
  } catch (const TReadTooLarge &ex) {
    return ex.what();
  }
  return std::string();
}

FIXTURE(NoBudgetChargesNothing) {
  EXPECT_FALSE(GetCurrentReadBudget());
  ChargeReadBudget(static_cast<size_t>(-1), static_cast<size_t>(-1));
  CheckReadBudgetAhead(static_cast<size_t>(-1), static_cast<size_t>(-1));
}

FIXTURE(Steps) {
  TReadBudget budget(10, 0);
  TReadBudgetScope scope(&budget);
  ChargeReadBudget(10, 1000000);
  EXPECT_EQ(budget.GetSteps(), 10UL);
  EXPECT_EQ(budget.GetBytes(), 1000000UL);
  const std::string msg = ChargeMessage(1, 0);
  EXPECT_NE(msg.find("--read_budget_steps"), std::string::npos);
}

FIXTURE(Bytes) {
  TReadBudget budget(0, 100);
  TReadBudgetScope scope(&budget);
  ChargeReadBudget(1000000, 100);
  const std::string msg = ChargeMessage(0, 1);
  EXPECT_NE(msg.find("--read_budget_mb"), std::string::npos);
}

FIXTURE(CheckAheadChargesNothing) {
  TReadBudget budget(10, 100);
  TReadBudgetScope scope(&budget);
  CheckReadBudgetAhead(10, 100);
  EXPECT_EQ(budget.GetSteps(), 0UL);
  EXPECT_EQ(budget.GetBytes(), 0UL);
  ChargeReadBudget(5, 0);
  EXPECT_THROW(TReadTooLarge, [] { CheckReadBudgetAhead(6, 0); });
  /* A huge estimate must not wrap around to a small one. */
  EXPECT_THROW(TReadTooLarge, [] { CheckReadBudgetAhead(static_cast<size_t>(-1), 0); });
  EXPECT_THROW(TReadTooLarge, [] { CheckReadBudgetAhead(0, static_cast<size_t>(-1)); });
  EXPECT_EQ(budget.GetSteps(), 5UL);
}

FIXTURE(ScopesNest) {
  TReadBudget outer(0, 0), inner(0, 0);
  {
    TReadBudgetScope outer_scope(&outer);
    {
      TReadBudgetScope inner_scope(&inner);
      EXPECT_TRUE(GetCurrentReadBudget() == &inner);
      ChargeReadBudget(1, 0);
    }
    EXPECT_TRUE(GetCurrentReadBudget() == &outer);
    ChargeReadBudget(2, 0);
  }
  EXPECT_FALSE(GetCurrentReadBudget());
  EXPECT_EQ(inner.GetSteps(), 1UL);
  EXPECT_EQ(outer.GetSteps(), 2UL);
}

FIXTURE(RangeSizeHint) {
  EXPECT_EQ(TRangeGenerator::New(0, 10, false)->GetSizeHint(), 10);
  EXPECT_EQ(TRangeGenerator::New(0, 10, true)->GetSizeHint(), 11);
  EXPECT_EQ(TRangeGenerator::New(10, 0, false)->GetSizeHint(), 10);
  EXPECT_EQ(TRangeGenerator::NewWithSecond(0, 10, false, 3)->GetSizeHint(), 4);
  EXPECT_EQ(TRangeGenerator::NewWithSecond(0, 9, true, 3)->GetSizeHint(), 4);
  EXPECT_EQ(TRangeGenerator::NewWithSecond(0, 9, false, 3)->GetSizeHint(), 3);
  EXPECT_EQ(TRangeGenerator::New(0)->GetSizeHint(), TGenerator<int64_t>::InfiniteSize);
  /* INT64_MAX + 1 elements clamps to the largest hint. (A wider range overflows limit - start in
     TRangeGenerator::New itself, so isn't a case here.) */
  EXPECT_EQ(TRangeGenerator::New(0, INT64_MAX, true)->GetSizeHint(), TGenerator<int64_t>::InfiniteSize);
  EXPECT_EQ(TRangeGenerator::New(0, INT64_MAX, false)->GetSizeHint(), INT64_MAX);
  /* The hint matches what a cursor yields. */
  for (bool include : {false, true}) {
    auto gen = TRangeGenerator::NewWithSecond(-7, 23, include, -2);
    int64_t count = 0;
    for (auto it = MakeCursor(gen); it; ++it) {
      ++count;
    }
    EXPECT_EQ(gen->GetSizeHint(), count);
  }
}

FIXTURE(RangeChargesSteps) {
  TReadBudget budget(100, 0);
  TReadBudgetScope scope(&budget);
  int64_t sum = 0;
  for (auto it = MakeCursor(TRangeGenerator::New(0, 50, false)); it; ++it) {
    sum += *it;
  }
  EXPECT_EQ(sum, 1225);
  EXPECT_EQ(budget.GetSteps(), 50UL);
  EXPECT_THROW(TReadTooLarge, [] {
    for (auto it = MakeCursor(TRangeGenerator::New(0)); it; ++it) {}
  });
}

FIXTURE(ReduceRefusedUpFront) {
  TReadBudget budget(100, 0);
  TReadBudgetScope scope(&budget);
  const TReduceFunc<int64_t, int64_t> add = [](const int64_t &carry, const int64_t &that) { return carry + that; };
  bool threw = false;
  try {
    Reduce<int64_t, int64_t>(TRangeGenerator::New(0, 1000, false), add, 0);
  } catch (const TReadTooLarge &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
  /* Refused before any element was walked. */
  EXPECT_EQ(budget.GetSteps(), 0UL);
  EXPECT_EQ((Reduce<int64_t, int64_t>(TRangeGenerator::New(0, 10, false), add, 0)), 45);
}

FIXTURE(VectorGrowth) {
  TReadBudget budget(0, 0);
  TReadBudgetScope scope(&budget);
  std::vector<int64_t> vec;
  for (int i = 0; i < 1000; ++i) {
    ChargeVectorGrowth(vec, 1UL);
    vec.push_back(i);
  }
  /* Charged what was allocated, and no more. */
  EXPECT_EQ(budget.GetBytes(), vec.capacity() * sizeof(int64_t));
}
