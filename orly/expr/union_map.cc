/* <orly/expr/union_map.cc>

   Implements <orly/expr/union_map.h>.

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

#include <orly/expr/union_map.h>

#include <orly/error.h>
#include <orly/expr/visitor.h>
#include <orly/pos_range.h>
#include <orly/type/seq.h>
#include <orly/type/set.h>
#include <orly/type/unwrap.h>
#include <orly/type/util.h>

using namespace Orly;
using namespace Orly::Expr;

TUnionMap::TPtr TUnionMap::New(
    const TExpr::TPtr &lhs,
    const TPosRange &pos_range) {
  return TUnionMap::TPtr(new TUnionMap(lhs, pos_range));
}

TUnionMap::TUnionMap(
    const TExpr::TPtr &lhs,
    const TPosRange &pos_range)
      : TThatableBinary(lhs, pos_range) {}

void TUnionMap::Accept(const TVisitor &visitor) const {
  visitor(this);
}

Type::TType TUnionMap::GetTypeImpl() const {
  if (!GetLhs()->GetType().Is<Type::TSeq>()) {
    throw TExprError(HERE, GetPosRange(),
        "The lhs of a union_map expression must be a sequence");
  }
  assert(GetRhs());
  /* The per-element body maps `that` to a set; the whole expression is the
     union of those sets, so its type IS that set type. */
  Type::TType elem_type = Type::Unwrap(GetRhs()->GetType());
  if (!elem_type.Is<Type::TSet>()) {
    throw TExprError(HERE, GetPosRange(),
        "The rhs of a union_map expression must be a set (it is unioned across the sequence)");
  }
  return elem_type;
}
