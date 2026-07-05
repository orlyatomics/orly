/* <orly/expr/walker.test.cc>

   This is intended to test the ForEachExpr to make sure it is walking the entire tree.
   Notably, does it collect all the objects that may need to be created?

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

#include <orly/orly.package.cst.h>

#include <orly/compiler.h>
#include <orly/expr/walker.h>
#include <orly/synth/package.h>
#include <orly/symbol/package.h>
#include <orly/symbol/scope.h>
#include <orly/type/object_collector.h>
#include <orly/type/type_czar.h>

#include <base/test/kit.h>

using namespace Orly::Package::Syntax;
using namespace Orly;
using namespace std;

static Type::TTypeCzar type_czar;

/* Helper function turns a string into a TPackage (by reading in the concrete syntax tree)

NOTE: Copied in <orly/expr/addr_walker.test.cc> */

Symbol::TPackage::TPtr MakePackage(const string &orlyscript) {
  auto cst = TPackage::ParseStr(orlyscript.c_str());
  if (cst.HasErrors()) {
    //Syntax error!
    cst.PrintErrors(std::cout);
    throw std::invalid_argument("Orlyscript has syntax errors");
  }
  return (new Orly::Synth::TPackage({{"walker_test"}}, cst.Get(), false))->GetSymbol();
}

/* Given a Orlyscript string, collects all of the objects used in the script */
void CollectObjects(const string &orlyscript, unordered_set<Orly::Type::TType> &objects) {
  Symbol::TPackage::TPtr my_package = MakePackage(orlyscript);
  assert(my_package);
  for (const auto &func: my_package->GetFunctions()) {
    Orly::Expr::TExpr::TPtr expr = func->GetExpr();
    Expr::ForEachExpr(expr, [&objects](const Expr::TExpr::TPtr &expr) {
      Orly::Type::CollectObjects(expr->GetType(), objects);
      return false;
      }, true);
  };
}

FIXTURE(Simple) {
  string simplescript =
    "write_obj = ((true) effecting {"
    "     new <[name]> <- <{ .foo:1 }>;"
    "   }) where { "
    "     name = given::(str);"
    "   };"
    "my_obj is <{ .foo: str }>;"
    "read_obj = (*<[name]>::(my_obj)) where {"
    "  name = given::(str);"
    "};";
  unordered_set<Orly::Type::TType> objects;
  CollectObjects(simplescript, objects);
  EXPECT_EQ(objects.size(), 2U);
}

FIXTURE(EmbeddedObjs) {
  string sscript =
    "George = {1943-02-25T0:0:0Z};"
    "George_as_obj = time_obj George;"
    "person_obj is <{ .name: str, .birthday: time_pnt }>;"
    "write_person = ((true) effecting {"
    "  new <[\"name\", person.name]> <- <{ .name:person.name, .birthday:person.birthday }>;"
    "  new <[\"birthmonth\", person.birthday]> <- <{ .name:person.name, .month:(time_obj person.birthday).month }>;"
    "}) where {"
    "   person = given::(person_obj);"
 "};";
  unordered_set<Orly::Type::TType> objects;
  CollectObjects(sscript, objects);
  EXPECT_EQ(objects.size(), 3U);
}

/* Re-enabled for #282: rewritten from the 2014 draft into valid orlyscript
   (keys needs the value type and an @ before the address pattern, sequences
   come from reduce or the ** unroll, and the histogram maps a function over
   the unrolled name set). */
FIXTURE(PeopledirHistogram) {
  string histogram =
    "namepair is <{.fname: str, .count: int}>;"
    "find_by_first_name = (((keys (int) @ <[fname, free::(str)]>) reduce start (empty [str]) + [that.1])) where {"
    "  fname = given::(str);"
    "};"
    "first_names = ((keys (int) @ <[free::(str), free::(str)]>) reduce start (empty {str}) | {that.0});"
    "name_with_count = (<{.fname: fname, .count: length_of find_by_first_name(.fname: fname)}>) where {"
    "  fname = given::(str);"
    "};"
    "histogram = (name_with_count(.fname: **first_names) as [namepair] sorted_by lhs.count > rhs.count);";
  unordered_set<Orly::Type::TType> objects;
  CollectObjects(histogram, objects);
  EXPECT_EQ(objects.size(), 1U);
}