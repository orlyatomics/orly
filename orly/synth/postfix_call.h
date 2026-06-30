/* <orly/synth/postfix_call.h>

   Synth-layer node for function call expressions
   (`f(.name1: val1, ...)`). Holds the callee expression and the
   named argument map. Lowers to `Expr::TCall` after the function
   symbol is resolved.

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
#include <map>

#include <base/class_traits.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/expr.h>

namespace Orly {

  namespace Synth {

    class TExprFactory;

    class TPostfixCall
        : public TExpr {
      NO_COPY(TPostfixCall);
      public:

      TPostfixCall(const TExprFactory *expr_factory, const Package::Syntax::TPostfixCall *postfix_call);

      virtual ~TPostfixCall();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      typedef std::map<TName, TExpr *> TArgMap;

      void Cleanup();

      const Package::Syntax::TPostfixCall *PostfixCall;

      TArgMap Args;

      TExpr *Expr;

    };  // TPostfixCall

  }  // Synth

}  // Orly
