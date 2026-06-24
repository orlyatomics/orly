/* <orly/synth/union_map_expr.h>

   Synth-layer node for `seq union_map elem` expressions. Holds the
   sequence (lhs) and the per-element set body (rhs). Implements
   `TThatableExpr` so the body's `that` resolves here. Lowers to
   `Expr::TUnionMap`.

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
#include <orly/expr/thatable.h>
#include <orly/expr/union_map.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/thatable_expr.h>

namespace Orly {

  namespace Synth {

    /* TODO */
    class TExprFactory;

    /* TODO */
    class TUnionMapExpr
        : public TThatableExpr {
      NO_COPY(TUnionMapExpr);
      public:

      /* TODO */
      TUnionMapExpr(const TExprFactory *expr_factory, const Package::Syntax::TUnionMapExpr *union_map_expr);

      /* TODO */
      virtual ~TUnionMapExpr();

      /* TODO */
      virtual Expr::TExpr::TPtr Build() const;

      /* TODO */
      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      /* TODO */
      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      /* TODO */
      const Expr::TUnionMap::TPtr &GetSymbol() const;

      /* TODO */
      virtual Expr::TThatable::TPtr GetThatableSymbol() const;

      private:

      /* TODO */
      const Package::Syntax::TUnionMapExpr *UnionMapExpr;

      /* TODO */
      TExpr *Lhs;

      /* TODO */
      TExpr *Rhs;

      /* TODO */
      mutable Expr::TUnionMap::TPtr Symbol;

    };  // TUnionMapExpr

  }  // Synth

}  // Orly
