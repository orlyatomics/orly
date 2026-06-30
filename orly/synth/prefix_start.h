/* <orly/synth/prefix_start.h>

   Synth-layer node for the `start` keyword inside a reduce body.
   Resolves to the nearest enclosing `TStartableExpr` (typically a
   `TReduceExpr` or `TCollatedByExpr`) during build and lowers to
   `Expr::TStart`.

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

#include <base/assert_true.h>
#include <base/class_traits.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/expr.h>

namespace Orly {

  namespace Synth {

    class TExprFactory;
    class TStartableExpr;

    class TPrefixStart
        : public TExpr {
      NO_COPY(TPrefixStart);
      public:

      TPrefixStart(const TExprFactory *expr_factory, const Package::Syntax::TPrefixStart *prefix_start);

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      const Package::Syntax::TPrefixStart *PrefixStart;

      TExpr *Expr;

      const TStartableExpr *StartableExpr;

    };  // TPrefixStart

  }  // Synth

}  // Orly
