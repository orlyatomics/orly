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
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <sched.h>
#include <unistd.h>

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
  /* A mask of the odd CPUs maps position i to 2i+1. */
  vector<size_t> odd;
  for (size_t i = 0; i < 16; ++i) {
    odd.emplace_back(2 * i + 1);
  }
  const TCoreLayout lodd = DefaultCoreLayout(odd);
  EXPECT_TRUE(lodd.Fast == (vector<size_t>{11, 13, 15, 27, 29, 31}));
  EXPECT_TRUE(lodd.DiskController == (vector<size_t>{9, 25}));
}

FIXTURE(TwelveToFifteen) {
  /* 12-15 CPUs get six merge cores, one per disk merge thread, out of the slow set; the 8-CPU
     table put 8 disk merge threads on 2 cores. Past 12 the rest are left idle, as before. */
  for (size_t n : {12, 15}) {
    const TCoreLayout layout = DefaultCoreLayout(Range(0, n));
    EXPECT_TRUE(layout.Fast == (vector<size_t>{5, 6, 7}));
    EXPECT_TRUE(layout.Slow == (vector<size_t>{0, 1}));
    EXPECT_TRUE(layout.MemMerge == (vector<size_t>{2, 3}));
    EXPECT_TRUE(layout.DiskMerge == (vector<size_t>{8, 9, 10, 11}));
    EXPECT_TRUE(layout.DiskController == (vector<size_t>{4}));
    EXPECT_EQ(layout.DiskMergeThreads, 4UL);
  }
  /* Other sizes keep 8 disk merge threads. */
  for (size_t n : {2, 8, 11, 16, 32, 64}) {
    EXPECT_EQ(DefaultCoreLayout(Range(0, n)).DiskMergeThreads, 8UL);
  }
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

FIXTURE(DefaultsGoAroundClaimedCpus) {
  const vector<size_t> cpus = Range(0, 16);
  /* Nothing claimed is the plain layout. */
  EXPECT_TRUE(DefaultCoreLayout(cpus, {}).MemMerge == DefaultCoreLayout(cpus).MemMerge);
  /* The 16-CPU table puts memory merges on 2 and 10 and the disk controllers on 4 and 12. Claim those for fast
     runners (as --fast_cores 2,4,10,12 would) and the defaults for the other pools must not use them. */
  const vector<size_t> claimed = {2, 4, 10, 12};
  const TCoreLayout layout = DefaultCoreLayout(cpus, claimed);
  for (const auto *vec : {&layout.Slow, &layout.MemMerge, &layout.DiskMerge, &layout.DiskController}) {
    EXPECT_FALSE(vec->empty());
    for (const size_t cpu : *vec) {
      EXPECT_TRUE(find(claimed.begin(), claimed.end(), cpu) == claimed.end());
      EXPECT_LT(cpu, 16UL);
    }
  }
  /* Claim all but one CPU: nothing is left to place around the claim, so the layout is the plain one. */
  EXPECT_TRUE(DefaultCoreLayout(cpus, Range(0, 15)).MemMerge == DefaultCoreLayout(cpus).MemMerge);
  /* A claim outside the allowed CPUs changes nothing. */
  EXPECT_TRUE(DefaultCoreLayout(cpus, {40, 41}).DiskMerge == DefaultCoreLayout(cpus).DiskMerge);
}

FIXTURE(CpuLists) {
  EXPECT_TRUE(ParseCpuList("0,28") == (vector<size_t>{0, 28}));
  EXPECT_TRUE(ParseCpuList("0-3,8\n") == (vector<size_t>{0, 1, 2, 3, 8}));
  EXPECT_TRUE(ParseCpuList("5") == (vector<size_t>{5}));
  EXPECT_TRUE(ParseCpuList("").empty());
  EXPECT_TRUE(ParseCpuList("x,7") == (vector<size_t>{7}));
}

FIXTURE(SiblingGroupsFromSysfs) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("core_layout_test_" + std::to_string(getpid()));
  fs::create_directories(dir);
  const auto add = [&](const string &cpu, const string &siblings) {
    fs::create_directories(dir / cpu / "topology");
    ofstream(dir / cpu / "topology" / "thread_siblings_list") << siblings << "\n";
  };
  add("cpu0", "0,4");
  add("cpu4", "0,4");
  add("cpu1", "1,5");
  add("cpu5", "1,5");
  add("cpu2", "2");  // No sibling: not a group.
  fs::create_directories(dir / "cpufreq");  // Not a CPU.
  const auto groups = ReadSiblingGroups(dir.string());
  EXPECT_TRUE(groups == (vector<vector<size_t>>{{0, 4}, {1, 5}}));
  EXPECT_TRUE(ReadSiblingGroups((dir / "missing").string()).empty());
  fs::remove_all(dir);
}

FIXTURE(FastSiblingConflictsAreListed) {
  const vector<vector<size_t>> groups = {{0, 4}, {1, 5}, {2, 6}};
  TCoreLayout layout;
  layout.Fast = {0, 1};
  layout.MemMerge = {5};
  layout.DiskMerge = {2};
  /* 1's sibling 5 is a memory merge; 0's sibling 4 is idle; 2 is not fast. */
  EXPECT_TRUE(FastSiblingConflicts(layout, groups) == (vector<pair<size_t, size_t>>{{1, 5}}));
  /* Two fast runners on one core conflict with each other, once each. */
  layout.Fast = {0, 4};
  EXPECT_TRUE(FastSiblingConflicts(layout, groups) == (vector<pair<size_t, size_t>>{{0, 4}, {4, 0}}));
  /* No topology, no conflicts. */
  EXPECT_TRUE(FastSiblingConflicts(layout, {}).empty());
}
