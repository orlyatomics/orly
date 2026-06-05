/* <orly/synth/when_expr.cc>

   Implements <orly/synth/when_expr.h>.

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

#include <orly/synth/when_expr.h>

#include <cassert>

#include <base/assert_true.h>
#include <orly/expr/when.h>
#include <orly/synth/cst_utils.h>
#include <orly/synth/get_pos_range.h>
#include <orly/synth/new_expr.h>

using namespace Orly;
using namespace Orly::Synth;

TWhenExpr::TWhenExpr(const TExprFactory *expr_factory, const Package::Syntax::TWhenExpr *when_expr)
    : WhenExpr(Base::AssertTrue(when_expr)), Operand(nullptr) {
  class TArmVisitor
      : public Package::Syntax::TWhenArm::TVisitor {
    NO_COPY(TArmVisitor);
    public:
    TArmVisitor(std::vector<std::string> &tags, std::vector<TExpr *> &bodies, const TExprFactory *expr_factory)
        : Tags(tags), Bodies(bodies), ExprFactory(expr_factory) {}
    virtual void operator()(const Package::Syntax::TBadWhenArm *) const { /* DO NOTHING */ }
    virtual void operator()(const Package::Syntax::TLabeledWhenArm *that) const {
      Tags.push_back(TName(that->GetName()).GetText());
      Bodies.push_back(ExprFactory->NewExpr(that->GetExpr()));
    }
    private:
    std::vector<std::string> &Tags;
    std::vector<TExpr *> &Bodies;
    const TExprFactory *ExprFactory;
  };  // TArmVisitor
  assert(expr_factory);
  try {
    Operand = expr_factory->NewExpr(WhenExpr->GetExpr());
    TArmVisitor visitor(Tags, Bodies, expr_factory);
    ForEach<Package::Syntax::TWhenArm>(WhenExpr->GetOptWhenArmSeq(),
        [&visitor](const Package::Syntax::TWhenArm *arm) -> bool {
          arm->Accept(visitor);
          return true;
        });
  } catch (...) {
    Cleanup();
    throw;
  }
}

TWhenExpr::~TWhenExpr() {
  Cleanup();
}

void TWhenExpr::Cleanup() {
  delete Operand;
  for (auto *body : Bodies) {
    delete body;
  }
}

Expr::TExpr::TPtr TWhenExpr::Build() const {
  Expr::TWhen::TExprVec bodies;
  bodies.reserve(Bodies.size());
  for (auto *body : Bodies) {
    bodies.push_back(body->Build());
  }
  return Expr::TWhen::New(Operand->Build(), Tags, bodies, GetPosRange(WhenExpr));
}

void TWhenExpr::ForEachInnerScope(const std::function<void (TScope *)> &cb) {
  assert(cb);
  Operand->ForEachInnerScope(cb);
  for (auto *body : Bodies) {
    body->ForEachInnerScope(cb);
  }
}

void TWhenExpr::ForEachRef(const std::function<void (TAnyRef &)> &cb) {
  assert(cb);
  Operand->ForEachRef(cb);
  for (auto *body : Bodies) {
    body->ForEachRef(cb);
  }
}
