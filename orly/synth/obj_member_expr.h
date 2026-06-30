/* <orly/synth/obj_member_expr.h>

   Synth-layer node for `obj.field` -- named-field access on a
   record. Carries the source expression and the field name. Lowers
   to `Expr::TObjMember`, which code-gen emits as `TObjMember`.

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
#include <orly/orly.package.cst.h>
#include <orly/pos_range.h>
#include <orly/synth/expr.h>
#include <orly/synth/name.h>

namespace Orly {

  namespace Synth {

    class TExprFactory;

    class TObjMemberExpr
        : public TExpr {
      NO_COPY(TObjMemberExpr);
      public:

      TObjMemberExpr(const TExprFactory *expr_factory, const Package::Syntax::TPostfixObjMember *postfix_obj_member);

      /* Build `<source>.<name>` from an already-synthesized source
         expression (taking ownership) rather than from a CST node -- used to
         synthesize a `when`-arm payload binder's `operand.Tag` accessor.
         See <orly/synth/when_expr.cc>. */
      TObjMemberExpr(TExpr *source, const TName &name, const TPosRange &pos_range);

      virtual ~TObjMemberExpr();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      /* The backing CST node, or null for the synthesized accessor. */
      const Package::Syntax::TPostfixObjMember *PostfixObjMember;

      TExpr *Expr;

      TName Name;

      /* Source position -- from the CST node when there is one, else
         supplied directly. */
      TPosRange PosRange;

    };  // TObjMemberExpr

  }  // Synth

}  // Orly
