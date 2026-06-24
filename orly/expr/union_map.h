/* <orly/expr/union_map.h>

   The `union_map` expression (#219): `seq union_map elem` maps each
   element of `seq` to a set (the `elem` body references `that`) and
   unions the results, from an implicit empty-set identity. Equivalent to
   `seq reduce start (empty {T}) | elem` but without spelling out the
   identity, so the per-element set type T is inferred here at type-check.

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

#include <base/class_traits.h>
#include <orly/expr/thatable_binary.h>

namespace Orly {

  namespace Expr {

    class TUnionMap
        : public TThatableBinary {
      NO_COPY(TUnionMap);
      public:

      typedef std::shared_ptr<TUnionMap> TPtr;

      static TPtr New(const TExpr::TPtr &lhs, const TPosRange &pos_range);

      virtual void Accept(const TVisitor &visitor) const;

      virtual Type::TType GetTypeImpl() const override;

      private:

      TUnionMap(const TExpr::TPtr &lhs, const TPosRange &pos_range);

    };  // TUnionMap

  }  // Expr

}  // Orly
