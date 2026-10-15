/* <orly/server/core_layout.cc>

   Implements <orly/server/core_layout.h>.

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

#include <sched.h>
#include <unistd.h>

using namespace std;
using namespace Orly::Server;

namespace {

  /* Positions [first, last) of the table. */
  void AddRange(vector<size_t> &vec, size_t first, size_t last) {
    for (size_t i = first; i < last; ++i) {
      vec.emplace_back(i);
    }
  }

  /* The table for n CPUs, by position. */
  TCoreLayout LayoutByPosition(size_t n) {
    TCoreLayout layout;
    /* Memory merges get cores of their own, never the disk mergers' (#904). A root flush is what
       frees the Update pools; on cores shared with the 8 disk merge threads it waited behind a long
       disk merge, memory layers piled up and writes were refused. Where the machine is big enough,
       the cores come out of the slow set. */
    if (n >= 64) {
      AddRange(layout.Slow, 0, 10);
      AddRange(layout.Slow, 32, 42);
      AddRange(layout.MemMerge, 10, 12);
      AddRange(layout.MemMerge, 42, 44);
      AddRange(layout.DiskMerge, 12, 16);
      AddRange(layout.DiskMerge, 44, 48);
      layout.DiskController = {16, 48};
      AddRange(layout.Fast, 17, 32);
      AddRange(layout.Fast, 49, 64);
    } else if (n >= 32) {
      AddRange(layout.Slow, 0, 5);
      AddRange(layout.Slow, 16, 21);
      layout.MemMerge = {5, 21};
      AddRange(layout.DiskMerge, 6, 8);
      AddRange(layout.DiskMerge, 22, 24);
      layout.DiskController = {8, 24};
      AddRange(layout.Fast, 9, 16);
      AddRange(layout.Fast, 25, 32);
    } else if (n >= 16) {
      AddRange(layout.Slow, 0, 2);
      AddRange(layout.Slow, 8, 10);
      layout.MemMerge = {2, 10};
      layout.DiskMerge = {3, 11};
      layout.DiskController = {4, 12};
      AddRange(layout.Fast, 5, 8);
      AddRange(layout.Fast, 13, 16);
    } else if (n >= 8) {
      layout.Slow = {0, 1, 4, 5};
      layout.MemMerge = {0, 1};
      layout.DiskMerge = {4, 5};
      layout.DiskController = {2};
      layout.Fast = {3, 6, 7};
    } else if (n >= 4) {
      layout.MemMerge = {0};
      layout.DiskMerge = {2};
      layout.Slow = {0};
      layout.DiskController = {2};
      layout.Fast = {1, 3};
    } else if (n >= 2) {
      layout.MemMerge = {0};
      layout.DiskMerge = {0};
      layout.Slow = {0};
      layout.DiskController = {0};
      layout.Fast = {1};
    }
    return layout;
  }

}  // namespace

TCoreLayout Orly::Server::DefaultCoreLayout(const vector<size_t> &cpus) {
  TCoreLayout layout = LayoutByPosition(cpus.size());
  for (auto *vec : {&layout.Fast, &layout.Slow, &layout.MemMerge, &layout.DiskMerge, &layout.DiskController}) {
    for (size_t &cpu : *vec) {
      cpu = cpus[cpu];
    }
  }
  return layout;
}

vector<size_t> Orly::Server::AllowedCpus() {
  vector<size_t> cpus;
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (sched_getaffinity(0, sizeof(mask), &mask) == 0) {
    for (size_t cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (CPU_ISSET(cpu, &mask)) {
        cpus.emplace_back(cpu);
      }
    }
  }
  if (cpus.empty()) {
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    for (long cpu = 0; cpu < online; ++cpu) {
      cpus.emplace_back(static_cast<size_t>(cpu));
    }
  }
  return cpus;
}
