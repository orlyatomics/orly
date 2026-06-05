/* <orly/synth/postfix_is_variant.cc>

   Implements <orly/synth/postfix_is_variant.h>.

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

#include <orly/synth/postfix_is_variant.h>

#include <base/assert_true.h>
#include <orly/expr/variant_is.h>
#include <orly/pos_range.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/get_pos_range.h>
#include <orly/synth/name.h>
#include <orly/synth/new_expr.h>

using namespace Orly;
using namespace Orly::Synth;

TPostfixIsVariant::TPostfixIsVariant(const TExprFactory *expr_factory, const Package::Syntax::TPostfixIsVariant *postfix_is_variant)
    : PostfixIsVariant(Base::AssertTrue(postfix_is_variant)),
      Expr(Base::AssertTrue(expr_factory)->NewExpr(PostfixIsVariant->GetExpr())) {}

TPostfixIsVariant::~TPostfixIsVariant() {
  delete Expr;
}

Expr::TExpr::TPtr TPostfixIsVariant::Build() const {
  return Expr::TVariantIs::New(Expr->Build(),
                               TName(PostfixIsVariant->GetName()).GetText(),
                               GetPosRange(PostfixIsVariant));
}

void TPostfixIsVariant::ForEachInnerScope(const std::function<void (TScope *cb)> &cb) {
  assert(cb);
  Expr->ForEachInnerScope(cb);
}

void TPostfixIsVariant::ForEachRef(const std::function<void (TAnyRef &cb)> &cb) {
  assert(cb);
  Expr->ForEachRef(cb);
}
