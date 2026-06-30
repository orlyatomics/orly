/* <orly/synth/filter_expr.h>

   Synth-layer node for `seq if (pred)` filter expressions. Holds
   the sequence and the predicate body; implements `TThatableExpr`
   for the predicate's `that` reference. Lowers to `Expr::TFilter`.

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
#include <orly/expr/filter.h>
#include <orly/expr/thatable.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/thatable_expr.h>

namespace Orly {

  namespace Synth {

    class TExprFactory;

    class TFilterExpr
        : public TThatableExpr {
      NO_COPY(TFilterExpr);
      public:

      TFilterExpr(const TExprFactory *expr_factory, const Package::Syntax::TInfixFilter *infix_filter);

      virtual ~TFilterExpr();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      const Expr::TFilter::TPtr &GetSymbol() const;

      virtual Expr::TThatable::TPtr GetThatableSymbol() const;

      private:

      const Package::Syntax::TInfixFilter *InfixFilter;

      TExpr *Lhs;

      TExpr *Rhs;

      mutable Expr::TFilter::TPtr Symbol;

    };  // TFilterExpr

  }  // Synth

}  // Orly
