/* <orly/synth/assert_expr.h>

   Synth-layer node for `assert { ... }` blocks. Holds a vector of
   `TAssertCase` (each optionally named) plus an outer `TExpr`.
   Implements `TThatableExpr` so the inner cases can reference the
   asserted value with `that`. Lowers to `Expr::TAssert`.

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

#include <cassert>
#include <string>
#include <vector>

#include <base/class_traits.h>
#include <optional>
#include <orly/expr/assert.h>
#include <orly/expr/thatable.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/thatable_expr.h>

namespace Orly {

  namespace Synth {

    class TExprFactory;

    class TAssertExpr
        : public TThatableExpr {
      NO_COPY(TAssertExpr);
      public:

      TAssertExpr(const TExprFactory *expr_factory, const Package::Syntax::TAssertExpr *assert_expr);

      virtual ~TAssertExpr();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      const Expr::TAssert::TPtr &GetSymbol() const;

      virtual Expr::TThatable::TPtr GetThatableSymbol() const;

      private:

      class TAssertCase {
        NO_COPY(TAssertCase);
        public:

        TAssertCase(const std::optional<std::string> &opt_name, TExpr *expr);

        ~TAssertCase();

        Expr::TAssertCase::TPtr Build(const Expr::TAssert::TPtr &assert) const;

        void ForEachInnerScope(const std::function<void (TScope *)> &cb);

        void ForEachRef(const std::function<void (TAnyRef &)> &cb);

        private:

        const std::optional<std::string> OptName;

        TExpr *Expr;

      };  // TAssertCase

      typedef std::vector<TAssertCase *> TAssertCaseVec;

      void Cleanup();

      const Package::Syntax::TAssertExpr *AssertExpr;

      TAssertCaseVec AssertCases;

      TExpr *Expr;

      mutable Expr::TAssert::TPtr Symbol;

    };  // TAssertExpr

  }  // Synth

}  // Orly
