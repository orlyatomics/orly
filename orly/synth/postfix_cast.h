/* <orly/synth/postfix_cast.h>

   Synth-layer node for the `expr as T` cast operator. Holds the
   source expression and the target type. Lowers to `Expr::TAs`,
   which code-gen turns into a `TUnary{Cast}`.

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
#include <orly/symbol/package.h>
#include <orly/synth/type.h>
#include <orly/synth/expr.h>

namespace Orly {

  namespace Synth {

    /* TODO */
    class TExprFactory;

    /* TODO */
    class TPostfixCast
        : public TExpr {
      NO_COPY(TPostfixCast);
      public:

      /* TODO */
      TPostfixCast(const TExprFactory *expr_factory, const Package::Syntax::TPostfixCast *postfix_cast);

      /* TODO */
      virtual ~TPostfixCast();

      /* TODO */
      virtual Expr::TExpr::TPtr Build() const;

      /* TODO */
      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      /* TODO */
      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      /* TODO */
      const Package::Syntax::TPostfixCast *PostfixCast;

      /* TODO */
      TExpr *Lhs;

      /* TODO */
      TType *Rhs;

    };  // TPostfixCast

    /* Synthesize recursive-variant `as`-widening (#104). Run once per package
       AFTER the multi-pass synth Build (so every def -- and therefore every
       expression's type -- is resolvable) and BEFORE Symbol::TScope::TypeCheck
       (so newly minted functions are not added to the package's function set
       while it is being iterated). For each `narrow as wide` cast between
       recursive variants, mint a deduplicated top-level widening fold into the
       package scope and annotate the cast's `Expr::TAs` to call it; codegen
       then emits the call instead of the (impossible) flat reinterpret cast.
       Casts whose payloads use a shape we do not yet synthesize are left
       un-annotated, so the type checker reports the canonical #104 message. */
    void SynthesizeRecursiveVariantWidenings(const Symbol::TPackage::TPtr &package);

    /* Insert IMPLICIT variant widenings (#104 Phase 5). Run once per package
       after Build and BEFORE SynthesizeRecursiveVariantWidenings: it slips an
       `as wide` cast around any value that flows into a wider variant context
       without an explicit cast -- a function argument whose parameter is a
       wider variant -- so the value is widened to the expected type. The
       inserted casts are ordinary `Expr::TAs` nodes, so the widening-synth pass
       that runs next lowers them exactly like an explicit cast (a fold for a
       recursive widening, a flat rebuild otherwise). */
    void InsertImplicitVariantWidenings(const Symbol::TPackage::TPtr &package);

  }  // Synth

}  // Orly
