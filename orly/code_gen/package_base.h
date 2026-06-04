/* <orly/code_gen/package_base.h>

   The dependency-injection base for `CodeGen::TPackage`. Exposes
   just the slice that function- and inline-builders actually need
   (the `AddrMap` / `ReverseAddrMap` index-UUID lookup, the symbol-
   package name) without forcing every helper to include the full
   `CodeGen::TPackage` definition. `CodeGen::TPackage` inherits this.

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

#include <unordered_map>

#include <base/hash.h>
#include <base/path.h>
#include <base/uuid.h>

#include <orly/symbol/package.h>
#include <orly/type.h>
#include <orly/type/unwrap.h>

namespace Orly {

  namespace CodeGen {

    /* Dependency injection for package to expose enough context to build functions. */
    namespace L0 {

      /* TODO */
      class TPackage {
        NO_COPY(TPackage);
        public:

        /* TODO */
        typedef std::unordered_map<Base::TUuid, std::pair<Orly::Type::TType, Orly::Type::TType>> TAddrMap;
        typedef std::unordered_map<std::pair<Orly::Type::TType, Orly::Type::TType>, Base::TUuid> TRevAddrMap;

        /* TODO */
        virtual ~TPackage() {}

        /* TODO */
        inline const Base::TUuid &GetIndexIdFor(const Type::TType &addr, const Type::TType &val) const {
          auto pos = ReverseAddrMap.find(
              std::make_pair(Type::UnwrapSequence(addr),
                             Type::UnwrapOptional(Type::UnwrapMutable(val))));
          assert(pos!= ReverseAddrMap.end());
          return pos->second;
        }

        /* TODO */
        inline const TAddrMap &GetAddrMap() const {
          return AddrMap;
        }

        /* TODO */
        inline const TRevAddrMap &GetReverseAddrMap() const {
          return ReverseAddrMap;
        }

        /* TODO */
        inline const Package::TName &GetName() const {
          assert(Symbol);
          return Symbol->GetName();
        }

        protected:

        /* TODO */
        TPackage(const Symbol::TPackage::TPtr &package) : Symbol(package) {}

        /* TODO */
        Symbol::TPackage::TPtr Symbol;

        /* TODO */
        TAddrMap AddrMap;

        /* TODO */
        TRevAddrMap ReverseAddrMap;

      };  // TPackage

    }  // L0

  }  // CodeGen

}  // Orly
