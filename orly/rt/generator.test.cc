/* <orly/rt/generator.test.cc>

   Unit test for <orly/rt/
   Generator base classes.

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

#include <orly/rt/generator.h>

#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

#include <orly/rt/opt.h>
#include <orly/rt/runtime_error.h>

#include <base/test/kit.h>

using namespace Orly::Rt;

FIXTURE(Range) {
  int expected = 14;
  for(auto it = MakeCursor(TRangeGenerator::NewWithSecond(14, 42, true, 15)); it; ++it) {
    EXPECT_EQ(expected++, *it);
  }
  EXPECT_EQ(expected, 43);

  EXPECT_THROW(TSystemError, []() {
    TRangeGenerator::NewWithSecond(1, 2, false, 0);
  });

  TRangeGenerator::NewWithSecond(2, 1, true, -1);

  expected = 14;
  for(auto it = MakeCursor(TRangeGenerator::New(14, 0, false)); it; ++it) {
    EXPECT_EQ(expected--, *it);
  }
  EXPECT_EQ(expected, 0);
}

namespace {

/* The first `max` elements of a range, however long it is. */
std::vector<int64_t> Head(const TRangeGenerator::TPtr &range, size_t max = 1000) {
  std::vector<int64_t> out;
  for (auto it = MakeCursor(range); it && out.size() < max; ++it) {
    out.push_back(*it);
  }
  return out;
}

}  // namespace

/* Ranges at the ends of int64_t, including ones that span more than INT64_MAX (#805): limit - start used to
   overflow in New and in the constructor. */
