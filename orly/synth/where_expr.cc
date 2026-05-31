/* <orly/synth/where_expr.cc>

   Implements <orly/synth/where_expr.h>.

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

#include <orly/synth/where_expr.h>

#include <base/as_str.h>
#include <base/assert_true.h>
#include <orly/synth/context.h>
#include <orly/synth/get_pos_range.h>
#include <orly/synth/new_expr.h>

using namespace Orly;
using namespace Orly::Synth;

TWhereExpr::TWhereExpr(
    const TExprFactory *expr_factory,
    const Package::Syntax::TWhereExpr *where_expr)
      : TScope(Base::AssertTrue(expr_factory)->OuterScope),
        WhereExpr(Base::AssertTrue(where_expr)) {
  TExprFactory local_expr_factory = *expr_factory;
  local_expr_factory.OuterScope = this;
  TLocalDefFactory::NewDefs(&local_expr_factory, WhereExpr->GetOptDefSeq());
  Expr = local_expr_factory.NewExpr(WhereExpr->GetExpr());
}

TWhereExpr::~TWhereExpr() {
  delete Expr;
}

Expr::TExpr::TPtr TWhereExpr::Build() const {
  assert(Expr);
  Symbol->SetExpr(Expr->Build());
  return Symbol;
}

void TWhereExpr::BuildSymbol() {
  assert(!Symbol);
  Symbol = Expr::TWhere::New(GetPosRange(WhereExpr));
}

void TWhereExpr::ForEachInnerScope(const std::function<void (TScope *)> &cb) {
  assert(&cb);
  assert(cb);
  cb(this);
  Expr->ForEachInnerScope(cb);
}

void TWhereExpr::ForEachControlledRef(const std::function<void (TAnyRef &)> &cb) const {
  assert(&cb);
  assert(cb);
  Expr->ForEachRef(cb);
}

Symbol::TScope::TPtr TWhereExpr::GetScopeSymbol() const {
  assert(Symbol);
  return Symbol;
}

Expr::TWhere::TPtr TWhereExpr::GetSymbol() const {
  assert(Symbol);
  return Symbol;
}

bool TWhereExpr::HasSymbol() const {
  return Symbol.get();
}

void TWhereExpr::TLocalDefFactory::NewDefs(
    const TExprFactory *expr_factory,
    const Package::Syntax::TOptDefSeq *opt_def_seq) {
  TWhereExpr::TLocalDefFactory(expr_factory).TDefFactory::NewDefs(opt_def_seq);
}

TWhereExpr::TLocalDefFactory::TLocalDefFactory(const TExprFactory *expr_factory)
    : TDefFactory(expr_factory) {}

void TWhereExpr::TLocalDefFactory::operator()(const Package::Syntax::TInstallerDef *that) const {
  OnTopLevel("an installer", GetPosRange(that));
}

void TWhereExpr::TLocalDefFactory::operator()(const Package::Syntax::TUninstallerDef *that) const {
  OnTopLevel("an uninstaller", GetPosRange(that));
}

void TWhereExpr::TLocalDefFactory::operator()(const Package::Syntax::TUpgraderDef *that) const {
  OnTopLevel("an upgrader", GetPosRange(that));
}

void TWhereExpr::TLocalDefFactory::OnTopLevel(const char *desc, const TPosRange &pos_range) const {
  assert(desc);
  assert(&pos_range);

  GetContext().AddError(pos_range, Base::AsStr(desc," is not allowed within a where clause"));
}
