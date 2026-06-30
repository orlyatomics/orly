/* <orly/synth/collected_by_expr.h>

   Synth-layer node for `collected_by`. Holds the sequence and the
   collect body. Implements `TThatableExpr` for the body's `that`
   reference. Lowers to `Expr::TCollectedBy`.

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
#include <orly/expr/collected_by.h>
#include <orly/expr/lhsrhsable.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/lhsrhsable_expr.h>

namespace Orly {

  namespace Synth {

    class TExprFactory;

    class TCollectedByExpr
        : public TLhsRhsableExpr {
      NO_COPY(TCollectedByExpr);
      public:

      TCollectedByExpr(
          const TExprFactory *expr_factory,
          const Package::Syntax::TCollectedByExpr *collected_by_expr);

      virtual ~TCollectedByExpr();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      const Expr::TCollectedBy::TPtr &GetSymbol() const;

      virtual Expr::TLhsRhsable::TPtr GetLhsRhsableSymbol() const;

      private:

      const Package::Syntax::TCollectedByExpr *CollectedByExpr;

      TExpr *Lhs;

      TExpr *Rhs;

      mutable Expr::TCollectedBy::TPtr Symbol;

    };  // TCollectedByExpr

  }  // Synth

}  // Orly