FIXTURE(RangeAtInt64Extremes) {
  using TVec = std::vector<int64_t>;
  const int64_t lo = INT64_MIN, hi = INT64_MAX;

  /* Spanning more than INT64_MAX: made with the right direction, and walked from either end. */
  EXPECT_TRUE((Head(TRangeGenerator::New(lo, hi, false), 3) == TVec{lo, lo + 1, lo + 2}));
  EXPECT_TRUE((Head(TRangeGenerator::New(lo, hi, true), 3) == TVec{lo, lo + 1, lo + 2}));
  EXPECT_TRUE((Head(TRangeGenerator::New(hi, lo, false), 3) == TVec{hi, hi - 1, hi - 2}));
  EXPECT_TRUE((Head(TRangeGenerator::New(hi, lo, true), 3) == TVec{hi, hi - 1, hi - 2}));
  EXPECT_TRUE((Head(TRangeGenerator::New(-1, hi, false), 3) == TVec{-1, 0, 1}));
  EXPECT_TRUE((Head(TRangeGenerator::New(0, lo, false), 3) == TVec{0, -1, -2}));

  /* The last elements, and stopping at the end of int64_t without stepping past it. */
  EXPECT_TRUE((Head(TRangeGenerator::New(hi - 2, hi, true)) == TVec{hi - 2, hi - 1, hi}));
  EXPECT_TRUE((Head(TRangeGenerator::New(hi - 2, hi, false)) == TVec{hi - 2, hi - 1}));
  EXPECT_TRUE((Head(TRangeGenerator::New(lo + 2, lo, true)) == TVec{lo + 2, lo + 1, lo}));
  EXPECT_TRUE((Head(TRangeGenerator::New(lo + 2, lo, false)) == TVec{lo + 2, lo + 1}));
  EXPECT_TRUE((Head(TRangeGenerator::New(hi - 1)) == TVec{hi - 1, hi}));
  EXPECT_TRUE((Head(TRangeGenerator::NewWithSecond(hi - 1, hi - 0)) == TVec{hi - 1, hi}));
  EXPECT_TRUE((Head(TRangeGenerator::NewWithSecond(lo + 1, lo)) == TVec{lo + 1, lo}));
  EXPECT_TRUE((Head(TRangeGenerator::NewWithSecond(hi - 4, hi, true, hi - 2)) == TVec{hi - 4, hi - 2, hi}));
  EXPECT_TRUE((Head(TRangeGenerator::NewWithSecond(hi - 4, hi, false, hi - 2)) == TVec{hi - 4, hi - 2}));
  EXPECT_TRUE((Head(TRangeGenerator::NewWithSecond(lo + 4, lo, true, lo + 2)) == TVec{lo + 4, lo + 2, lo}));

  /* A stride that spans more than INT64_MAX: the second element is still the second given. */
  EXPECT_TRUE((Head(TRangeGenerator::NewWithSecond(lo, hi, true, hi)) == TVec{lo, hi}));
  EXPECT_TRUE((Head(TRangeGenerator::NewWithSecond(lo, hi, false, hi)) == TVec{lo}));
  EXPECT_TRUE((Head(TRangeGenerator::NewWithSecond(hi, lo, true, lo)) == TVec{hi, lo}));
  EXPECT_TRUE((Head(TRangeGenerator::NewWithSecond(hi, lo, false, lo)) == TVec{hi}));
  EXPECT_TRUE((Head(TRangeGenerator::NewWithSecond(lo, hi)) == TVec{lo, hi}));
  EXPECT_TRUE((Head(TRangeGenerator::NewWithSecond(hi, lo)) == TVec{hi, lo}));
  EXPECT_TRUE((Head(TRangeGenerator::NewWithSecond(-1, hi, true, hi - 1)) == TVec{-1, hi - 1}));

  /* Empty and one-element ranges. */
  EXPECT_TRUE(Head(TRangeGenerator::New(5, 5, false)).empty());
  EXPECT_TRUE((Head(TRangeGenerator::New(5, 5, true)) == TVec{5}));
  EXPECT_TRUE(Head(TRangeGenerator::New(hi, hi, false)).empty());
  EXPECT_TRUE((Head(TRangeGenerator::New(hi, hi, true)) == TVec{hi}));
  EXPECT_TRUE(Head(TRangeGenerator::New(lo, lo, false)).empty());
  EXPECT_TRUE((Head(TRangeGenerator::New(lo, lo, true)) == TVec{lo}));
  EXPECT_TRUE(Head(TRangeGenerator::New(lo, lo + 1, false), 1) == TVec{lo});
  EXPECT_TRUE(Head(TRangeGenerator::New(hi - 1, hi, false)) == TVec{hi - 1});

  /* Reversed: a stride away from the limit is refused, whatever the span. */
  EXPECT_THROW(TSystemError, []() { TRangeGenerator::NewWithSecond(0, hi, true, lo); });
  EXPECT_THROW(TSystemError, []() { TRangeGenerator::NewWithSecond(0, lo, true, hi); });
  EXPECT_THROW(TSystemError, []() { TRangeGenerator::NewWithSecond(lo + 1, hi, false, lo); });
  EXPECT_THROW(TSystemError, []() { TRangeGenerator::NewWithSecond(hi - 1, lo, true, hi); });
  /* ... and the other direction is not. */
  TRangeGenerator::NewWithSecond(hi - 1, lo, true, hi - 2);

  /* Reading past the end throws rather than wrapping. */
  auto it = MakeCursor(TRangeGenerator::New(hi, hi, true));
  EXPECT_TRUE(it);
  ++it;
  EXPECT_FALSE(it);
  bool past_end = false;
  try {
    ++it;
  } catch (const TPastEndError &) {
    past_end = true;
  }
  EXPECT_TRUE(past_end);
}

FIXTURE(Stl) {
  std::vector<int> vec = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  int index = 0;
  for(auto it = MakeCursor(TStlGenerator<std::vector<int>>::New(vec)); it; ++it, ++index) {
    EXPECT_EQ(vec[index], *it);
  }
  std::unordered_set<int> Set = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  size_t count = 0;
  for(auto it = MakeCursor(TStlGenerator<std::unordered_set<int>>::New(Set)); it; ++it, ++count) {}
  EXPECT_EQ(count, Set.size());
}

