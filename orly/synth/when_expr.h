/* <orly/synth/when_expr.h>

   Synth-layer node for the exhaustive `(e) when { Tag: body; ... }`
   variant match expression (#95). Lowers to `Expr::TWhen`, holding the
   operand and one sub-expression per arm body.

   A plain `Tag: body` arm reads the active payload via the `e.<Tag>`
   accessor. A `Tag(n): body` arm additionally binds the payload to the
   name `n`: its body is wrapped (in when_expr.cc) in a local scope that
   defines `n = operand.Tag`, lowering to an `Expr::TWhere` that
   `Expr::TWhen` consumes as an ordinary arm body. See when_expr.cc.

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
