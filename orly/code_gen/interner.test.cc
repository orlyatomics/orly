/* <orly/code_gen/interner.test.cc>

   Unit test for <orly/code_gen/interner.h>

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

#include <orly/code_gen/interner.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include <orly/code_gen/binary.h>
#include <orly/code_gen/cpp_printer.h>
#include <orly/code_gen/literal.h>
#include <orly/type.h>
#include <orly/type/type_czar.h>

#include <base/test/kit.h>

using namespace Orly;
using namespace Orly::CodeGen;

FIXTURE(Typical) {
  Type::TTypeCzar type_czar;
  TIdScope::TPtr id_s = TIdScope::New();
  TCodeScope s(id_s);

  auto &Interner = *(s.GetInterner());

  auto lit1 = Interner.GetLiteral(nullptr, Var::TVar(1));
  auto lit2 = Interner.GetLiteral(nullptr, Var::TVar(2));
  auto lit3 = Interner.GetLiteral(nullptr, Var::TVar(3));
  auto lit4 = Interner.GetLiteral(nullptr, Var::TVar(4));
  auto lit4a = Interner.GetLiteral(nullptr, Var::TVar(4));

  auto bin1 = Interner.GetBinary(nullptr, Type::TInt::Get(), TBinary::Add, lit1, lit4);
  auto bin2 = Interner.GetBinary(nullptr, Type::TInt::Get(), TBinary::Add, lit3, lit2);
  auto bin3 = Interner.GetBinary(nullptr, Type::TInt::Get(), TBinary::Sub, lit1, lit4);
  auto bin4 = Interner.GetBinary(nullptr, Type::TInt::Get(), TBinary::Add, lit1, lit4);

  EXPECT_EQ(lit4, lit4a);
  EXPECT_EQ(bin1, bin4);
  EXPECT_NE(bin1, bin2);
  EXPECT_NE(bin1, bin3);
  EXPECT_NE(bin2, bin3);
  EXPECT_NE(bin2, bin4);
  EXPECT_NE(bin3, bin4);
  /* CSE is live again (#297): the duplicate fetch of bin1 made it a local, while the duplicate
     literal fetch did not (leaves whose emission is already a constant are excluded). */
  EXPECT_EQ(s.GetLocals().size(), 1u);
  EXPECT_TRUE(bin1->HasId());
  EXPECT_FALSE(lit4->HasId());

}

/* Print the locals through WriteStart and pin the emitted shape (#308): one `auto <id> = ...;`
   definition per local, inner definition first (dependency order), and the outer local's
   expression referring to the inner one by its id rather than re-expanding it. */
FIXTURE(WriteStartPrintsLocalsInDependencyOrder) {
  Type::TTypeCzar type_czar;
  TIdScope::TPtr id_s = TIdScope::New();
  TCodeScope s(id_s);
  auto &Interner = *(s.GetInterner());

  auto lit1 = Interner.GetLiteral(nullptr, Var::TVar(1));
  auto lit2 = Interner.GetLiteral(nullptr, Var::TVar(2));
  auto lit3 = Interner.GetLiteral(nullptr, Var::TVar(3));
  auto inner = Interner.GetBinary(nullptr, Type::TInt::Get(), TBinary::Add, lit1, lit2);
  auto outer = Interner.GetBinary(nullptr, Type::TInt::Get(), TBinary::Mult, inner, lit3);
  /* Duplicate outer first, then inner: arrival order inverts dependency order. */
  Interner.GetBinary(nullptr, Type::TInt::Get(), TBinary::Mult, inner, lit3);
  Interner.GetBinary(nullptr, Type::TInt::Get(), TBinary::Add, lit1, lit2);

  const std::string path = std::string(TEST_OUTPUT_DIR) + "interner_write_start.cc";
  /* printer scope so the file flushes */ {
    TCppPrinter out(path);
    s.WriteStart(out);
  }
  std::ostringstream buf;
  buf << std::ifstream(path).rdbuf();
  std::remove(path.c_str());
  const std::string text = buf.str();

  /* Two definitions, in dependency order: the first must be the inner one (its expression
     holds the literals 1 and 2, not 3), and the second must reference the first's variable
     by name instead of re-expanding the shared subexpression. */
  const auto first_at = text.find("auto ");
  EXPECT_TRUE(first_at != std::string::npos);
  const auto second_at = text.find("auto ", first_at + 1);
  EXPECT_TRUE(second_at != std::string::npos);
  const std::string first_line = text.substr(first_at, text.find('\n', first_at) - first_at);
  const std::string second_line = text.substr(second_at, text.find('\n', second_at) - second_at);
  EXPECT_TRUE(first_line.find('1') != std::string::npos);
  EXPECT_TRUE(first_line.find('2') != std::string::npos);
  EXPECT_TRUE(first_line.find('3') == std::string::npos);
  const std::string first_var = first_line.substr(5, first_line.find(" = ") - 5);
  EXPECT_TRUE(!first_var.empty());
  EXPECT_TRUE(second_line.find(first_var) != std::string::npos);
}

/* The inversion that kept CSE disabled (#297): when a larger expression is CSE'd before one of
   its own subexpressions is, arrival order would emit the outer local before the inner one it
   references.  OrderedLocals must put the inner definition first. */
FIXTURE(CseLocalsEmitInDependencyOrder) {
  Type::TTypeCzar type_czar;
  TIdScope::TPtr id_s = TIdScope::New();
  TCodeScope s(id_s);

  auto &Interner = *(s.GetInterner());

  auto lit1 = Interner.GetLiteral(nullptr, Var::TVar(1));
  auto lit2 = Interner.GetLiteral(nullptr, Var::TVar(2));
  auto lit3 = Interner.GetLiteral(nullptr, Var::TVar(3));

  auto inner = Interner.GetBinary(nullptr, Type::TInt::Get(), TBinary::Add, lit1, lit2);
  auto outer = Interner.GetBinary(nullptr, Type::TInt::Get(), TBinary::Mult, inner, lit3);

  /* Duplicate the OUTER expression first, then the INNER one: arrival order is now the
     reverse of dependency order. */
  auto outer_dup = Interner.GetBinary(nullptr, Type::TInt::Get(), TBinary::Mult, inner, lit3);
  auto inner_dup = Interner.GetBinary(nullptr, Type::TInt::Get(), TBinary::Add, lit1, lit2);

  EXPECT_EQ(outer, outer_dup);
  EXPECT_EQ(inner, inner_dup);
  EXPECT_EQ(s.GetLocals().size(), 2u);
  EXPECT_TRUE(s.GetLocals()[0] == outer);  // arrival order: outer first...
  auto ordered = s.OrderedLocals();
  EXPECT_EQ(ordered.size(), 2u);
  EXPECT_TRUE(ordered[0] == inner);        // ...but emission order defines inner first.
  EXPECT_TRUE(ordered[1] == outer);
}