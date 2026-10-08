/* <orly/expr/keys.cc>

   Implements <orly/expr/keys.h>

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

#include <orly/expr/keys.h>

#include <sstream>

#include <base/as_str.h>
#include <orly/error.h>
#include <orly/expr/visitor.h>
#include <orly/type/addr.h>
#include <orly/type/any.h>
#include <orly/type/orlyify.h>
#include <orly/type/seq.h>
#include <orly/type/util.h>

using namespace Orly;
using namespace Orly::Expr;

TKeys::TPtr TKeys::New(const TAddr::TMemberVec &members, const Type::TType &value_type, const TPosRange &pos_range,
                       const TExpr::TPtr &bound, bool bound_is_inclusive) {
  return TKeys::TPtr(new TKeys(members, value_type, pos_range, bound, bound_is_inclusive));
}

TKeys::TKeys(const TAddr::TMemberVec &members, const Type::TType &value_type, const TPosRange &pos_range,
             const TExpr::TPtr &bound, bool bound_is_inclusive)
    : TNAry(members, pos_range), ValueType(value_type), Bound(bound), BoundIsInclusive(bound_is_inclusive) {
  if (Bound) {
    Bound->SetExprParent(this);
  }
}

TKeys::~TKeys() {
  if (Bound) {
    Bound->UnsetExprParent(this);
  }
}

void TKeys::Accept(const TVisitor &visitor) const {
  visitor(this);
}

Type::TType TKeys::GetAddrType() const {
  Type::TAddr::TElems elems;
  bool is_sequence = false;
  for (auto member : GetMembers()) {
    elems.push_back(std::make_pair(member.first, (member.second)->GetType()));
    is_sequence |= member.second->GetType().Is<Type::TSeq>();
  }
  Type::TType type = Type::TAddr::Get(elems);
  if (is_sequence) {
    type = Type::TSeq::Get(type);
  }
  return type;
}

Type::TType TKeys::GetTypeImpl() const {
  Type::TType addr_type = GetAddrType();
  if (Bound) {
    /* The bound is a whole key of the pattern's shape (#735): same arity,
       same element types, same asc/desc on each element. Anything else could
       not be compared with the index's keys, so refuse it here rather than
       let it reach the walk. */
    const char *keyword = BoundIsInclusive ? "from" : "after";
    if (addr_type.Is<Type::TSeq>()) {
      throw TExprError(HERE, Bound->GetPosRange(),
          Base::AsStr("a keys bound (`", keyword, "`) needs a pattern of single values, not sequences").c_str());
    }
    Type::TType bound_type = Bound->GetType();
    /* A TAny is an as-yet-unresolved recursive call; defer, as function
       arguments do (#128). */
    if (!bound_type.Is<Type::TAny>() && bound_type != addr_type) {
      std::ostringstream bound_name, addr_name;
      Type::Orlyify(bound_name, bound_type);
      Type::Orlyify(addr_name, addr_type);
      throw TExprError(HERE, Bound->GetPosRange(),
          Base::AsStr("the `", keyword, "` bound has type ", bound_name.str(),
                      ", but this pattern's keys have type ", addr_name.str(),
                      "; the bound must be a whole key of the same shape, such as the last key of the previous page").c_str());
    }
  }
  return Type::TSeq::Get(addr_type);
}