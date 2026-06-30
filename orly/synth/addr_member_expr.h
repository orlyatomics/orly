/* <orly/synth/addr_member_expr.h>

   Synth-layer node for `addr.<n>` -- indexed access into an
   address tuple. Lowers to `Expr::TAddrMember` on `Build()`, which
   code-gen emits as `TAddrMember` with the corresponding part-id.

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

    class TAddrMemberExpr
        : public TExpr {
      NO_COPY(TAddrMemberExpr);
      public:

      TAddrMemberExpr(const TExprFactory *expr_factory, const Package::Syntax::TPostfixAddrMember *that);

      virtual ~TAddrMemberExpr();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      const Package::Syntax::TPostfixAddrMember *AddrMember;

      TExpr *Expr;

      size_t Index;

    };  // TAddrMemberExpr

  }  // Synth

}  // Orly
