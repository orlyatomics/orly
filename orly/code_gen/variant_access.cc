/* <orly/code_gen/variant_access.cc>

   Implements <orly/code_gen/variant_access.h>.

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

#include <orly/code_gen/variant_access.h>

using namespace Orly;
using namespace Orly::CodeGen;

TVariantIs::TVariantIs(const L0::TPackage *package,
                       const Type::TType &type,
                       const TInline::TPtr &operand,
                       size_t which)
    : TInline(package, type), Operand(operand), Which(which) {}

void TVariantIs::WriteExpr(TCppPrinter &out) const {
  /* GetWhich() returns the active arm's asciibetical index; the predicate
     is true iff it equals this tag's index. */
  out << "((" << Operand << ").GetWhich() == " << Which << ')';
}

TVariantMember::TVariantMember(const L0::TPackage *package,
                               const Type::TType &type,
                               const TInline::TPtr &operand,
                               const std::string &tag)
    : TInline(package, type), Operand(operand), Tag(tag) {}

void TVariantMember::WriteExpr(TCppPrinter &out) const {
  /* GetV<Tag>() returns the active arm's payload and asserts the arm is
     active -- callers gate this with `is <Tag>`. */
  out << '(' << Operand << ").GetV" << Tag << "()";
}
