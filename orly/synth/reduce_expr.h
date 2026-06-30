/* <orly/synth/reduce_expr.h>

   Synth-layer node for `seq reduce start X + that` expressions.
   Holds the sequence (lhs) and the reduce body (rhs). Implements
   both `TStartableExpr` (so the body's `start` resolves here) and
   `TThatableExpr` (so the body's `that` resolves here). Lowers to
   `Expr::TReduce`.

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

#pragma once

#include <cassert>

#include <base/class_traits.h>
#include <orly/expr/reduce.h>
#include <orly/expr/thatable.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/startable_expr.h>
#include <orly/synth/thatable_expr.h>

namespace Orly {

  namespace Synth {

    class TExprFactory;

    class TReduceExpr
        : public TStartableExpr,
          public TThatableExpr {
      NO_COPY(TReduceExpr);
      public:

      TReduceExpr(const TExprFactory *expr_factory, const Package::Syntax::TInfixReduce *infix_reduce);

      virtual ~TReduceExpr();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      const Expr::TReduce::TPtr &GetSymbol() const;

      virtual Expr::TStartable::TPtr GetStartableSymbol() const;

      virtual Expr::TThatable::TPtr GetThatableSymbol() const;

      private:

      const Package::Syntax::TInfixReduce *InfixReduce;

      TExpr *Lhs;

      TExpr *Rhs;

      mutable Expr::TReduce::TPtr Symbol;

    };  // TReduceExpr

  }  // Synth

}  // Orly
