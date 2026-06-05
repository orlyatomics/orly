/* <orly/expr/variant_is.cc>

   Implements <orly/expr/variant_is.h>

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

#include <orly/expr/variant_is.h>

#include <base/as_str.h>
#include <orly/error.h>
#include <orly/expr/visitor.h>
#include <orly/type.h>
#include <orly/type/impl.h>
#include <orly/type/unwrap.h>
#include <orly/type/variant.h>

using namespace Orly;
using namespace Orly::Expr;

TVariantIs::TPtr TVariantIs::New(const TExpr::TPtr &expr, const std::string &tag, const TPosRange &pos_range) {
  return TVariantIs::TPtr(new TVariantIs(expr, tag, pos_range));
}

TVariantIs::TVariantIs(const TExpr::TPtr &expr, const std::string &tag, const TPosRange &pos_range)
    : TUnary(expr, pos_range), Tag(tag) {}

void TVariantIs::Accept(const TVisitor &visitor) const {
  visitor(this);
}

size_t TVariantIs::GetWhich() const {
  const Type::TVariant *variant = Type::Unwrap(GetExpr()->GetType()).TryAs<Type::TVariant>();
  if (!variant) {
    throw TExprError(HERE, GetPosRange(), "`is <Tag>` operand is not a variant type");
  }
  const Type::TVariantElems &elems = variant->GetElems();
  size_t idx = 0;
  for (const auto &elem : elems) {
    if (elem.first == Tag) {
      return idx;
    }
    ++idx;
  }
  std::string msg = Base::AsStr("`is ", Tag, "` names a tag that is not an arm of the operand variant type");
  throw TExprError(HERE, GetPosRange(), msg.c_str());
}

Type::TType TVariantIs::GetTypeImpl() const {
  /* Validate operand is a variant whose tags include Tag (GetWhich does
     both checks and throws on failure); the predicate is always bool. */
  GetWhich();
  return Type::TBool::Get();
}
