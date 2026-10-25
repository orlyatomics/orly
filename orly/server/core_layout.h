/* <orly/server/core_layout.h>

   The default assignment of orlyi's thread pools to CPUs.

   The layout is a table per machine size, written for CPUs 0..n-1. Before this it took n from
   the number of CPUs online and used those numbers as they were, so a process held to some of
   the machine (`taskset`, a container's cpuset) still placed its threads by the whole machine's
   table: under `taskset -c 0-11` on 16 CPUs, three of the six fast runners and a disk controller
   pinned themselves to 12-15, outside the mask and onto whatever was meant to run there. The
   table is now chosen by the number of CPUs this process may run on, and position i in it is the
   i-th of those CPUs, so a process with every CPU gets the layout it always did.

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

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Orly {

  namespace Server {

    /* The CPUs each of orlyi's thread pools runs on. */
    struct TCoreLayout {

      std::vector<size_t> Fast, Slow, MemMerge, DiskMerge, DiskController;

      /* How many disk merge threads to run over DiskMerge, unless --num_disk_merge_threads says. */
      size_t DiskMergeThreads = 8;

    };  // TCoreLayout

    /* The default layout over cpus, which is ascending: the table for cpus.size() CPUs, with
       position i mapped to cpus[i]. Fewer than 2 CPUs gets an empty layout (orlyi does not support
       a single processor). */
    TCoreLayout DefaultCoreLayout(const std::vector<size_t> &cpus);

    /* The same, for pools the command line did not place when it placed others. The CPUs in `claimed`
       (the ones the user gave) are left out of cpus before the table is chosen, so a default pool never
       lands on a CPU the user pinned something else to. Left with fewer than 2 CPUs, there is nothing
       to place around the claim and the layout is over all of cpus, as before. The caller keeps only
       the pools it needs: the user's own come from the command line. */
    TCoreLayout DefaultCoreLayout(const std::vector<size_t> &cpus, const std::vector<size_t> &claimed);

    /* The CPUs this process may run on, ascending, from its affinity mask. If the mask can't be
       read, every CPU online. */
    std::vector<size_t> AllowedCpus();

    /* The CPUs in a list as the kernel writes them ("0,28", "0-3,8"). Anything unparsable is skipped. */
    std::vector<size_t> ParseCpuList(const std::string &text);

    /* The groups of CPUs that share a physical core (hyper-threads), each ascending, from
       <sys_cpu_dir>/cpuN/topology/thread_siblings_list. Only groups of two or more are returned. Empty
       where the topology can't be read. */
    std::vector<std::vector<size_t>> ReadSiblingGroups(const std::string &sys_cpu_dir = "/sys/devices/system/cpu");

    /* Each (fast CPU, other CPU) where another pinned thread pool (or another fast runner) is on a
       hyper-thread sibling of a fast runner's CPU. A busy sibling
       shares the core's execution resources with it; measurements of read latency under write load found the
       read tail depends on it. */
    std::vector<std::pair<size_t, size_t>> FastSiblingConflicts(const TCoreLayout &layout,
                                                               const std::vector<std::vector<size_t>> &groups);

  }  // Server

}  // Orly
