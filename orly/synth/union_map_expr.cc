/* <orly/synth/union_map_expr.cc>

   Implements <orly/synth/union_map_expr.h>.

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

#include <orly/synth/union_map_expr.h>

#include <cassert>

#include <base/assert_true.h>
#include <orly/synth/get_pos_range.h>
#include <orly/synth/new_expr.h>

using namespace Orly;
using namespace Orly::Synth;

TUnionMapExpr::TUnionMapExpr(const TExprFactory *expr_factory, const Package::Syntax::TUnionMapExpr *union_map_expr)
    : UnionMapExpr(Base::AssertTrue(union_map_expr)), Lhs(nullptr), Rhs(nullptr) {
  assert(expr_factory);
  try {
    Lhs = expr_factory->NewExpr(UnionMapExpr->GetSeq());
    TExprFactory local_expr_factory = *expr_factory;
    local_expr_factory.ThatableExpr = this;
    Rhs = local_expr_factory.NewExpr(UnionMapExpr->GetElem());
  } catch (...) {
    delete Lhs;
    delete Rhs;
    throw;
  }
}

TUnionMapExpr::~TUnionMapExpr() {
  delete Lhs;
  delete Rhs;
}

Expr::TExpr::TPtr TUnionMapExpr::Build() const {
  assert(!Symbol);
  // NOTE: rhs is set separately so the inner `that` can resolve to this
  // symbol -- see <orly/synth/thatable_expr.h>.
  Symbol = Expr::TUnionMap::New(Lhs->Build(), GetPosRange(UnionMapExpr));
  Symbol->SetRhs(Rhs->Build());
  return Symbol;
}

void TUnionMapExpr::ForEachInnerScope(const std::function<void (TScope *)> &cb) {
  assert(cb);
  Lhs->ForEachInnerScope(cb);
  Rhs->ForEachInnerScope(cb);
}

void TUnionMapExpr::ForEachRef(const std::function<void (TAnyRef &)> &cb) {
  assert(cb);
  Lhs->ForEachRef(cb);
  Rhs->ForEachRef(cb);
}

const Expr::TUnionMap::TPtr &TUnionMapExpr::GetSymbol() const {
  assert(Symbol);
  return Symbol;
}

Expr::TThatable::TPtr TUnionMapExpr::GetThatableSymbol() const {
  assert(Symbol);
  return Symbol;
}
