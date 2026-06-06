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
#include <orly/expr/where.h>
#include <orly/symbol/scope.h>
#include <orly/synth/cst_utils.h>
#include <orly/synth/func_def.h>
#include <orly/synth/get_pos_range.h>
#include <orly/synth/name.h>
#include <orly/synth/new_expr.h>
#include <orly/synth/obj_member_expr.h>
#include <orly/synth/scope_and_def.h>

using namespace Orly;
using namespace Orly::Synth;

namespace {

  /* The payload binder of a `Tag(n): body` arm: a zero-argument function
     def `n = operand.Tag`, so a reference to `n` in the arm body resolves
     (like any name) to the active payload. Reuses the whole TFuncDef build
     machinery; the body is supplied directly rather than parsed (the
     CST-less TFuncDef constructor). */
  class TWhenBinderDef final
      : public TFuncDef {
    NO_COPY(TWhenBinderDef);
    public:

    TWhenBinderDef(TScope *scope, const TName &name, TExpr *accessor, const TPosRange &pos_range)
        : TFuncDef(scope, name, pos_range) {
      SetExpr(accessor);
    }

  };  // TWhenBinderDef

  /* The local scope a `Tag(n): body` arm introduces. Structurally a
     stripped-down where-clause: it owns one TWhenBinderDef (binding the
     payload name) and the arm body, and lowers to an Expr::TWhere wrapping
     the body -- which Expr::TWhen / code_gen consume as an ordinary arm
     body, needing no variant-match-specific change. */
  class TWhenBinderScope final
      : public TExpr, public TScope {
    NO_COPY(TWhenBinderScope);
    public:

    TWhenBinderScope(
        const TExprFactory *expr_factory,
        const Package::Syntax::TExpr *operand,
        const TName &tag,
        const TName &binder,
        const Package::Syntax::TExpr *body,
        const TPosRange &pos_range)
          : TScope(Base::AssertTrue(expr_factory)->OuterScope),
            PosRange(pos_range),
            Body(nullptr) {
      /* The accessor `operand.Tag` is evaluated in the *enclosing* scope
         (its own refs -- e.g. the operand name -- resolve out there), so
         build it with the incoming factory. */
      TExpr *accessor = new TObjMemberExpr(expr_factory->NewExpr(operand), tag, pos_range);
      /* The binder def lives in *this* scope (it owns the def). */
      new TWhenBinderDef(this, binder, accessor, pos_range);
      /* The body sees this scope, so a use of the binder name resolves to
         the def above. */
      TExprFactory local_expr_factory = *expr_factory;
      local_expr_factory.OuterScope = this;
      Body = local_expr_factory.NewExpr(body);
    }

    ~TWhenBinderScope() {
      delete Body;
    }

    /* See TExpr. */
    virtual Expr::TExpr::TPtr Build() const override {
      assert(Symbol);
      Symbol->SetExpr(Body->Build());
      return Symbol;
    }

    /* See TScope. */
    virtual void BuildSymbol() override {
      assert(!Symbol);
      Symbol = Expr::TWhere::New(PosRange);
    }

    /* See TScope. */
    virtual Symbol::TScope::TPtr GetScopeSymbol() const override {
      assert(Symbol);
      return Symbol;
    }

    /* See TScope. */
    virtual bool HasSymbol() const override {
      return Symbol.get();
    }

    /* See TExpr. The binder scope is itself an inner scope. */
    virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb) override {
      assert(cb);
      cb(this);
      Body->ForEachInnerScope(cb);
    }

    private:

    /* See TScope. The body's refs are controlled by this scope (so a use of
       the binder name binds here); they are intentionally NOT exposed via
       TExpr::ForEachRef, so they don't leak to the enclosing scope. */
    virtual void ForEachControlledRef(const std::function<void (TAnyRef &)> &cb) const override {
      assert(cb);
      Body->ForEachRef(cb);
    }

    TPosRange PosRange;

    TExpr *Body;

    Expr::TWhere::TPtr Symbol;

  };  // TWhenBinderScope

}  // namespace

TWhenExpr::TWhenExpr(const TExprFactory *expr_factory, const Package::Syntax::TWhenExpr *when_expr)
    : WhenExpr(Base::AssertTrue(when_expr)), Operand(nullptr) {
  class TArmVisitor
      : public Package::Syntax::TWhenArm::TVisitor {
    NO_COPY(TArmVisitor);
    public:
    TArmVisitor(
        std::vector<std::string> &tags,
        std::vector<TExpr *> &bodies,
        const TExprFactory *expr_factory,
        const Package::Syntax::TExpr *operand)
        : Tags(tags), Bodies(bodies), ExprFactory(expr_factory), Operand(operand) {}
    virtual void operator()(const Package::Syntax::TBadWhenArm *) const { /* DO NOTHING */ }
    virtual void operator()(const Package::Syntax::TLabeledWhenArm *that) const {
      Tags.push_back(TName(that->GetName()).GetText());
      Bodies.push_back(ExprFactory->NewExpr(that->GetExpr()));
    }
    /* `Tag(n): body` -- the payload binder arm (#95). Same tag handling as a
       labeled arm, but the body is wrapped in a scope binding `n` to the
       active payload `operand.Tag`. */
    virtual void operator()(const Package::Syntax::TBinderWhenArm *that) const {
      Tags.push_back(TName(that->GetTag()).GetText());
      Bodies.push_back(new TWhenBinderScope(
          ExprFactory, Operand, TName(that->GetTag()), TName(that->GetBinder()),
          that->GetExpr(), GetPosRange(that->GetExpr())));
    }
    private:
    std::vector<std::string> &Tags;
    std::vector<TExpr *> &Bodies;
    const TExprFactory *ExprFactory;
    const Package::Syntax::TExpr *Operand;
  };  // TArmVisitor
  assert(expr_factory);
  try {
    Operand = expr_factory->NewExpr(WhenExpr->GetExpr());
    TArmVisitor visitor(Tags, Bodies, expr_factory, WhenExpr->GetExpr());
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
