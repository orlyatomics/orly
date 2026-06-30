/* <orly/synth/postfix_slice.h>

   Synth-layer node for `expr[idx]` (index) and `expr[lhs:rhs]`
   (slice). `Colon` distinguishes the two forms; `OptLhs` / `OptRhs`
   are nullable for open-ended slices. Lowers to `Expr::TSlice`.

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
#include <orly/orly.package.cst.h>
#include <orly/synth/type.h>
#include <orly/synth/expr.h>

namespace Orly {

  namespace Synth {

    class TExprFactory;

    class TPostfixSlice
        : public TExpr {
      NO_COPY(TPostfixSlice);
      public:

      TPostfixSlice(const TExprFactory *expr_factory, const Package::Syntax::TPostfixSlice *postfix_slice);

      virtual ~TPostfixSlice();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      void Cleanup();

      const Package::Syntax::TPostfixSlice *PostfixSlice;

      bool Colon;

      TExpr *Expr;

      TExpr *OptLhs;

      TExpr *OptRhs;

    };  // TPostfixSlice

  }  // Synth

}  // Orly
