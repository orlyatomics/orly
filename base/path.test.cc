/* <base/path.test.cc>

   Unit test for <base/path.h>.

   Replaces the old base/path.test.broken.cc, which targeted the TName /
   TAbsPath API that predated the simplification to the current TPath
   (#282).

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

#include <base/path.h>

#include <sstream>

#include <base/test/kit.h>

using namespace Base;
using namespace std;

using TStrList = TPath::TStrList;

static string ToStr(const TPath &path) {
  ostringstream strm;
  strm << path;
  return strm.str();
}

FIXTURE(ParseFull) {
  TPath path("/a/b/c.txt");
  EXPECT_TRUE(path.Namespace == TStrList({"a", "b"}));
  EXPECT_EQ(path.Name, "c");
  EXPECT_TRUE(path.Extension == TStrList({"txt"}));
  EXPECT_TRUE(bool(path));
}

FIXTURE(ParseBareName) {
  /* A bare name gets the empty last extension (the executable convention:
     it isn't printed). */
  TPath path("a");
  EXPECT_TRUE(path.Namespace.empty());
  EXPECT_EQ(path.Name, "a");
  EXPECT_TRUE(path.Extension == TStrList({""}));
}

FIXTURE(ParseMultiExtension) {
  TPath path("x.test.cc");
  EXPECT_EQ(path.Name, "x");
  EXPECT_TRUE(path.Extension == TStrList({"test", "cc"}));

  /* Adjacent dots are empty extensions. */
  TPath gaps("a..b");
  EXPECT_EQ(gaps.Name, "a");
  EXPECT_TRUE(gaps.Extension == TStrList({"", "b"}));
}

FIXTURE(ParseDotfile) {
  /* Names can be zero length (ex: .foobar). */
  TPath path("/etc/.conf");
  EXPECT_TRUE(path.Namespace == TStrList({"etc"}));
  EXPECT_EQ(path.Name, "");
  EXPECT_TRUE(path.Extension == TStrList({"conf"}));
}

FIXTURE(PieceConstructors) {
  TPath a(TStrList{"a", "b"}, "c", TStrList{"txt"});
  TPath b(TStrList{"a", "b", "c"}, TStrList{"txt"});
  TPath c("/a/b", "c", TStrList{"txt"});
  EXPECT_TRUE(a == b);
  EXPECT_TRUE(a == c);
  EXPECT_TRUE(a == TPath("/a/b/c.txt"));
  EXPECT_TRUE(a != TPath("/a/b/d.txt"));
}

FIXTURE(InvalidThrows) {
  /* A '/' in a name or a '.' or '/' reaching the name via the piece
     constructors makes the path invalid. */
  bool caught = false;
  try {
    TPath path(TStrList{"a"}, "b/c", TStrList{});
  } catch (const TPath::TInvalid &) {
    caught = true;
  }
  EXPECT_TRUE(caught);
  caught = false;
  try {
    TPath path(TStrList{"a", ""}, "b", TStrList{});
  } catch (const TPath::TInvalid &) {
    caught = true;
  }
  EXPECT_TRUE(caught);
}

FIXTURE(EndsWith) {
  TPath path("x.test.cc");
  EXPECT_TRUE(path.EndsWith({"cc"}));
  EXPECT_TRUE(path.EndsWith({"test", "cc"}));
  EXPECT_TRUE(path.EndsWith({}));
  EXPECT_FALSE(path.EndsWith({"test"}));
  EXPECT_FALSE(path.EndsWith({"h"}));
  EXPECT_FALSE(path.EndsWith({"x", "test", "cc"}));
}

FIXTURE(NamespaceIncludingName) {
  TPath path("/a/b/c.txt");
  EXPECT_TRUE(path.ToNamespaceIncludingName() == vector<string>({"a", "b", "c"}));
}

FIXTURE(Print) {
  /* Printing is absolute-style: leading slash, empty last extension
     suppressed. */
  EXPECT_EQ(ToStr(TPath("/a/b/c.txt")), "/a/b/c.txt");
  EXPECT_EQ(ToStr(TPath("a")), "/a");
  EXPECT_EQ(ToStr(TPath("x.test.cc")), "/x.test.cc");
  EXPECT_EQ(ToStr(TPath(TStrList{"bin"}, "orlyi", TStrList{""})), "/bin/orlyi");
}

FIXTURE(ExtensionHelpers) {
  EXPECT_EQ(ToStr(AddExtension(TPath("/a/b.c"), {"gz"})), "/a/b.c.gz");
  EXPECT_EQ(ToStr(DropExtension(TPath("/a/b.c.gz"), 1)), "/a/b.c");
  EXPECT_EQ(ToStr(SwapExtension(TPath("/a/b.c"), {"h"})), "/a/b.h");
  /* Swapping in the empty extension makes an executable-style name. */
  EXPECT_EQ(ToStr(SwapExtension(TPath("/a/b.c"), {""})), "/a/b");
}

FIXTURE(SplitNamespaceShapes) {
  EXPECT_TRUE(SplitNamespace("") == vector<string>());
  EXPECT_TRUE(SplitNamespace("a/b") == vector<string>({"a", "b"}));
  EXPECT_TRUE(SplitNamespace("/a/b") == vector<string>({"a", "b"}));
  EXPECT_TRUE(SplitNamespace("/a/b/") == vector<string>({"a", "b"}));
}

FIXTURE(WriteNamespaceShapes) {
  ostringstream a;
  WriteNamespace(a, {"x", "y"});
  EXPECT_EQ(a.str(), "/x/y/");
  ostringstream b;
  WriteNamespace(b, {}, false);
  EXPECT_EQ(b.str(), "");
  ostringstream c;
  WriteNamespace(c, {"x"}, false);
  EXPECT_EQ(c.str(), "x/");
}
