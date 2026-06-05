/* <orly/synth/postfix_is_variant.h>

   Synth-layer node for the `expr is <Tag>` variant-arm predicate
   (#95 Phase 3 M4). Lowers to `Expr::TVariantIs`. A near-mirror of
   `postfix_is_known.h` (the `expr is known` test), but the trailing
   token is a tag `name` rather than the `known` keyword.

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
#include <orly/synth/expr.h>

namespace Orly {

  namespace Synth {

    class TExprFactory;

    class TPostfixIsVariant
        : public TExpr {
      NO_COPY(TPostfixIsVariant);
      public:

      TPostfixIsVariant(const TExprFactory *expr_factory, const Package::Syntax::TPostfixIsVariant *postfix_is_variant);

      virtual ~TPostfixIsVariant();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      const Package::Syntax::TPostfixIsVariant *PostfixIsVariant;

      TExpr *Expr;

    };  // TPostfixIsVariant

  }  // Synth

}  // Orly
