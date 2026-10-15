/* <orly/server/core_layout.test.cc>

   Unit test for <orly/server/core_layout.h>.

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

#include <orly/server/core_layout.h>

#include <algorithm>
#include <set>

#include <sched.h>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Server;

namespace {

  vector<size_t> Range(size_t first, size_t last) {
    vector<size_t> out;
    for (size_t i = first; i < last; ++i) {
      out.emplace_back(i);
    }
    return out;
  }

  /* Every CPU the layout uses. */
  set<size_t> Used(const TCoreLayout &layout) {
    set<size_t> used;
    for (const auto *vec : {&layout.Fast, &layout.Slow, &layout.MemMerge, &layout.DiskMerge, &layout.DiskController}) {
      used.insert(vec->begin(), vec->end());
    }
    return used;
  }

}  // namespace

FIXTURE(WholeMachineUnchanged) {
  /* With every CPU allowed, the layout is the table as it was written. */
  const TCoreLayout l16 = DefaultCoreLayout(Range(0, 16));
  EXPECT_TRUE(l16.Fast == (vector<size_t>{5, 6, 7, 13, 14, 15}));
  EXPECT_TRUE(l16.Slow == (vector<size_t>{0, 1, 8, 9}));
  EXPECT_TRUE(l16.MemMerge == (vector<size_t>{2, 10}));
  EXPECT_TRUE(l16.DiskMerge == (vector<size_t>{3, 11}));
  EXPECT_TRUE(l16.DiskController == (vector<size_t>{4, 12}));
  const TCoreLayout l8 = DefaultCoreLayout(Range(0, 8));
  EXPECT_TRUE(l8.Fast == (vector<size_t>{3, 6, 7}));
  EXPECT_TRUE(l8.Slow == (vector<size_t>{0, 1, 4, 5}));
  EXPECT_TRUE(l8.MemMerge == (vector<size_t>{0, 1}));
  EXPECT_TRUE(l8.DiskMerge == (vector<size_t>{4, 5}));
  EXPECT_TRUE(l8.DiskController == (vector<size_t>{2}));
  const TCoreLayout l2 = DefaultCoreLayout(Range(0, 2));
  EXPECT_TRUE(l2.Fast == (vector<size_t>{1}));
  EXPECT_TRUE(l2.DiskController == (vector<size_t>{0}));
}

FIXTURE(StaysInsideTheMask) {
  /* `taskset -c 0-11` on a 16-CPU machine used to get the 16-CPU table and put fast runners on
     13-15. Every size of mask, contiguous or not, must use only the CPUs in it. */
  for (size_t n = 2; n <= 72; ++n) {
    for (size_t stride : {1, 2, 3}) {
      vector<size_t> cpus;
      for (size_t i = 0; i < n; ++i) {
        cpus.emplace_back(4 + i * stride);
      }
      const TCoreLayout layout = DefaultCoreLayout(cpus);
      const set<size_t> allowed(cpus.begin(), cpus.end());
      for (size_t cpu : Used(layout)) {
        EXPECT_TRUE(allowed.count(cpu));
      }
      EXPECT_FALSE(layout.Fast.empty());
      EXPECT_FALSE(layout.MemMerge.empty());
      EXPECT_FALSE(layout.DiskMerge.empty());
      EXPECT_FALSE(layout.DiskController.empty());
    }
  }
}

FIXTURE(MaskedPositionsMap) {
  /* Twelve CPUs get the 8-CPU table; a mask of the odd CPUs maps position i to 2i+1. */
  const TCoreLayout l12 = DefaultCoreLayout(Range(0, 12));
  EXPECT_TRUE(l12.Fast == (vector<size_t>{3, 6, 7}));
  vector<size_t> odd;
  for (size_t i = 0; i < 16; ++i) {
    odd.emplace_back(2 * i + 1);
  }
  const TCoreLayout lodd = DefaultCoreLayout(odd);
  EXPECT_TRUE(lodd.Fast == (vector<size_t>{11, 13, 15, 27, 29, 31}));
  EXPECT_TRUE(lodd.DiskController == (vector<size_t>{9, 25}));
}

FIXTURE(TooFew) {
  EXPECT_TRUE(Used(DefaultCoreLayout({})).empty());
  EXPECT_TRUE(Used(DefaultCoreLayout({7})).empty());
}

FIXTURE(AllowedCpusIsTheMask) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  EXPECT_EQ(sched_getaffinity(0, sizeof(mask), &mask), 0);
  const vector<size_t> cpus = AllowedCpus();
  EXPECT_EQ(cpus.size(), static_cast<size_t>(CPU_COUNT(&mask)));
  EXPECT_TRUE(is_sorted(cpus.begin(), cpus.end()));
  for (size_t cpu : cpus) {
    EXPECT_TRUE(CPU_ISSET(cpu, &mask));
  }
}
