/* <orly/synth/variant_ctor.h>

   Synth-layer node for a variant / tagged-union constructor
   (`<| Tag(T) | ... |>.Tag(payload)`, #95 Phase 3). Holds the synth
   variant type, the chosen tag, and the (optional) payload expression.
   Lowers to `Expr::TVariantCtor` on `Build()`; a tag-only arm builds an
   empty-object payload (the unit value).

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
#include <orly/synth/type.h>

namespace Orly {

  namespace Synth {

    class TVariantCtor
        : public TExpr {
      NO_COPY(TVariantCtor);
      public:

      TVariantCtor(const TExprFactory *expr_factory, const Package::Syntax::TVariantCtor *variant_ctor);

      virtual ~TVariantCtor();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      const Package::Syntax::TVariantCtor *VariantCtor;

      /* The synth variant type (`<| ... |>`). */
      TType *VariantTypeNode;

      /* The chosen arm's tag. */
      std::string Tag;

      /* The payload expression, or nullptr for a tag-only arm. */
      TExpr *Payload;

    };  // TVariantCtor

  }  // Synth

}  // Orly
