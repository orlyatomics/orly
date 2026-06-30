/* <orly/synth/mutate_stmt.h>

   Synth-layer node for mutation statements (`x += v;`, `s |= {x};`,
   `m = v;`). Holds the lhs target, the mutator op, and the rhs
   value. Lowers to `Symbol::Stmt::TMutate`.

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
#include <orly/shared_enum.h>
#include <orly/orly.package.cst.h>
#include <orly/symbol/stmt/mutate.h>
#include <orly/synth/stmt.h>
#include <orly/synth/scope_and_def.h>

namespace Orly {

  namespace Synth {

    class TExpr;
    class TExprFactory;

    class TMutateStmt
        : public TStmt {
      NO_COPY(TMutateStmt);
      public:

      TMutateStmt(const TExprFactory *expr_factory, const Package::Syntax::TMutateStmt *mutate_stmt);

      virtual ~TMutateStmt();

      virtual Symbol::Stmt::TStmt::TPtr Build() const;

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb) const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb) const;

      private:

      const Package::Syntax::TMutateStmt *MutateStmt;

      TExpr *Lhs;

      TExpr *Rhs;

    };  // TMutateStmt

  }  // Synth

}  // Orly
