/* <orly/code_gen/if_else.h>

   `TIfElse` emits a ternary `if pred then true_case else false_case`
   expression. `True` and `False` are `TInlineScope`s (not flat
   `TInline`s) because the branches can introduce local definitions.
   (The old `InDependsOn` recursion guard is gone: AppendDependency's
   insert-check breaks dependency-walk cycles for every node type,
   #297/#298.)

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

#include <iostream>
#include <orly/code_gen/inline.h>
#include <orly/code_gen/inline_scope.h>

namespace Orly {

  namespace CodeGen {

    class TIfElse : public TInline {
      NO_COPY(TIfElse);
      public:

      typedef std::shared_ptr<const TIfElse> TPtr;

      static TPtr New(
          const L0::TPackage *package,
          const Type::TType &ret_type,
          const Expr::TExpr::TPtr &true_case,
          const TInline::TPtr &predicate,
          const Expr::TExpr::TPtr &false_case);

      void WriteExpr(TCppPrinter &out) const;

      /* Dependency graph */
      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const override {
        AppendDependency(Predicate, dependency_set);
        AppendDependency(True, dependency_set);
        AppendDependency(False, dependency_set);
      }

      private:
      TIfElse(const L0::TPackage *package,
              const Type::TType &ret_type,
              const Expr::TExpr::TPtr &true_case,
              const TInline::TPtr &predicate,
              const Expr::TExpr::TPtr &false_case);

      TInline::TPtr Predicate;
      TInlineScope::TPtr True, False;
    };

  } // CodeGen

} // Orly