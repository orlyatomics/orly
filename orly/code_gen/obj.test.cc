/* <orly/code_gen/obj.test.cc>

   Unit test for <orly/code_gen/obj.h>.

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

#include <orly/code_gen/obj.h>

#include <cstdlib>
#include <string>

#include <unistd.h>

#include <orly/type.h>
#include <orly/type/type_czar.h>

#include <base/test/kit.h>

using namespace Orly;
using namespace Orly::CodeGen;
using namespace Orly::Type;

FIXTURE(GenObjHeader) {
  TTypeCzar type_czar;

  /* Validity of the generated code is gated end-to-end by the lang_test suite, where
     every test compiles a full generated package with gcc (#308) -- re-proving it here
     would mean shelling out to the compiler from a unit test. */
  //A basic object
  auto obj1 = TObj::Get({{"a", TInt::Get()}, {"b", TBool::Get()}});
  GenObjHeader("/tmp/", obj1);

  //An object full of complex types.
  auto obj2 = TObj::Get({{"a", TDict::Get(TInt::Get(), TBool::Get())}, {"b", TSet::Get(TBool::Get())}, {"c", TList::Get(TInt::Get())}});
  GenObjHeader("/tmp/", obj2);

  //An object with more interesting variable
  auto obj3 = TObj::Get({{"mutable", obj2}, {"irtual", TInt::Get()}, {"float", TInt::Get()}});
  GenObjHeader("/tmp/", obj3);
}
FIXTURE(ObjHeaderNameShortKeepsMangledName) {
  TTypeCzar type_czar;
  auto obj = TObj::Get({{"a", TInt::Get()}, {"b", TBool::Get()}});
  EXPECT_EQ(ObjHeaderName(obj), obj.GetMangledName());
}

/* A nested type's mangled name outgrows a file name (#815). */
FIXTURE(ObjHeaderNameLongIsHashed) {
  TTypeCzar type_czar;
  TObj::TElems elems;
  for (int i = 0; i < 8; ++i) {
    elems["field_with_a_fairly_long_name_" + std::to_string(i)] = TInt::Get();
  }
  auto obj = TObj::Get(elems);
  EXPECT_GT(obj.GetMangledName().size(), size_t(255));
  const std::string name = ObjHeaderName(obj);
  EXPECT_EQ(name.size(), size_t(33));
  EXPECT_EQ(name[0], 'H');
  EXPECT_EQ(name, ObjHeaderName(obj));
  elems["field_with_a_fairly_long_name_0"] = TBool::Get();
  EXPECT_NE(name, ObjHeaderName(TObj::Get(elems)));

  char dir[] = "/tmp/orly_obj_header_XXXXXX";
  EXPECT_TRUE(mkdtemp(dir));
  const std::string out_dir = std::string(dir) + "/";
  GenObjHeader(out_dir, obj);
  EXPECT_EQ(access((out_dir + name + ".h").c_str(), R_OK), 0);
  unlink((out_dir + name + ".h").c_str());
  rmdir(dir);
}
