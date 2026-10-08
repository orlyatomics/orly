/* <orly/code_gen/keys.h>

   `TKeys` emits a `keys <[pattern]>` expression: builds the
   `TAddrElems` (same `(TAddrDir, TInline)` shape as
   `TBasicCtor<TAddrContainer>`) plus the dereferenced value type
   and emits the runtime call that walks the index for matching
   keys. With a keyset-paging bound (#735) the call also passes the
   bound key and whether it is inclusive, and the runtime starts the
   walk there.

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

#include <orly/code_gen/inline.h>

#include <orly/shared_enum.h>

namespace Orly {

  namespace CodeGen {

    class TKeys : public TInline {
      NO_COPY(TKeys);
      public:

      typedef std::vector<std::pair<TAddrDir, TInline::TPtr>> TAddrElems;
      typedef std::shared_ptr<const TKeys> TPtr;

      TKeys(const L0::TPackage *package,
            const Type::TType &seq_type,
            const Type::TType &val_type,
            TAddrElems &&addr_elems,
            const TInline::TPtr &bound,
            bool bound_is_inclusive,
            bool count_only);

      void WriteExpr(TCppPrinter &out) const;

      /* Dependency graph */
      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const override {
        for (const auto &iter : AddrElems) {
          AppendDependency(iter.second, dependency_set);
        }
        if (Bound) {
          AppendDependency(Bound, dependency_set);
        }
      }

      private:
      TAddrElems AddrElems;

      Type::TType ValType;

      /* The keyset-paging bound, or null for an unbounded walk. */
      TInline::TPtr Bound;

      bool BoundIsInclusive;

      Type::TType AddrType;

      bool CountOnly;
    };

  } // CodeGen

} // Orly