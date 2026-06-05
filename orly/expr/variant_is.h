/* <orly/expr/variant_is.h>

   `TVariantIs` -- the `expr is <Tag>` variant-arm predicate IR node
   (#95 Phase 3 M4). Mirrors `TIsKnown` (orly/expr/known.h): a unary test
   that yields a bool. It is true at runtime iff the operand variant's
   active arm is `<Tag>`.

   The operand must type-check as a `Type::TVariant` whose tag set
   includes `<Tag>`; `GetTypeImpl` enforces both and returns `TBool`.
   It also holds the tag so the code_gen layer can lower it to
   `(operand).GetWhich() == <idx>`, where `<idx>` is the asciibetical
   index of `<Tag>` in the variant's `TVariantElems` (the iteration order
   the generated native struct's `Which` follows).

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

#include <string>

#include <base/class_traits.h>
#include <orly/expr/unary.h>
#include <orly/expr/visitor.h>
#include <orly/pos_range.h>

namespace Orly {

  namespace Expr {

    class TVariantIs
        : public TUnary {
      NO_COPY(TVariantIs);
      public:

      typedef std::shared_ptr<TVariantIs> TPtr;

      static TPtr New(const TExpr::TPtr &expr, const std::string &tag, const TPosRange &pos_range);

      virtual void Accept(const TVisitor &visitor) const;

      const std::string &GetTag() const {
        return Tag;
      }

      /* The asciibetical index of Tag in the operand variant's tag set --
         the value the generated native struct's GetWhich() returns for
         this arm. Computed from the (type-checked) operand variant type. */
      size_t GetWhich() const;

      virtual Type::TType GetTypeImpl() const override;

      private:

      TVariantIs(const TExpr::TPtr &expr, const std::string &tag, const TPosRange &pos_range);

      std::string Tag;

    };  // TVariantIs

  }  // Expr

}  // Orly
