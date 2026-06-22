/* <orly/expr/if_else.h>

   `TIfElse` -- the ternary `if pred then a else b` IR node.
   Inherits `TNAry<array<TExpr::TPtr, 3>>` -- predicate / true-case
   / false-case. Type-checked to the unified type of the two
   branches.

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

#include <array>
#include <memory>

#include <base/class_traits.h>
#include <orly/expr/n_ary.h>

namespace Orly {

  namespace Expr {

    class TIfElse
        : public TNAry<std::array<TExpr::TPtr, 3U>> {
      NO_COPY(TIfElse);
      public:

      typedef std::shared_ptr<TIfElse> TPtr;

      typedef std::array<TExpr::TPtr, 3U> TExprArray;

      static TPtr New(
          const TExpr::TPtr &true_case,
          const TExpr::TPtr &predicate,
          const TExpr::TPtr &false_case,
          const TPosRange &pos_range);

      virtual void Accept(const TVisitor &visitor) const;

      /* Alias for GetContainer */
      inline const TExprArray &GetExprs() const {
        return GetContainer();
      }

      const TExpr::TPtr &GetTrue() const;

      const TExpr::TPtr &GetPredicate() const;

      const TExpr::TPtr &GetFalse() const;

      /* Wrap the true / false branch with `wrap(branch)` -- used by the
         implicit-widening pass (#104 Phase 5) to widen the narrower branch so
         both branches share the join type. */
      void WrapTrue(const std::function<TExpr::TPtr (const TExpr::TPtr &)> &wrap) {
        WrapChild(GetContainer()[0], wrap);
      }
      void WrapFalse(const std::function<TExpr::TPtr (const TExpr::TPtr &)> &wrap) {
        WrapChild(GetContainer()[2], wrap);
      }

      virtual Type::TType GetTypeImpl() const override;

      private:

      TIfElse(
          const TExpr::TPtr &true_case,
          const TExpr::TPtr &predicate,
          const TExpr::TPtr &false_case,
          const TPosRange &pos_range);

    };  // TIfElse

  }  // Expr

}  // Orly
