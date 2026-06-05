/* <orly/expr/variant.cc>

   Implements <orly/expr/variant.h>

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

#include <orly/expr/variant.h>

#include <base/as_str.h>
#include <orly/error.h>
#include <orly/type/unwrap.h>
#include <orly/type/util.h>
#include <orly/type/variant.h>

using namespace Orly;
using namespace Orly::Expr;

TVariantCtor::TPtr TVariantCtor::New(const std::string &tag,
                                     const TExpr::TPtr &payload,
                                     const Type::TType &variant_type,
                                     const TPosRange &pos_range) {
  return TVariantCtor::TPtr(new TVariantCtor(tag, payload, variant_type, pos_range));
}

TVariantCtor::TVariantCtor(const std::string &tag,
                           const TExpr::TPtr &payload,
                           const Type::TType &variant_type,
                           const TPosRange &pos_range)
    : TUnary(payload, pos_range), Tag(tag), VariantType(variant_type) {}

void TVariantCtor::Accept(const TVisitor &visitor) const {
  visitor(this);
}

Type::TType TVariantCtor::GetTypeImpl() const {
  const Type::TVariant *variant = VariantType.TryAs<Type::TVariant>();
  if (!variant) {
    throw TExprError(HERE, GetPosRange(), "variant constructor's annotation is not a variant type");
  }
  const Type::TVariantElems &elems = variant->GetElems();
  auto iter = elems.find(Tag);
  if (iter == elems.end()) {
    std::string msg = Base::AsStr("variant constructor tag \"", Tag, "\" is not an arm of the declared variant type");
    throw TExprError(HERE, GetPosRange(), msg.c_str());
  }
  /* The payload expression's type must match the declared arm payload
     type. (A tag-only arm's declared payload is the empty object and the
     synth layer supplies an empty-object payload expression, so this check
     covers both cases uniformly.) */
  Type::TType payload_type = Type::Unwrap(GetExpr()->GetType());
  if (payload_type != iter->second) {
    std::string msg = Base::AsStr("variant constructor payload type does not match the declared type of arm \"", Tag, "\"");
    throw TExprError(HERE, GetPosRange(), msg.c_str());
  }
  return VariantType;
}
