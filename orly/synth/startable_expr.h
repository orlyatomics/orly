/* <orly/synth/startable_expr.h>

   Mixin base for synth expressions that introduce a `start`
   binding (`TReduceExpr`, `TCollatedByExpr`). The outer
   expression's symbol must be built before the rhs body so the
   inner `start` reference can resolve to it -- counterpart of
   `TThatableExpr` / `TLhsRhsableExpr`.

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
#include <orly/expr/startable.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/expr.h>

namespace Orly {

  namespace Synth {

    /* TODO */
    class TExprFactory;

    /* TODO */
    class TStartableExpr
        : virtual public TExpr {
      NO_COPY(TStartableExpr);
      public:

      /* TODO */
      TStartableExpr() = default;

      virtual ~TStartableExpr() = default;

      /* TODO */
      virtual Expr::TStartable::TPtr GetStartableSymbol() const = 0;

    };  // TStartableExpr

  }  // Synth

}  // Orly
