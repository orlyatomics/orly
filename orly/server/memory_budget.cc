/* <orly/server/memory_budget.cc>

   Implements <orly/server/memory_budget.h>.

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

#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <vector>

using namespace std;

namespace {

  /* The first line of a file, or nothing if it can't be read. */
  optional<string> ReadFirstLine(const string &path) {
    ifstream strm(path);
    string line;
    if (!strm || !getline(strm, line)) {
      return nullopt;
    }
    return line;
  }

  /* Keep the smaller of the limit so far and the one in this file, if it has one. */
  void TakeTighter(optional<Orly::Server::TCgroupMemoryLimit> &best, const string &file) {
    auto text = ReadFirstLine(file);
    if (!text) {
      return;
    }
    auto bytes = Orly::Server::ParseCgroupMemoryLimit(*text);
    if (bytes && (!best || *bytes < best->Bytes)) {
      best = Orly::Server::TCgroupMemoryLimit{*bytes, file};
    }
  }

  /* `path` and each of its ancestors, deepest first: "/a/b" gives "/a/b", "/a" and "". */
  vector<string> SelfAndAncestors(string path) {
    vector<string> result;
    while (!path.empty() && path.back() == '/') {
      path.pop_back();
    }
    for (;;) {
      result.push_back(path);
      if (path.empty()) {
        break;
      }
      path.resize(path.rfind('/') == string::npos ? 0 : path.rfind('/'));
    }
    return result;
  }

}  // namespace

optional<uint64_t> Orly::Server::ParseCgroupMemoryLimit(const string &text) {
  size_t begin = 0, end = text.size();
  while (begin < end && isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  uint64_t bytes = 0;
  auto [ptr, ec] = from_chars(text.data() + begin, text.data() + end, bytes);
  if (ec != errc() || ptr != text.data() + end || bytes == 0 || bytes >= (1ULL << 62)) {
    return nullopt;
  }
  return bytes;
}

optional<Orly::Server::TCgroupMemoryLimit> Orly::Server::ReadCgroupMemoryLimit(
    const string &cgroup_root, const string &proc_self_cgroup) {
  /* Our own cgroup's path in each hierarchy: "0::<path>" for v2, "<n>:<controllers>:<path>" for
     v1, where the controllers are a comma-separated list that may include "memory". */
  optional<string> v2_path, v1_memory_path;
  ifstream strm(proc_self_cgroup);
  for (string line; getline(strm, line);) {
    const auto first = line.find(':');
    if (first == string::npos) {
      continue;
    }
    const auto second = line.find(':', first + 1);
    if (second == string::npos) {
      continue;
    }
    const string id = line.substr(0, first), controllers = line.substr(first + 1, second - first - 1),
                 path = line.substr(second + 1);
    if (id == "0" && controllers.empty()) {
      v2_path = path;
    } else {
      stringstream list(controllers);
      for (string controller; getline(list, controller, ',');) {
        if (controller == "memory") {
          v1_memory_path = path;
        }
      }
    }
  }
  optional<TCgroupMemoryLimit> best;
  /* v2. A path that doesn't exist under cgroup_root (a namespace we can't see out of) just
     contributes nothing, and the root is always tried. */
  for (const string &dir: SelfAndAncestors(v2_path.value_or(""))) {
    TakeTighter(best, cgroup_root + dir + "/memory.max");
  }
  if (best) {
    return best;
  }
  /* v1. */
  if (v1_memory_path) {
    TakeTighter(best, cgroup_root + "/memory" + *v1_memory_path + "/memory.limit_in_bytes");
  }
  TakeTighter(best, cgroup_root + "/memory/memory.limit_in_bytes");
  return best;
}
