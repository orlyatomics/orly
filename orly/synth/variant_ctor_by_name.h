/* <orly/synth/variant_ctor_by_name.h>

   Synth-layer node for a variant constructor routed through a type
   definition's name (`tree.Leaf(1)`, `tree.Nil()`, #103). A recursive
   variant type's literal cannot be written, so the alias must be able to
   construct; the form works for any variant type def.

   The CST shape is a `TPostfixCall` whose callee is an obj-member on a
   bare name and whose arguments are a single positional expression (the
   payload) or empty (a tag-only arm). `Claims()` recognizes that shape;
   the expr factory routes it here instead of to `TPostfixCall`. The name
   binds as a `TRef<TTypeDef>` and `Build()` lowers to the existing
   `Expr::TVariantCtor` with the def's symbolic type, so no new expr node
   (and no walker-matrix work) is needed.

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
#include <string>

#include <base/class_traits.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/expr.h>
#include <orly/synth/new_expr.h>
#include <orly/synth/type_def.h>

namespace Orly {

  namespace Synth {

    class TVariantCtorByName
        : public TExpr {
      NO_COPY(TVariantCtorByName);
      public:

      /* True iff. the call has the `name.Tag(payload?)` shape this node
         handles: callee is an obj-member whose source is a bare name, and
         the arguments are a single positional expression or empty. */
      static bool Claims(const Package::Syntax::TPostfixCall *postfix_call);

      TVariantCtorByName(const TExprFactory *expr_factory, const Package::Syntax::TPostfixCall *postfix_call);

      virtual ~TVariantCtorByName();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      const Package::Syntax::TPostfixCall *PostfixCall;

      /* The type definition whose symbolic type is the declared variant. */
      TDef::TRef<TTypeDef> TypeDef;

      /* The chosen arm's tag. */
      std::string Tag;

      /* The payload expression, or nullptr for a tag-only arm. */
      TExpr *Payload;

    };  // TVariantCtorByName

  }  // Synth

}  // Orly