FIXTURE(Dict) {
  const TDict<int64_t, int64_t> dict = {{0, 0}, {1, 1}, {2, 2}};
  size_t index = 0;
  for (auto it = MakeCursor(TStlGenerator<TDict<int64_t, int64_t>>::New(dict)); it; ++it, ++index) {
    EXPECT_TRUE(*it == std::make_tuple(index, index));
  }
  EXPECT_EQ(index, dict.size());
}

FIXTURE(VectorOfBool) {
  const std::vector<bool> vec = { true, false, false, true, false, false, true, false };
  size_t index = 0;
  for(auto it = MakeCursor(TStlGenerator<std::vector<bool>>::New(vec)); it; ++it, ++index) {
    EXPECT_EQ(*it, (index % 3) == 0);
  }
  EXPECT_EQ(index, vec.size());
}

FIXTURE(Filter) {
  std::vector<int> vec = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  bool keep = true;
  std::function<bool (const int &)> filter_func = [&keep] (const int &){
    return keep;
  };
  auto vec_gen = TStlGenerator<std::vector<int>>::New(vec);
  unsigned int index = 0;
  for(auto it = MakeCursor(TFilterGenerator<int>::New(filter_func, vec_gen)); it; ++it, ++index) {
    EXPECT_EQ(vec[index], *it);
  }
  EXPECT_EQ(index, vec.size());

  //Make sure the filter/stl generator is reusable, and that the lambda is working properly.
  index = 0u;
  for(auto it = MakeCursor(TFilterGenerator<int>::New(filter_func, vec_gen)); it; ++it, ++index, keep = !keep) {
    EXPECT_EQ(vec[index], *it);
  }
  //We should only have gotten 2 out.
  EXPECT_EQ(index, 2u);

}

FIXTURE(Map) {
  std::vector<int> vec = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  std::function<bool (const int &)> map_func = [] (const int &v) { return v % 2 == 0; };

  auto vec_gen = TStlGenerator<std::vector<int>>::New(vec);
  bool expected = false;
  for(auto it = TMapGenerator<bool, int>::New(map_func, vec_gen)->NewCursor(); it; ++it, expected = !expected) {
    EXPECT_EQ(expected, *it);
  }

}

FIXTURE(Take) {
  std::vector<int> vec {1, 2, 3, 4, 5, 6, 7, 8, 9};
  auto vec_gen = TStlGenerator<std::vector<int>>::New(vec);
  int64_t take_count = 3;
  auto iter = TTakeGenerator<int>::New(take_count, vec_gen)->NewCursor();
  for (int i = 0; iter; ++i, ++iter) {
    EXPECT_TRUE(i < take_count);
    EXPECT_EQ(*iter, vec[i]);
  }
}

FIXTURE(While) {
  // **[1, 2, 3, 4, 5, 6, 7, 8, 9] while true
  std::vector<int> vec {1, 2, 3, 4, 5, 6, 7, 8, 9};
  auto vec_gen = TStlGenerator<std::vector<int>>::New(vec);
  std::function<bool (const int &)> while_func = [](const int &) -> bool { return true; };
  auto iter = TWhileGenerator<int>::New(while_func, vec_gen)->NewCursor();
  for (int i = 0; iter; ++i, ++iter) {
    EXPECT_EQ(*iter, vec[i]);
  }
  // **[1, 2, 3, 4, 5] while that < 3
  std::vector<int> vec1 {1, 2, 3, 4, 5};
  auto vec_gen1 = TStlGenerator<std::vector<int>>::New(vec1);
  std::function<bool (const int &)> while_func1 = [](const int &n) -> bool { return n < 3; };
  auto iter1 = TWhileGenerator<int>::New(while_func1, vec_gen1)->NewCursor();
  for (int i = 0; iter1; ++i, ++iter1) {
    EXPECT_LT(*iter1, 3);
    EXPECT_TRUE(while_func1(*iter1));
    EXPECT_EQ(*iter1, vec1[i]);
  }
}

