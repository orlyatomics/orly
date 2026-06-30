/* <orly/synth/set_ctor.h>

   Synth-layer node for a set literal (`{elem, elem, ...}`). Holds
   the ordered element expressions from the CST. Lowers to a
   `TBasicCtor<TSetContainer>` on `Build()`.

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

#include <unordered_set>

#include <base/class_traits.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/expr.h>

namespace Orly {

  namespace Synth {

    class TExprFactory;

    class TSetCtor
        : public TExpr {
      NO_COPY(TSetCtor);
      public:

      TSetCtor(const TExprFactory *expr_factory, const Package::Syntax::TSetCtor *set_ctor);

      virtual ~TSetCtor();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      void Cleanup();

      const Package::Syntax::TSetCtor *SetCtor;

      std::unordered_set<TExpr *> Exprs;

    };  // TSetCtor

  }  // Synth

}  // Orly
