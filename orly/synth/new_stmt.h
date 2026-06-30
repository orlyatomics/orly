/* <orly/synth/new_stmt.h>

   Synth-layer node for `new <[key]> <- val;` statements inside
   effecting blocks. Lowers to `Symbol::Stmt::TNew`.

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
#include <orly/synth/stmt.h>
#include <orly/symbol/stmt/stmt.h>

namespace Orly {

  namespace Synth {

    class TExpr;
    class TExprFactory;

    class TNewStmt
        : public TStmt {
      NO_COPY(TNewStmt);
      public:

      TNewStmt(const TExprFactory *expr_factory, const Package::Syntax::TNewStmt *new_stmt);

      virtual ~TNewStmt();

      virtual Symbol::Stmt::TStmt::TPtr Build() const;

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb) const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb) const;

      private:

      const Package::Syntax::TNewStmt *NewStmt;

      TExpr *Lhs;

      TExpr *Rhs;

    };  // TNewStmt

  }  // Synth

}  // Orly
