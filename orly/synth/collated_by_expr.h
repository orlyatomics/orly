/* <orly/synth/collated_by_expr.h>

   Synth-layer node for `collated_by ... having ...`. Holds the
   sequence, start, reduce body, and having body. Implements both
   `TStartableExpr` and `TThatableExpr` so the reduce body can
   reference `start` and `that`. Lowers to `Expr::TCollatedBy`.

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
#include <orly/expr/collated_by.h>
#include <orly/expr/thatable.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/startable_expr.h>
#include <orly/synth/thatable_expr.h>

namespace Orly {

  namespace Synth {

    class TExprFactory;

    class TCollatedByExpr
        : public TStartableExpr,
          public TThatableExpr {
      NO_COPY(TCollatedByExpr);
      public:

      TCollatedByExpr(const TExprFactory *expr_factory, const Package::Syntax::TCollatedByExpr *collated_by_expr);

      virtual ~TCollatedByExpr();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      const Expr::TCollatedBy::TPtr &GetSymbol() const;

      virtual Expr::TStartable::TPtr GetStartableSymbol() const;

      virtual Expr::TThatable::TPtr GetThatableSymbol() const;

      private:

      const Package::Syntax::TCollatedByExpr *CollatedByExpr;

      TExpr *Seq;

      TExpr *Reduce;

      TExpr *Having;

      mutable Expr::TCollatedBy::TPtr Symbol;

    };  // TCollatedByExpr

  }  // Synth

}  // Orly
