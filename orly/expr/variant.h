/* <orly/expr/variant.h>

   `TVariantCtor` -- variant / tagged-union constructor IR node
   (`<| Tag(T) | ... |>.Tag(payload)`, #95 Phase 3). Holds the active
   tag, the (already-lowered) payload expression, and the full declared
   variant type. A tag-only arm carries an empty-object payload (the
   unit value), matching `orly/type/variant.h`.

   `GetTypeImpl` returns the full declared `Type::TVariant` -- the
   constructor is self-contained (the surface carries the whole type),
   so no widening / call-site `::(T)` flow-back is needed. It checks
   that the chosen tag exists in the declared type and that the payload
   expression's type matches the declared arm payload type.

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

#include <base/class_traits.h>
#include <orly/expr/unary.h>
#include <orly/expr/visitor.h>
#include <orly/pos_range.h>
#include <orly/type/impl.h>

namespace Orly {

  namespace Expr {

    class TVariantCtor
        : public TUnary {
      NO_COPY(TVariantCtor);
      public:

      typedef std::shared_ptr<TVariantCtor> TPtr;

      static TPtr New(const std::string &tag,
                      const TExpr::TPtr &payload,
                      const Type::TType &variant_type,
                      const TPosRange &pos_range);

      virtual void Accept(const TVisitor &visitor) const;

      const std::string &GetTag() const {
        return Tag;
      }

      const Type::TType &GetVariantType() const {
        return VariantType;
      }

      virtual Type::TType GetTypeImpl() const override;

      private:

      TVariantCtor(const std::string &tag,
                   const TExpr::TPtr &payload,
                   const Type::TType &variant_type,
                   const TPosRange &pos_range);

      std::string Tag;

      Type::TType VariantType;

    };  // TVariantCtor

  }  // Expr

}  // Orly
