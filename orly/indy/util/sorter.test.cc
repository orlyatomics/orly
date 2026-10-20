/* <orly/indy/util/sorter.test.cc>

   Unit test for <orly/indy/util/sorter.h>.

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

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <orly/indy/util/sorter.h>

#include <base/test/kit.h>

using namespace std;
using namespace Base;
using namespace Orly::Indy::Util;

FIXTURE(Typical) {
  const size_t max_size = 100;
  TSorter<size_t, max_size> sorter;
  for(size_t i = 0; i < max_size; ++i) {
    sorter.Emplace(max_size - i);
  }
  EXPECT_EQ(sorter.GetSize(), max_size);
  size_t found = 0U;
  size_t expected = 100U;
  for (auto iter : sorter) {
    EXPECT_EQ(iter, expected--);
    ++found;
  }
  EXPECT_EQ(found, 100U);
  EXPECT_EQ(sorter.GetSize(), found);
  sort(sorter.begin(), sorter.end());
  found = 0U;
  expected = 1U;
  for (auto iter : sorter) {
    ++found;
    EXPECT_EQ(iter, expected++);
  }
  EXPECT_EQ(found, 100U);
  EXPECT_EQ(sorter.GetSize(), found);
  sorter.Clear();
  found = 0U;
  for (auto iter : sorter) {
    assert(iter);
    ++found;
  }
  EXPECT_EQ(found, 0U);
  EXPECT_EQ(sorter.GetSize(), found);
}
/* #988: the buffer starts empty, takes InitialCapacity values when the first arrives, and doubles from there until it
   holds MaxSize, which it never passes. The sorter is full at exactly MaxSize values, as when it took MaxSize up
   front, and the values survive every growth in order. */
FIXTURE(GrowsByDoublingToMaxSize) {
  constexpr size_t max_size = 5000U;
  using TSizeSorter = TSorter<size_t, max_size>;
  TSizeSorter sorter;
  EXPECT_EQ(sorter.GetCapacity(), 0U);
  EXPECT_EQ(TSizeSorter::InitialCapacity, 1024U);
  const vector<pair<size_t, size_t>> steps = {{1U, 1024U}, {1024U, 1024U}, {1025U, 2048U}, {2048U, 2048U},
                                              {2049U, 4096U}, {4096U, 4096U}, {4097U, max_size}, {max_size, max_size}};
  size_t next = 0U;
  for (const auto &[size, capacity] : steps) {
    while (next < size) {
      EXPECT_FALSE(sorter.IsFull());
      sorter.Emplace(next * 7U);
      ++next;
    }
    EXPECT_EQ(sorter.GetSize(), size);
    EXPECT_EQ(sorter.GetCapacity(), capacity);
  }
  EXPECT_TRUE(sorter.IsFull());
  for (size_t i = 0U; i < max_size; ++i) {
    if (!EXPECT_EQ(sorter[i], i * 7U)) {
      break;
    }
  }
}

/* A sorter smaller than InitialCapacity takes only its MaxSize. */
FIXTURE(SmallSorterTakesOnlyMaxSize) {
  TSorter<size_t, 100U> sorter;
  sorter.Emplace(1U);
  EXPECT_EQ(sorter.GetCapacity(), 100U);
  for (size_t i = 1U; i < 100U; ++i) {
    sorter.Emplace(i);
  }
  EXPECT_TRUE(sorter.IsFull());
  EXPECT_EQ(sorter.GetCapacity(), 100U);
}

/* Clear keeps the buffer, so refilling a sorter that spilled does not grow it again. */
FIXTURE(ClearKeepsCapacity) {
  constexpr size_t max_size = 3000U;
  TSorter<size_t, max_size> sorter;
  for (size_t i = 0U; i < max_size; ++i) {
    sorter.Emplace(i);
  }
  EXPECT_EQ(sorter.GetCapacity(), max_size);
  sorter.Clear();
  EXPECT_EQ(sorter.GetSize(), 0U);
  EXPECT_EQ(sorter.GetCapacity(), max_size);
  sorter.Emplace(42U);
  EXPECT_EQ(sorter.GetCapacity(), max_size);
  EXPECT_EQ(sorter[0], 42U);
}

namespace {

  /* A value with a real move constructor, which marks what it moved from, so a growth that lost or garbled a value
     shows. */
  struct TTracked {
    TTracked(uint64_t val) : Val(val), Live(true) {}
    TTracked(TTracked &&that) noexcept : Val(that.Val), Live(that.Live) { that.Live = false; }
    TTracked &operator=(TTracked &&that) noexcept {
      Val = that.Val;
      Live = that.Live;
      that.Live = false;
      return *this;
    }
    bool operator<(const TTracked &that) const { return Val < that.Val; }
    uint64_t Val;
    bool Live;
  };

}

/* The spill loop TIndexManager runs: emplace until the sorter is full, then sort it, write it out as a run, and clear
   it. With a growing buffer the spills come at exactly the same values and the runs are exactly those a buffer of
   MaxSize taken up front gives, which is a plain chunking of the input into MaxSize-value pieces, each sorted. */
FIXTURE(SpillsMatchUpFrontAllocation) {
  constexpr size_t max_size = 3000U;
  constexpr size_t num_values = 4U * max_size + 1234U;
  vector<uint64_t> input;
  uint64_t x = 988U;
  for (size_t i = 0U; i < num_values; ++i) {
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    input.push_back(x >> 40);
  }
  /* the runs a buffer of max_size taken up front gives */
  vector<vector<uint64_t>> want;
  for (size_t begin = 0U; begin < num_values; begin += max_size) {
    want.emplace_back(input.begin() + begin, input.begin() + min(num_values, begin + max_size));
    sort(want.back().begin(), want.back().end());
  }
  /* and those of the growing one */
  TSorter<TTracked, max_size> sorter;
  vector<vector<uint64_t>> got;
  vector<size_t> spilled_at;
  const auto spill = [&] {
    sort(sorter.begin(), sorter.end());
    got.emplace_back();
    for (const auto &val : sorter) {
      EXPECT_TRUE(val.Live);
      got.back().push_back(val.Val);
    }
    sorter.Clear();
  };
  for (size_t i = 0U; i < num_values; ++i) {
    if (sorter.IsFull()) {
      spilled_at.push_back(i);
      spill();
    }
    sorter.Emplace(input[i]);
  }
  spill();
  EXPECT_TRUE(spilled_at == vector<size_t>({max_size, 2U * max_size, 3U * max_size, 4U * max_size}));
  EXPECT_TRUE(got == want);
}
