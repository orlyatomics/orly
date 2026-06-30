/* <orly/synth/affix_expr.h>

   Synth-layer node for unary affix expressions (`not`, unary `-`,
   `*` deref, etc.). Holds a single operand `TExpr` and a `TNew`
   function pointer that selects which `Expr::T*` factory to invoke
   at `Build()` time.

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
#include <orly/expr/expr.h>
#include <orly/pos_range.h>
#include <orly/synth/expr.h>

namespace Orly {

  namespace Synth {

    class TAffixExpr
        : public TExpr {
      NO_COPY(TAffixExpr);
      public:

      typedef Expr::TExpr::TPtr (*TNew)(const Expr::TExpr::TPtr &expr, const TPosRange &pos_range);

      TAffixExpr(TExpr *expr, TNew new_, const TPosRange &pos_range);

      virtual ~TAffixExpr();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      TExpr *Expr;

      TNew New;

      const TPosRange PosRange;

    };  // TAffixExpr

  }  // Synth

}  // Orly
