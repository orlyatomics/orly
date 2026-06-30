/* <orly/var/unknown.h>

   The 'unknown' value, which is Orly's equivalent of null, none, nil, etc.

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

#include <orly/var/impl.h>

namespace Orly {

  namespace Var {

    class TUnknown
        : public TVar::TImpl {
      public:

      virtual Var::TVar &Index(const TVar &);

      virtual TUnknown &Add(const TVar &);

      virtual TUnknown &And(const TVar &);

      virtual TUnknown &Div(const TVar &);

      virtual TUnknown &Exp(const TVar &);

      virtual TUnknown &Intersection(const TVar &);

      virtual TUnknown &Mod(const TVar &);

      virtual TUnknown &Mult(const TVar &);

      virtual TUnknown &Or(const TVar &);

      virtual TUnknown &Sub(const TVar &);

      virtual TUnknown &SymmetricDiff(const TVar &);

      virtual TUnknown &Union(const TVar &);

      virtual TUnknown &Xor(const TVar &);

      virtual size_t GetHash() const;

      virtual Type::TType GetType() const;

      virtual void Touch();

      virtual void Write(std::ostream &) const;

      static TVar New();

      private:

      TUnknown() {}

      virtual ~TUnknown();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      friend class TVar;

    };  // TUnknown

  }  // Var

}  // Orly
