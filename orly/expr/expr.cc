/* <orly/expr/expr.cc>

   Implements <orly/expr/expr.h>

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

#include <orly/expr/expr.h>

#include <orly/type/unroll.h>
#include <orly/type/util.h>

using namespace Orly;
using namespace Orly::Expr;

TExpr::TExpr(const TPosRange &pos_range)
    : ExprParent(nullptr), PosRange(pos_range) {}

const TExprParent *TExpr::GetExprParent() const {
  assert(ExprParent);
  return ExprParent;
}

Type::TType TExpr::GetType() const {
  if (CachedType) {
    return *CachedType;
  }

  auto type = GetTypeImpl();
  /* Don't memoize a type that still contains TAny anywhere -- TAny is the
     unresolved recursive-call placeholder (TFunction::GetReturnType), so such
     a type is provisional. Caching it would freeze the placeholder in (e.g. a
     record `<{.next: TAny}>` built from a recursive result), and codegen would
     later read the stale TAny and fail to emit it. Leaving it uncached makes
     the expression recompute once the recursion resolves to a concrete type
     (by codegen). Generalizes the old `!Is<TAny>` guard to compound types
     (#128 Option A's caching note; needed for #104 recursive-variant widening
     folds). */
  if (!Type::HasAny(type)) {
    assert (!CachedType || *CachedType == type);
    CachedType = type;
  }
  return type;
}

void TExpr::SetExprParent(const TExprParent *expr_parent) {
  assert(expr_parent);
  assert(!ExprParent);
  ExprParent = expr_parent;
}

const TPosRange &TExpr::GetPosRange() const {
  return PosRange;
}

void TExpr::UnsetExprParent(const TExprParent *expr_parent) {
  assert(expr_parent);
  assert(ExprParent == expr_parent);
  ExprParent = nullptr;
}
