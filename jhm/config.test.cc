/* <jhm/config.test.cc>

   Unit test for <jhm/config.h>.

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

#include <jhm/config.h>

#include <fstream>
#include <unistd.h>

#include <base/test/kit.h>
#include <base/util/path.h>

using namespace Base;
using namespace Jhm;
using namespace std;
using namespace Util;

namespace {

  /* A scratch directory, unique per test process, holding the .jhm fixture files below. Cleaned
     up (best-effort) at process exit via a static dtor. */
  class TScratchDir final {
    public:
    TScratchDir() : Path("/tmp/jhm_config_test_" + to_string(getpid())) {
      EnsureDirExists(Path.c_str());
    }
    ~TScratchDir() {
      EnsureDirIsGone(Path.c_str());
    }
    string File(const string &name) const {
      return Path + '/' + name;
    }
    private:
    string Path;
  };

  TScratchDir Scratch;

  void WriteConfig(const string &path, TJson::TObject &&obj) {
    ofstream out(path);
    TJson(move(obj)).Write(out);
  }

} // namespace

FIXTURE(Parent) {
  WriteConfig(Scratch.File("base.jhm"), {{"a", TJson(1.0)}});
  WriteConfig(Scratch.File("child.jhm"), {{"parent", TJson(string("base.jhm"))}, {"b", TJson(2.0)}});

  TConfig conf(Scratch.File("child.jhm"));
  EXPECT_EQ(conf.Read<double>({"a"}), 1.0);
  EXPECT_EQ(conf.Read<double>({"b"}), 2.0);
}

FIXTURE(ChildOverridesParent) {
  WriteConfig(Scratch.File("base2.jhm"), {{"a", TJson(1.0)}});
  WriteConfig(Scratch.File("child2.jhm"), {{"parent", TJson(string("base2.jhm"))}, {"a", TJson(9.0)}});

  TConfig conf(Scratch.File("child2.jhm"));
  EXPECT_EQ(conf.Read<double>({"a"}), 9.0);
}

FIXTURE(IncludeLastWins) {
  WriteConfig(Scratch.File("inc1.jhm"), {{"x", TJson(1.0)}});
  WriteConfig(Scratch.File("inc2.jhm"), {{"x", TJson(2.0)}});
  WriteConfig(Scratch.File("main.jhm"), {
      {"include", TJson(TJson::TArray{TJson(string("inc1.jhm")), TJson(string("inc2.jhm"))})}});

  TConfig conf(Scratch.File("main.jhm"));
  EXPECT_EQ(conf.Read<double>({"x"}), 2.0);
}

FIXTURE(OwnEntryBeatsIncludes) {
  WriteConfig(Scratch.File("inc3.jhm"), {{"x", TJson(1.0)}});
  WriteConfig(Scratch.File("main2.jhm"), {
      {"include", TJson(TJson::TArray{TJson(string("inc3.jhm"))})}, {"x", TJson(99.0)}});

  TConfig conf(Scratch.File("main2.jhm"));
  EXPECT_EQ(conf.Read<double>({"x"}), 99.0);
}

FIXTURE(MissingParentThrows) {
  WriteConfig(Scratch.File("missing_parent.jhm"), {{"parent", TJson(string("does_not_exist.jhm"))}});

  EXPECT_THROW(runtime_error, [&] { TConfig conf(Scratch.File("missing_parent.jhm")); });
}

FIXTURE(ParentCycleThrows) {
  WriteConfig(Scratch.File("cycle_a.jhm"), {{"parent", TJson(string("cycle_b.jhm"))}});
  WriteConfig(Scratch.File("cycle_b.jhm"), {{"parent", TJson(string("cycle_a.jhm"))}});

  EXPECT_THROW(runtime_error, [&] { TConfig conf(Scratch.File("cycle_a.jhm")); });
}
