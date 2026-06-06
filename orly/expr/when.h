/* <orly/expr/when.h>

   `TWhen` -- the exhaustive `(e) when { Tag: body; ... }` variant match
   expression IR node (#95 Phase 4). The operand must type-check as a
   `Type::TVariant`; the arm tags must cover its tag set EXACTLY (every
   arm a real tag, no duplicates, no missing arm), checked at compile
   time in `GetTypeImpl`. The result type is the unified type of the arm
   bodies (the same join `TIfElse` uses).

   It carries the operand and the arm bodies as `TNAry` children
   (container[0] is the operand; container[1..] are the bodies, parallel
   to `Tags`). The code_gen layer lowers it to a nested ternary on the
   operand's `GetWhich()` selecting the matching arm -- reusing the M4
   primitives. Arm bodies read the active payload via the `e.<Tag>`
   accessor, or bind it to a name with the `Tag(n): body` arm form (sugar
   desugared in synth to `body where { n = operand.Tag; }`, so it is
   transparent here -- a binder arm body is simply an `Expr::TWhere`).

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

#include <memory>
#include <string>
#include <vector>

#include <base/class_traits.h>
#include <orly/expr/n_ary.h>

namespace Orly {

  namespace Expr {

    class TWhen
        : public TNAry<std::vector<TExpr::TPtr>> {
      NO_COPY(TWhen);
      public:

      typedef std::shared_ptr<TWhen> TPtr;

      typedef std::vector<TExpr::TPtr> TExprVec;

      /* `tags` and `bodies` are parallel (one per arm); `operand` is the
         matched variant. */
      static TPtr New(
          const TExpr::TPtr &operand,
          const std::vector<std::string> &tags,
          const TExprVec &bodies,
          const TPosRange &pos_range);

      virtual void Accept(const TVisitor &visitor) const;

      const TExpr::TPtr &GetOperand() const {
        return GetContainer()[0];
      }

      size_t GetArmCount() const {
        return Tags.size();
      }

      const std::string &GetArmTag(size_t arm_idx) const {
        return Tags[arm_idx];
      }

      const TExpr::TPtr &GetArmBody(size_t arm_idx) const {
        return GetContainer()[arm_idx + 1];
      }

      /* The asciibetical index (the value the generated native struct's
         GetWhich() returns) of arm `arm_idx`'s tag in the operand variant
         type. Valid once the operand has type-checked as a variant. */
      size_t GetArmWhich(size_t arm_idx) const;

      virtual Type::TType GetTypeImpl() const override;

      private:

      TWhen(
          const TExpr::TPtr &operand,
          const std::vector<std::string> &tags,
          const TExprVec &bodies,
          const TPosRange &pos_range);

      std::vector<std::string> Tags;

    };  // TWhen

  }  // Expr

}  // Orly
