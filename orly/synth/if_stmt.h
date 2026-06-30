/* <orly/synth/if_stmt.h>

   Synth-layer node for `if pred { stmts } else { stmts }`
   statements inside effecting blocks. Holds the predicate, the
   then-block, and an optional else-block. Lowers to
   `Symbol::Stmt::TIf`.

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
#include <vector>

#include <base/class_traits.h>
#include <orly/orly.package.cst.h>
#include <orly/symbol/stmt/if.h>
#include <orly/synth/new_expr.h>
#include <orly/synth/stmt.h>
#include <orly/synth/stmt_block.h>

namespace Orly {

  namespace Synth {

    class TExpr;
    class TStmtBlock;

    class TIfStmt
        : public TStmt {
      NO_COPY(TIfStmt);
      public:

      TIfStmt(const TExprFactory *expr_factory, const Package::Syntax::TIfStmt *if_stmt);

      virtual ~TIfStmt();

      virtual Symbol::Stmt::TStmt::TPtr Build() const;

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb) const;

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb) const;

      private:

      class TIfClause {
        NO_COPY(TIfClause);
        public:

        template <typename TNode>
        TIfClause(const TExprFactory *expr_factory, const TNode *node)
            : Expr(nullptr), StmtBlock(nullptr) {
          assert(expr_factory);
          try {
            Expr = expr_factory->NewExpr(node->GetExpr());
            StmtBlock = new TStmtBlock(expr_factory, node->GetStmtBlock());
          } catch (...) {
            delete Expr;
            delete StmtBlock;
            throw;
          }
        }

        ~TIfClause();

        Symbol::Stmt::TIfClause::TPtr Build() const;

        virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb) const;

        virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb) const;

        private:

        TExpr *Expr;

        const TStmtBlock *StmtBlock;

      };  // TIfClause

      typedef std::vector<TIfClause *> TIfClauseVec;

      void Cleanup();

      const Package::Syntax::TIfStmt *IfStmt;

      TIfClauseVec IfClauses;

      TStmtBlock *OptElseBlock;

    };  // TIfStmt

  }  // Synth

}  // Orly
