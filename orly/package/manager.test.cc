/* <orly/package/manager.test.cc>

   Unit test for <orly/package/manager.h>: the install/upgrade/uninstall
   lifecycle rules and the lock-free reader design (#356).

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

#include <orly/package/manager.h>

#include <sys/stat.h>

#include <atomic>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <orly/compiler.h>
#include <orly/type/type_czar.h>

#include <base/util/error.h>

#include <base/test/kit.h>

using namespace std;
using namespace Base;
using namespace Orly;
using namespace Orly::Package;

namespace {

/* Compile a one-function package of the given version into pkg_dir. */
void CompileVersion(const string &scratch, const string &pkg_dir, uint64_t version) {
  const string src_path = scratch + "/sample.orly";
  {
    ofstream src(src_path);
    src << "package #" << version << ";\n"
        << "get_version = (" << version << ");\n";
  }
  Compiler::Compile(TPath(src_path), Jhm::TTree(pkg_dir), {});
}

string MakeScratch() {
  char tmpl[] = "/tmp/pkg_manager_test_XXXXXX";
  const char *dir = mkdtemp(tmpl);
  if (!dir) {
    throw runtime_error("mkdtemp failed");
  }
  return string(dir);
}

/* Both fixtures use the same two compiled package versions; compiling is by
   far the slowest thing this test does, so do it once. */
const string &SharedPkgDir() {
  static const string pkg_dir = [] {
    const string scratch = MakeScratch();
    const string dir = scratch + "/packages";
    Util::IfLt0(mkdir(dir.c_str(), 0755));
    ofstream marker(dir + "/__orly__");
    CompileVersion(scratch, dir, 1);
    CompileVersion(scratch, dir, 2);
    return dir;
  }();
  return pkg_dir;
}

}  // anonymous namespace

FIXTURE(Lifecycle) {
  Orly::Type::TTypeCzar type_czar;
  TManager manager((Jhm::TTree(SharedPkgDir())));
  const TName name{{"sample"}};

  /* Get before install throws. */
  bool caught = false;
  try {
    manager.Get(name);
  } catch (const TManager::TError &) {
    caught = true;
  }
  EXPECT_TRUE(caught);

  /* Install v1; the pre-install callback sees it. */
  size_t callbacks = 0;
  manager.Install({{name, 1}}, [&callbacks](TLoaded::TPtr, bool) { ++callbacks; });
  EXPECT_EQ(callbacks, 1U);
  EXPECT_EQ(manager.Get(name)->GetName().Version, 1U);

  /* Reinstalling the same version is a no-op: no callback. */
  manager.Install({{name, 1}}, [&callbacks](TLoaded::TPtr, bool) { ++callbacks; });
  EXPECT_EQ(callbacks, 1U);

  /* Upgrade to v2. */
  manager.Install({{name, 2}});
  EXPECT_EQ(manager.Get(name)->GetName().Version, 2U);

  /* Downgrade is refused, and refusal leaves the installed set untouched. */
  caught = false;
  try {
    manager.Install({{name, 1}});
  } catch (const TManager::TError &) {
    caught = true;
  }
  EXPECT_TRUE(caught);
  EXPECT_EQ(manager.Get(name)->GetName().Version, 2U);

  /* Yield sees exactly the one package. */
  size_t yielded = 0;
  manager.YieldInstalled([&yielded](const TVersionedName &) { ++yielded; return true; });
  EXPECT_EQ(yielded, 1U);

  /* Uninstall; Get throws again; a second uninstall throws. */
  manager.Uninstall({{name, 2}});
  caught = false;
  try {
    manager.Get(name);
  } catch (const TManager::TError &) {
    caught = true;
  }
  EXPECT_TRUE(caught);
  caught = false;
  try {
    manager.Uninstall({{name, 2}});
  } catch (const TManager::TError &) {
    caught = true;
  }
  EXPECT_TRUE(caught);

  /* A fresh install after an uninstall may be any version. */
  manager.Install({{name, 1}});
  EXPECT_EQ(manager.Get(name)->GetName().Version, 1U);
}

FIXTURE(ConcurrentReaders) {
  Orly::Type::TTypeCzar type_czar;
  TManager manager((Jhm::TTree(SharedPkgDir())));
  const TName name{{"sample"}};
  manager.Install({{name, 1}});

  /* Readers hammer Get and Yield against their own pinned snapshots while
     the writer churns the installed set underneath them (#356): a reader
     must always see either a coherent package or a clean not-installed
     error, and the snapshot must keep whatever package it returned alive. */
  atomic<bool> stop = false;
  atomic<size_t> hits = 0, misses = 0;
  vector<thread> readers;
  for (size_t i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      while (!stop) {
        try {
          TLoaded::TPtr pkg = manager.Get(name);
          const uint64_t version = pkg->GetName().Version;
          if (version != 1 && version != 2) {
            abort();
          }
          ++hits;
        } catch (const TManager::TError &) {
          ++misses;
        }
        manager.YieldInstalled([](const TVersionedName &) { return true; });
      }
    });
  }
  for (size_t cycle = 0; cycle < 50; ++cycle) {
    manager.Uninstall({{name, (cycle % 2) ? 2UL : 1UL}});
    manager.Install({{name, (cycle % 2) ? 1UL : 2UL}});
  }
  stop = true;
  for (auto &reader : readers) {
    reader.join();
  }
  EXPECT_TRUE(hits + misses > 0);
  EXPECT_EQ(manager.Get(name)->GetName().Version, 1U);
}
