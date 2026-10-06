/* <orly/server/memory_budget.test.cc>

   Unit test for <orly/server/memory_budget.h>.

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

#include <orly/server/memory_budget.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include <base/test/kit.h>

using namespace std;
using namespace Orly::Server;

namespace {

  /* A throwaway directory standing in for /sys/fs/cgroup and /proc/self/cgroup. */
  class TFakeCgroupFs {
    public:

    TFakeCgroupFs() {
      char tmpl[] = "/tmp/memory_budget_test.XXXXXX";
      Root = mkdtemp(tmpl);
    }

    ~TFakeCgroupFs() {
      system(("rm -rf " + Root).c_str());
    }

    /* Write `text` to `rel` under the root, creating its directories. */
    void Write(const string &rel, const string &text) const {
      for (size_t pos = rel.find('/', 1); pos != string::npos; pos = rel.find('/', pos + 1)) {
        mkdir((Root + rel.substr(0, pos)).c_str(), 0755);
      }
      ofstream(Root + rel) << text;
    }

    optional<TCgroupMemoryLimit> Read() const {
      return ReadCgroupMemoryLimit(Root + "/cg", Root + "/self");
    }

    string Root;

  };  // TFakeCgroupFs

}  // namespace

FIXTURE(Parse) {
  EXPECT_EQ(ParseCgroupMemoryLimit("1073741824\n").value_or(0), 1073741824UL);
  EXPECT_EQ(ParseCgroupMemoryLimit(" 4096 ").value_or(0), 4096UL);
  EXPECT_FALSE(ParseCgroupMemoryLimit("max\n").has_value());
  /* v1's "unlimited". */
  EXPECT_FALSE(ParseCgroupMemoryLimit("9223372036854771712\n").has_value());
  EXPECT_FALSE(ParseCgroupMemoryLimit("0").has_value());
  EXPECT_FALSE(ParseCgroupMemoryLimit("").has_value());
  EXPECT_FALSE(ParseCgroupMemoryLimit("12k").has_value());
}

FIXTURE(DockerV2) {
  /* `docker run --memory=1g` with a private cgroup namespace. */
  TFakeCgroupFs fs;
  fs.Write("/self", "0::/\n");
  fs.Write("/cg/memory.max", "1073741824\n");
  auto limit = fs.Read();
  EXPECT_TRUE(limit.has_value());
  EXPECT_EQ(limit->Bytes, 1073741824UL);
  EXPECT_EQ(limit->File, fs.Root + "/cg/memory.max");
}

FIXTURE(DockerV2Unlimited) {
  TFakeCgroupFs fs;
  fs.Write("/self", "0::/\n");
  fs.Write("/cg/memory.max", "max\n");
  EXPECT_FALSE(fs.Read().has_value());
}

FIXTURE(NoCgroupFs) {
  /* A bare host without cgroups mounted where we look, or a test sandbox. */
  TFakeCgroupFs fs;
  EXPECT_FALSE(fs.Read().has_value());
}

FIXTURE(V2TightestAncestorWins) {
  /* A systemd unit with MemoryMax= under a slice with a tighter one; the root has no file. */
  TFakeCgroupFs fs;
  fs.Write("/self", "0::/system.slice/orly.service\n");
  fs.Write("/cg/system.slice/orly.service/memory.max", "4294967296\n");
  fs.Write("/cg/system.slice/memory.max", "2147483648\n");
  auto limit = fs.Read();
  EXPECT_TRUE(limit.has_value());
  EXPECT_EQ(limit->Bytes, 2147483648UL);
  EXPECT_EQ(limit->File, fs.Root + "/cg/system.slice/memory.max");
}

FIXTURE(V2PathOutsideOurView) {
  /* The path names a cgroup this mount doesn't show; the root's limit still counts. */
  TFakeCgroupFs fs;
  fs.Write("/self", "0::/kubepods/pod1/abc\n");
  fs.Write("/cg/memory.max", "536870912\n");
  EXPECT_EQ(fs.Read().value().Bytes, 536870912UL);
}

FIXTURE(DockerV1) {
  /* cgroup v1: Docker mounts the container's own memory cgroup at /sys/fs/cgroup/memory. */
  TFakeCgroupFs fs;
  fs.Write("/self", "12:pids:/docker/abc\n4:cpu,cpuacct:/docker/abc\n3:memory:/docker/abc\n0::/\n");
  fs.Write("/cg/memory/memory.limit_in_bytes", "1073741824\n");
  EXPECT_EQ(fs.Read().value().Bytes, 1073741824UL);
}

FIXTURE(V1Unlimited) {
  TFakeCgroupFs fs;
  fs.Write("/self", "3:memory:/\n");
  fs.Write("/cg/memory/memory.limit_in_bytes", "9223372036854771712\n");
  EXPECT_FALSE(fs.Read().has_value());
}

FIXTURE(V1HostPath) {
  /* cgroup v1 on a host: the process's own memory cgroup, found by path. */
  TFakeCgroupFs fs;
  fs.Write("/self", "5:blkio,memory:/user/orly\n");
  fs.Write("/cg/memory/memory.limit_in_bytes", "9223372036854771712\n");
  fs.Write("/cg/memory/user/orly/memory.limit_in_bytes", "3221225472\n");
  EXPECT_EQ(fs.Read().value().Bytes, 3221225472UL);
}
