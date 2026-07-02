/* <orly/package/name.test.cc>

   Unit test for <orly/package/name.h>.

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

#include <orly/package/name.h>

#include <stdexcept>

#include <base/test/kit.h>

#include <base/as_str.h>
#include <base/syntax_error.h>

using namespace std;
using namespace Base;
using namespace Orly::Package;

/* Expected kinds of input:
   "scope/A.1"
   "scope/A"
   "A.1"
   "A"
*/

FIXTURE(VersionedNameWithScope) {
  const TVersionedName package = TVersionedName::Parse(AsPiece("scope/sample.1"));
  EXPECT_EQ(package.Name, (TName{{"scope", "sample"}}));
  EXPECT_EQ(package.Version, 1u);
  EXPECT_EQ(AsStr(package.GetSoRelPath()), "scope/sample.1.so");
}

FIXTURE(NameWithScope) {
  EXPECT_EQ(TName::Parse("scope/sample"), (TName{{"scope", "sample"}}));
}

FIXTURE(VersionedNameWithoutScope) {
  const TVersionedName package = TVersionedName::Parse(AsPiece("sample.1000"));
  EXPECT_TRUE(package.Name == TName{{"sample"}});
  EXPECT_EQ(package.Version, 1000u);
  EXPECT_EQ(AsStr(package.GetSoRelPath()), "sample.1000.so");
}

FIXTURE(UnversionedDefaultsToZero) {
  const TVersionedName package = TVersionedName::Parse(AsPiece("scope/sample"));
  EXPECT_EQ(package.Name, (TName{{"scope", "sample"}}));
  EXPECT_EQ(package.Version, 0u);
}

FIXTURE(DeepScope) {
  const TVersionedName package = TVersionedName::Parse(AsPiece("a/b/c.3"));
  EXPECT_EQ(package.Name, (TName{{"a", "b", "c"}}));
  EXPECT_EQ(package.Version, 3u);
  EXPECT_EQ(AsStr(package.GetSoRelPath()), "a/b/c.3.so");
}

FIXTURE(LastDotSplitsVersion) {
  /* The version is everything after the LAST dot. */
  const TVersionedName package = TVersionedName::Parse(AsPiece("scope/sam.ple.7"));
  EXPECT_EQ(package.Name, (TName{{"scope", "sam.ple"}}));
  EXPECT_EQ(package.Version, 7u);
}

FIXTURE(VersionedNameEquality) {
  const TVersionedName a = TVersionedName::Parse(AsPiece("scope/sample.1"));
  const TVersionedName b = TVersionedName::Parse(AsPiece("scope/sample.1"));
  const TVersionedName c = TVersionedName::Parse(AsPiece("scope/sample.2"));
  const TVersionedName d = TVersionedName::Parse(AsPiece("other/sample.1"));
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a == c);  // same name, different version
  EXPECT_FALSE(a == d);  // same version, different name
}

FIXTURE(InvalidNames) {
  /* An empty namespace component is invalid. */
  EXPECT_THROW(std::runtime_error, []() {
    TName::Parse("scope//sample");
  });
  EXPECT_THROW(std::runtime_error, []() {
    TName::Parse("/scope");
  });
  /* A non-numeric version is a syntax error. */
  EXPECT_THROW(Base::TSyntaxError, []() {
    TVersionedName::Parse(AsPiece("sample.x"));
  });
}
