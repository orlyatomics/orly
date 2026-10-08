/* <orly/package/manager.test.cc>

   Unit test for <orly/package/manager.h>: the install/upgrade/uninstall
   lifecycle rules and the lock-free reader design (#356), and the loader's
   handling of packages that share a record type (#797).

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

#include <algorithm>
#include <atomic>
#include <cstdlib>
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

/* Compile `source` (a whole package) into pkg_dir as `name`. */
void CompilePackage(const string &scratch, const string &pkg_dir, const string &name, const string &source) {
  const string src_path = scratch + "/" + name + ".orly";
  {
    ofstream src(src_path);
    src << source;
  }
  Compiler::Compile(TPath(src_path), Jhm::TTree(pkg_dir), {});
}

/* Build a shared object with g++ from `source`, at pkg_dir/<so_name>. */
void BuildSo(const string &scratch, const string &pkg_dir, const string &so_name, const string &source) {
  const string src_path = scratch + "/" + so_name + ".cc";
  {
    ofstream src(src_path);
    src << source;
  }
  const string cmd = "g++ -std=c++23 -fPIC -shared -o '" + pkg_dir + "/" + so_name + "' '" + src_path + "'";
  if (system(cmd.c_str()) != 0) {
    throw runtime_error("failed: " + cmd);
  }
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

  /* Uninstalling a version that isn't the installed one is refused with a typed error naming the installed
     version, and the package stays installed (#800). */
  string refusal;
  caught = false;
  try {
    manager.Uninstall({{name, 1}});
  } catch (const TManager::TVersionNotInstalledError &ex) {
    caught = true;
    refusal = ex.what();
  }
  EXPECT_TRUE(caught);
  EXPECT_TRUE(refusal.find("version 2 is the one installed") != string::npos);
  EXPECT_EQ(manager.Get(name)->GetName().Version, 2U);

  /* So is a version newer than the installed one. */
  caught = false;
  try {
    manager.Uninstall({{name, 3}});
  } catch (const TManager::TVersionNotInstalledError &) {
    caught = true;
  }
  EXPECT_TRUE(caught);
  EXPECT_EQ(manager.Get(name)->GetName().Version, 2U);

  /* A batch is all or nothing: a good entry isn't uninstalled when another one in the batch is refused. */
  const TName other{{"other"}};
  caught = false;
  try {
    manager.Uninstall({{name, 2}, {other, 1}});
  } catch (const TManager::TError &) {
    caught = true;
  }
  EXPECT_TRUE(caught);
  EXPECT_EQ(manager.Get(name)->GetName().Version, 2U);

  /* A package that isn't installed at all is still the plain TError, not the version one. */
  caught = false;
  bool wrong_type = false;
  try {
    manager.Uninstall({{other, 1}});
  } catch (const TManager::TVersionNotInstalledError &) {
    wrong_type = true;
  } catch (const TManager::TError &) {
    caught = true;
  }
  EXPECT_TRUE(caught);
  EXPECT_FALSE(wrong_type);

  /* Uninstall at the installed version; Get throws again; a second uninstall throws. */
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

/* #797: every package used to export its record registry -- the static
   members of Orly::Native::Record<T> -- as STB_GNU_UNIQUE symbols, so a
   second package declaring the same record type (or the next version of the
   same package) shared and corrupted the first one's, and the next read of
   those records crashed orlyi. A freshly compiled package must keep its
   record registry to itself. */
FIXTURE(RecordRegistryIsPrivateToEachPackage) {
  Orly::Type::TTypeCzar type_czar;
  const string scratch = MakeScratch();
  const string pkg_dir = scratch + "/packages";
  Util::IfLt0(mkdir(pkg_dir.c_str(), 0755));
  ofstream marker(pkg_dir + "/__orly__");
  const string body =
      "v_t is <{.oid: str, .p: str}>;\n"
      "get = (<{.oid: \"o\", .p: \"p\"}>);\n";
  CompilePackage(scratch, pkg_dir, "rec", "package #1;\n" + body);
  CompilePackage(scratch, pkg_dir, "rec", "package #2;\n" + body);
  CompilePackage(scratch, pkg_dir, "other", "package #1;\n" + body);
  for (const char *so : {"rec.1.so", "rec.2.so", "other.1.so"}) {
    const auto symbols = GetUniqueRecordSymbols(pkg_dir + "/" + so);
    EXPECT_TRUE(symbols.empty());
    for (const auto &symbol : symbols) {
      cerr << so << " exports unique record symbol " << symbol << endl;
    }
  }

  /* Both versions and the other package install side by side. */
  TManager manager((Jhm::TTree(pkg_dir)));
  manager.Install({{TName{{"rec"}}, 1}, {TName{{"other"}}, 1}});
  manager.Install({{TName{{"rec"}}, 2}});
  EXPECT_EQ(manager.Get(TName{{"rec"}})->GetName().Version, 2U);
}

/* A package compiled before the fix still exports its registry, and two such
   packages declaring the same record type cannot be made safe once loaded
   together. The loader must refuse the second one with an error instead of
   loading it and crashing on the next read. Stand-ins built with g++ carry the
   same kind of symbol a pre-#797 package did. */
FIXTURE(LegacyRecordRegistryCollisionIsRefused) {
  const string scratch = MakeScratch();
  const string pkg_dir = scratch + "/packages";
  Util::IfLt0(mkdir(pkg_dir.c_str(), 0755));
  const string legacy =
      "namespace Orly { namespace Rt { namespace Objects { struct TObjLegacy797 {}; } } }\n"
      "namespace Orly { namespace Native {\n"
      "  template <typename T> struct Record { static unsigned long ElemCount; };\n"
      "  template <typename T> unsigned long Record<T>::ElemCount = 0;\n"
      "} }\n"
      "unsigned long *Use() { return &Orly::Native::Record<Orly::Rt::Objects::TObjLegacy797>::ElemCount; }\n";
  BuildSo(scratch, pkg_dir, "legacya.1.so", legacy);
  BuildSo(scratch, pkg_dir, "legacyb.1.so", legacy);
  BuildSo(scratch, pkg_dir, "unrelated.1.so",
          "namespace Orly { namespace Rt { namespace Objects { struct TObjOther797 {}; } } }\n"
          "namespace Orly { namespace Native {\n"
          "  template <typename T> struct Record { static unsigned long ElemCount; };\n"
          "  template <typename T> unsigned long Record<T>::ElemCount = 0;\n"
          "} }\n"
          "unsigned long *Use() { return &Orly::Native::Record<Orly::Rt::Objects::TObjOther797>::ElemCount; }\n");
  const auto symbols = GetUniqueRecordSymbols(pkg_dir + "/legacya.1.so");
  EXPECT_EQ(symbols.size(), 1U);

  /* The stand-ins aren't packages (no GetApiVersion), so even a load that
     gets past the guard fails -- but after dlopen, which is the point at
     which a real package's registry would be bound. */
  const Jhm::TTree tree(pkg_dir);
  auto load_error = [&tree](const char *name) -> string {
    try {
      TLoaded::Load(tree, TVersionedName{TName{{name}}, 1});
    } catch (const TLoaderError &ex) {
      return string("loader: ") + ex.what();
    } catch (const exception &ex) {
      return string("other: ") + ex.what();
    }
    return "loaded";
  };
  const string first = load_error("legacya");
  EXPECT_TRUE(first.starts_with("other: "));
  const string second = load_error("legacyb");
  EXPECT_TRUE(second.starts_with("loader: "));
  EXPECT_TRUE(second.find("#797") != string::npos);
  EXPECT_TRUE(second.find("TObjLegacy797") != string::npos);
  /* A different record type is no collision. */
  EXPECT_TRUE(load_error("unrelated").starts_with("other: "));
  cerr << "first: " << first << endl << "second: " << second << endl;
}
