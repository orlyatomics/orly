/* <orly/synth/sort_expr.h>

   Synth-layer node for `seq sorted_by (lhs OP rhs)` expressions.
   Holds the sequence and the comparator body. Inherits
   `TLhsRhsableExpr` so the comparator can reference `lhs` and
   `rhs`. Lowers to `Expr::TSort`.

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
#include <orly/expr/sort.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/expr.h>
#include <orly/synth/lhsrhsable_expr.h>

namespace Orly {

  namespace Synth {

    class TExprFactory;

    class TSortExpr
        : public TLhsRhsableExpr {
      NO_COPY(TSortExpr);
      public:

      TSortExpr(const TExprFactory *expr_factory, const Package::Syntax::TInfixSort *infix_sort);

      virtual ~TSortExpr();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      const Expr::TSort::TPtr &GetSymbol() const;

      virtual Expr::TLhsRhsable::TPtr GetLhsRhsableSymbol() const;

      private:

      void Cleanup();

      const Package::Syntax::TInfixSort *InfixSort;

      TExpr *Lhs;

      TExpr *Rhs;

      mutable Expr::TSort::TPtr Symbol;

    };  // TSortExpr

  }  // Synth

}  // Orly