FIXTURE(GeneratorOfGenerator) { //generators of generators
  typedef TStlGenerator<std::vector<int>> TGen1;
  std::vector<int> vec = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  TGen1::TPtr vec_gen = TGen1::New(vec);
  std::vector<TGen1::TPtr> vec_vec_gen {vec_gen, vec_gen, vec_gen};
  auto vec_gen_gen = TStlGenerator<std::vector<TGen1::TPtr>>::New(vec_vec_gen);
  int outer_count = 0, inner_count = 0;
  for (auto it1 = MakeCursor(vec_gen_gen); it1; ++it1) {
    ++outer_count;
    int j = 0;
    for (auto it2 = MakeCursor(*it1); it2; ++it2, ++j) {
      EXPECT_EQ(*it2, vec[j]);  // inner sequences yield their own elements
      ++inner_count;
    }
  }
  EXPECT_EQ(outer_count, 3);
  EXPECT_EQ(inner_count, 27);
}

FIXTURE(GenericGenerators) {
  TGenerator<int64_t>::TPtr foo = TRangeGenerator::NewWithSecond(1, 2, true, 2);
  uint32_t count = 0;
  for(auto it = foo->NewCursor(); it; ++it) {
    ++count;
  }
  EXPECT_EQ(count, 2u);

  TGenerator<int64_t>::TPtr bar = TFilterGenerator<int64_t>::New([](const int64_t &) { return true; }, foo);
  count = 0;
  for(auto it2 = bar->NewCursor(); it2; ++it2) {
    ++count;
  }
  EXPECT_EQ(count, 2u);
}

/* A source that counts how far its cursor has been stepped and how many
   elements have been read from it, standing in for a walker whose every step
   decodes, and may fetch, an element (#699). */
class TCountingGenerator final
    : public TGenerator<int>,
      public std::enable_shared_from_this<TCountingGenerator> {
  public:

  struct TCounts {
    size_t Steps = 0;
    size_t Reads = 0;
  };

  using TPtr = std::shared_ptr<const TCountingGenerator>;

  class TCursor final : public Base::TIter<const int> {
    public:

    TCursor(const TPtr &ptr) : Ptr(ptr), Pos(0) {}

    virtual operator bool() const override {
      return Pos < Ptr->Size;
    }

    virtual const int &operator*() const override {
      ++Ptr->Counts->Reads;
      Val = Pos;
      return Val;
    }

    virtual Base::TIter<const int> &operator++() override {
      ++Ptr->Counts->Steps;
      ++Pos;
      return *this;
    }

    private:

    TPtr Ptr;

    int Pos;

    mutable int Val;

  };  // TCursor

  TCountingGenerator(int size, TCounts *counts) : Size(size), Counts(counts) {}

  virtual Base::TIterHolder<const int> NewCursor() const override {
    return MakeHolder(new TCursor(shared_from_this()));
  }

  int Size;

  TCounts *Counts;

};  // TCountingGenerator

FIXTURE(TakeStepsOnlyOverTakenElements) {
  for (int64_t take_count : {0L, 1L, 3L, 9L}) {
    TCountingGenerator::TCounts counts;
    auto src = std::make_shared<const TCountingGenerator>(100, &counts);
    int64_t seen = 0;
    for (auto it = TTakeGenerator<int>::New(take_count, src)->NewCursor(); it; ++it) {
      EXPECT_EQ(*it, seen);
      ++seen;
    }
    EXPECT_EQ(seen, take_count);
    /* Taking N reads N elements and steps between them N-1 times; it never
       steps onto element N+1. */
    EXPECT_EQ(counts.Reads, static_cast<size_t>(take_count));
    EXPECT_EQ(counts.Steps, static_cast<size_t>(take_count > 0 ? take_count - 1 : 0));
  }
  /* Taking more than there are stops at the end of the source. */
  TCountingGenerator::TCounts counts;
  auto src = std::make_shared<const TCountingGenerator>(5, &counts);
  int64_t seen = 0;
  for (auto it = TTakeGenerator<int>::New(10, src)->NewCursor(); it; ++it) {
    ++seen;
  }
  EXPECT_EQ(seen, 5);
  EXPECT_EQ(counts.Steps, 5u);
}
