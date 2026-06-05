/* <orly/synth/when_expr.h>

   Synth-layer node for the exhaustive `(e) when { Tag: body; ... }`
   variant match expression (#95 Phase 4). Lowers to `Expr::TWhen`.
   Holds the operand and the arm bodies as plain sub-expressions (arms
   introduce no new binding -- a v1 arm body reads the active payload via
   the `e.<Tag>` accessor), so unlike `TAssertExpr` it needs no thatable
   or symbol machinery.

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

#include <string>
#include <vector>

#include <base/class_traits.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/expr.h>

namespace Orly {

  namespace Synth {

    class TExprFactory;

    class TWhenExpr
        : public TExpr {
      NO_COPY(TWhenExpr);
      public:

      TWhenExpr(const TExprFactory *expr_factory, const Package::Syntax::TWhenExpr *when_expr);

      virtual ~TWhenExpr();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      void Cleanup();

      const Package::Syntax::TWhenExpr *WhenExpr;

      TExpr *Operand;

      std::vector<std::string> Tags;

      std::vector<TExpr *> Bodies;

    };  // TWhenExpr

  }  // Synth

}  // Orly
