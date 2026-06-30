/* <orly/synth/where_expr.h>

   Synth-layer node for `expr where { definitions }` -- a body
   expression with locally-scoped definitions. Owns an inner
   `TScope` populated by `TDefFactory` from the parsed definitions.
   Lowers to `Expr::TWhere`.

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
#include <functional>

#include <base/class_traits.h>
#include <orly/expr/where.h>
#include <orly/orly.package.cst.h>
#include <orly/symbol/scope.h>
#include <orly/synth/expr.h>
#include <orly/synth/def_factory.h>
#include <orly/synth/scope_and_def.h>
#include <tools/nycr/lexeme.h>

namespace Orly {

  namespace Synth {

    class TExprFactory;

    class TWhereExpr
        : public TExpr, public TScope {
      NO_COPY(TWhereExpr);
      public:

      TWhereExpr(const TExprFactory *expr_factory, const Package::Syntax::TWhereExpr *where_expr);

      ~TWhereExpr();

      Expr::TExpr::TPtr Build() const;

      void BuildSymbol();

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      Symbol::TScope::TPtr GetScopeSymbol() const;

      Expr::TWhere::TPtr GetSymbol() const;

      bool HasSymbol() const;

      private:

      class TLocalDefFactory
          : public TDefFactory {
        NO_COPY(TLocalDefFactory);
        public:

        static void NewDefs(
              const TExprFactory *expr_factory,
              const Package::Syntax::TOptDefSeq *opt_def_seq);

        private:

        TLocalDefFactory(const TExprFactory *expr_factory);

        virtual void operator()(const Package::Syntax::TInstallerDef *that) const;

        virtual void operator()(const Package::Syntax::TUpgraderDef *that) const;

        virtual void operator()(const Package::Syntax::TUninstallerDef *that) const;

        void OnTopLevel(const char *desc, const TPosRange &pos_range) const;

      };  // TLocalDefFactory

      virtual void ForEachControlledRef(const std::function<void (TAnyRef &)> &cb) const;

      const Package::Syntax::TWhereExpr *WhereExpr;

      TExpr *Expr;

      Expr::TWhere::TPtr Symbol;

    };  // TWhereExpr

  }  // Synth

}  // Orly
