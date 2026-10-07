/* <orly/expr/keys.h>

   `TKeys` -- the `keys <[pattern]>` IR node that walks the index
   for matching keys. Carries the address-shaped key pattern, the
   dereferenced value type and, for keyset paging (#735), an optional
   bound: `after <[..]>` starts the walk just past a key, `from <[..]>`
   at it. Lowered to `CodeGen::TKeys`.

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
#include <vector>

#include <base/class_traits.h>
#include <orly/shared_enum.h>
#include <orly/expr/addr.h>
#include <orly/expr/n_ary.h>

namespace Orly {

  namespace Expr {

    class TKeys
        : public TNAry<TAddr::TMemberVec> {
      NO_COPY(TKeys);
      public:

      typedef std::shared_ptr<TKeys> TPtr;

      static TPtr New(const TAddr::TMemberVec &members, const Type::TType &value_type, const TPosRange &pos_range,
                      const TExpr::TPtr &bound = nullptr, bool bound_is_inclusive = false);

      virtual ~TKeys();

      virtual void Accept(const TVisitor &visitor) const;

      /* Alias for GetContainer from TNAry */
      inline const TAddr::TMemberVec &GetMembers() const {
        return GetContainer();
      }

      /* Returns the type of Addr used in the key expression. */
      virtual Type::TType GetAddrType() const;

      /* A sequence of GetAddrType */
      virtual Type::TType GetTypeImpl() const override;

      /* Get's the type of the value stored in keys expression */
      const Type::TType &GetValueType() const {
        return ValueType;
      }

      /* The keyset-paging bound (#735), or null for an unbounded walk. Its
         type is the pattern's address type (checked in GetTypeImpl). */
      const TExpr::TPtr &GetBound() const {
        return Bound;
      }

      /* True for `from` (the bound key itself is included), false for `after`. */
      bool GetBoundIsInclusive() const {
        return BoundIsInclusive;
      }

      private:
      Type::TType ValueType;

      TExpr::TPtr Bound;

      bool BoundIsInclusive;

      TKeys(const TAddr::TMemberVec &members, const Type::TType &value_type, const TPosRange &pos_range,
            const TExpr::TPtr &bound, bool bound_is_inclusive);

    };  // TKeys

  }  // Expr

}  // Orly
