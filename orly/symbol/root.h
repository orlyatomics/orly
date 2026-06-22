/* <orly/symbol/root.h>

   Holds the top-level expression of a callable. `TFunction` inherits
   this to carry its body expression; `TRoot` itself is an
   `Expr::TExprParent` so the expression tree can climb back to the
   owning function.

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

#include <functional>

#include <base/assert_true.h>
#include <base/class_traits.h>
#include <orly/expr/expr.h>
#include <orly/expr/expr_parent.h>

namespace Orly {

  namespace Symbol {

    class TRoot
        : public Expr::TExprParent {
      NO_COPY(TRoot);
      public:

      virtual ~TRoot();

      const Expr::TExpr::TPtr &GetExpr() const;

      void SetExpr(const Expr::TExpr::TPtr &expr);

      /* Replace the held expression with one that wraps it -- `wrap(old)` must
         return a new expression whose construction re-parents `old` to itself
         (e.g. `Expr::TAs::New(old, ...)`). Used by the implicit-widening pass
         (#104 Phase 5) to slip an `as wide` cast around an argument value. The
         old expression is detached from this root before `wrap` runs, so the
         wrapper's constructor sees it parentless and may adopt it. */
      void WrapExpr(const std::function<Expr::TExpr::TPtr (const Expr::TExpr::TPtr &)> &wrap);

      protected:

      TRoot();  // Should only be used by TFunction

      TRoot(const Expr::TExpr::TPtr &expr);

      private:

      Expr::TExpr::TPtr Expr;

    };  // TRoot

  }  // Symbol

}  // Symbol
