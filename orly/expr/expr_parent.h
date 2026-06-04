/* <orly/expr/expr_parent.h>

   `TExprParent` -- a tiny abstract marker class for "I can be the
   parent of one or more `TExpr`s". Used so `TExpr::ExprParent`
   doesn't have to be `TExpr *` (the parent might be a synth- or
   symbol-layer object that owns the expression tree).

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

namespace Orly {

  namespace Expr {

    class TExprParent {
      NO_COPY(TExprParent);
      public:

      virtual ~TExprParent();

      protected:

      TExprParent();

    };  // TExprParent

  }  // Expr

}  // Orly
