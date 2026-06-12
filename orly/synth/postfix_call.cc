/* <orly/synth/postfix_call.cc>

   Implements <orly/synth/postfix_call.h>.

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

#include <orly/synth/postfix_call.h>

#include <iomanip>

#include <base/as_str.h>
#include <base/assert_true.h>
#include <orly/error.h>
#include <orly/expr/function_app.h>
#include <orly/pos_range.h>
#include <orly/synth/context.h>
#include <orly/synth/cst_utils.h>
#include <orly/synth/get_pos_range.h>
#include <orly/synth/new_expr.h>

using namespace Orly;
using namespace Orly::Synth;

TPostfixCall::TPostfixCall(const TExprFactory *expr_factory, const Package::Syntax::TPostfixCall *postfix_call)
    : PostfixCall(Base::AssertTrue(postfix_call)) {
  class TOptCallArgsVisitor
      : public Package::Syntax::TOptCallArgs::TVisitor {
    NO_COPY(TOptCallArgsVisitor);
    public:
    TOptCallArgsVisitor(const TExprFactory *expr_factory, TArgMap &args)
        : Args(args), ExprFactory(expr_factory) {}
    virtual void operator()(const Package::Syntax::TExplicitCallArgs *that) const {
      ForEach<Package::Syntax::TObjMember>(that->GetObjMemberList(),
                                           [this](const Package::Syntax::TObjMember * obj_member)->bool {
            auto name = TName(obj_member->GetName());
            auto result = Args.insert(std::make_pair(name, ExprFactory->NewExpr(obj_member->GetExpr())));
            if (!result.second) {
              GetContext().AddError(name.GetPosRange(),
                                    Base::AsStr("Duplicate argument name ", std::quoted(name.GetText()),
                                    " first specified at ",
                                    result.first->first.GetPosRange()));
            }
            return true;
      });
    }
    virtual void operator()(const Package::Syntax::TNoCallArgs *) const { /* DO NOTHING */ }
    virtual void operator()(const Package::Syntax::TPositionalCallArgs *that) const {
      /* The `name.Tag(payload)` shape is claimed by TVariantCtorByName in
         the expr factory before TPostfixCall is ever constructed (#103),
         so a positional argument reaching here has a non-ctor callee. */
      throw TNotImplementedError(HERE, GetPosRange(that->GetExpr()),
          "A positional argument is only valid when constructing a variant through a type name");
    }
    virtual void operator()(const Package::Syntax::TUnrolledCallArgs *that) const {
      throw TNotImplementedError(HERE, GetPosRange(that->GetStar(), that->GetExpr()),
          "Unrolled call arguments are not yet supported");
    }
    private:
    TArgMap &Args;
    const TExprFactory *ExprFactory;
  };  // TOptCallArgsVisitor
  assert(expr_factory);
  try {
    Expr = expr_factory->NewExpr(PostfixCall->GetExpr());
    PostfixCall->GetOptCallArgs()->Accept(TOptCallArgsVisitor(expr_factory, Args));
  } catch (...) {
    Cleanup();
    throw;
  }
}

TPostfixCall::~TPostfixCall() {
  Cleanup();
}

Expr::TExpr::TPtr TPostfixCall::Build() const {
  Expr::TFunctionApp::TFunctionAppArgMap args;
  for (auto arg : Args) {
    auto result = args.insert(make_pair((arg.first).GetText(), Expr::TFunctionAppArg::New((arg.second)->Build())));
    assert(result.second);
  }
  return Expr::TFunctionApp::New(Expr->Build(), args, GetPosRange(PostfixCall));
}

void TPostfixCall::Cleanup() {
  delete Expr;
  for (auto arg : Args) {
    delete arg.second;
  }
}

void TPostfixCall::ForEachInnerScope(const std::function<void (TScope *)> &cb) {
  assert(cb);
  Expr->ForEachInnerScope(cb);
  for (auto arg : Args) {
    (arg.second)->ForEachInnerScope(cb);
  }
}

void TPostfixCall::ForEachRef(const std::function<void (TAnyRef &)> &cb) {
  assert(cb);
  Expr->ForEachRef(cb);
  for (auto arg : Args) {
    (arg.second)->ForEachRef(cb);
  }
}
