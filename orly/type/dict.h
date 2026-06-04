/* <orly/type/dict.h>

   The dictionary type `{TKey : TVal}`. Interned by the `(TKey,
   TVal)` pair via `TInternedType<TDict, TType, TType>` -- two
   `TDict::Get(k, v)` calls with the same key/value types return the
   same instance.

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

#include <orly/type/managed_type.h>

namespace Orly {

  namespace Type {

    class TDict : public TInternedType<TDict, TType, TType> {
      NO_COPY(TDict);
      public:

      const TType &GetKey() const {
        return std::get<0>(TInternedType::GetKey());
      }

      const TType &GetVal() const {
        return std::get<1>(TInternedType::GetKey());
      }

      static TType Get(const TType &key, const TType &val) {
        return TInternedType::Get(key, val);
      }

      private:
      TDict(const TType &key, const TType &val) : TInternedType(key, val) {}
      virtual ~TDict();

      virtual void Write(std::ostream &) const;
      friend class TInternedType;
    };  // TDict

  }  // Type

}  // Orly