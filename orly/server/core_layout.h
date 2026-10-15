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
#include <vector>

namespace Orly {

  namespace Server {

    /* The CPUs each of orlyi's thread pools runs on. */
    struct TCoreLayout {

      std::vector<size_t> Fast, Slow, MemMerge, DiskMerge, DiskController;

    };  // TCoreLayout

    /* The default layout over cpus, which is ascending: the table for cpus.size() CPUs, with
       position i mapped to cpus[i]. Fewer than 2 CPUs gets an empty layout (orlyi does not support
       a single processor). */
    TCoreLayout DefaultCoreLayout(const std::vector<size_t> &cpus);

    /* The CPUs this process may run on, ascending, from its affinity mask. If the mask can't be
       read, every CPU online. */
    std::vector<size_t> AllowedCpus();

  }  // Server

}  // Orly
