/* <orly/synth/if_else_expr.h>

   Synth-layer node for `if pred then a else b` ternary expressions
   (and chained `if`/`else if`). Holds the predicate and the two
   branches. Lowers to `Expr::TIfElse`.

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

#include <base/class_traits.h>
#include <orly/synth/expr.h>

namespace Orly {

  namespace Synth {

    class TIfElseExpr
        : public TExpr {
      NO_COPY(TIfElseExpr);
      public:

      TIfElseExpr(
          TExpr *true_case,
          TExpr *predicate,
          TExpr *false_case,
          const Package::Syntax::TIfExpr *if_expr);

      virtual ~TIfElseExpr();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      const Package::Syntax::TIfExpr *IfExpr;

      TExpr *FalseCase;

      TExpr *Predicate;

      TExpr *TrueCase;

    };  // TIfElseExpr

  }  // Synth

}  // Orly
