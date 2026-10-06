/* <orly/server/memory_budget.h>

   Reading the memory limit a container or a systemd unit puts on this process (#669).

   orlyi sizes its pools from a memory budget. Before #669 that budget was always the host's free
   RAM, which in a container is the host's, not the container's: the released image planned for
   the whole Docker VM and was OOM-killed under `docker run --memory=1g`. The budget is now the
   smaller of free RAM and the cgroup limit read here.

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

#include <cstdint>
#include <optional>
#include <string>

namespace Orly {

  namespace Server {

    /* A cgroup memory limit and the file it came from. */
    struct TCgroupMemoryLimit {

      /* The limit, in bytes. */
      uint64_t Bytes;

      /* The file it was read from, for the startup log. */
      std::string File;

    };  // TCgroupMemoryLimit

    /* Parse the contents of a cgroup v2 `memory.max` or v1 `memory.limit_in_bytes`. Returns
       nothing for "max", for v1's "unlimited" (2^63 rounded down to a page, so anything from 2^62
       up counts), for 0 and for anything that isn't a number. */
    std::optional<uint64_t> ParseCgroupMemoryLimit(const std::string &text);

    /* The tightest memory limit any cgroup puts on this process, or nothing if there is none.

       cgroup v2: the cgroup named by the `0::<path>` line of `proc_self_cgroup` and each of its
       ancestors under `cgroup_root`, taking the smallest `memory.max`. Inside a container with its
       own cgroup namespace (Docker's default on v2) the path is `/` and `cgroup_root` is the
       container's own cgroup, so this reads `/sys/fs/cgroup/memory.max`. On a host it finds a
       systemd unit's `MemoryMax=`.

       cgroup v1, if no v2 limit was found: `memory.limit_in_bytes` in `<cgroup_root>/memory`, at
       the process's own path and at the root (where Docker mounts the container's cgroup).

       The paths are parameters so the unit test can point them at a fake tree. */
    std::optional<TCgroupMemoryLimit> ReadCgroupMemoryLimit(
        const std::string &cgroup_root = "/sys/fs/cgroup", const std::string &proc_self_cgroup = "/proc/self/cgroup");

  }  // Server

}  // Orly
