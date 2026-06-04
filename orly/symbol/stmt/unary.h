/* <orly/symbol/stmt/unary.h>

   Abstract base for one-operand statements. Concrete subclass is
   `TDelete` (deletes the key referenced by the stmt arg).

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
#include <orly/pos_range.h>
#include <orly/symbol/stmt/stmt.h>
#include <orly/symbol/stmt/stmt_arg.h>

namespace Orly {

  namespace Symbol {

    namespace Stmt {

      class TUnary
          : public TStmt {
        NO_COPY(TUnary);
        public:

        virtual ~TUnary();

        const TStmtArg::TPtr &GetStmtArg() const;

        protected:

        TUnary(const TStmtArg::TPtr &stmt_arg, const TPosRange &pos_range);

        private:

        TStmtArg::TPtr StmtArg;

      };  // TUnary

    }  // Stmt

  }  // Symbol

}  // Orly
