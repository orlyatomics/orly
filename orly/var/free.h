/* <orly/var/free.h>

   The 'free' value, which is Orly's way of searching.

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

    class TFree
        : public TVar::TImpl {
      public:

      virtual Var::TVar &Index(const TVar &);

      virtual TFree &Add(const TVar &);

      virtual TFree &And(const TVar &);

      virtual TFree &Div(const TVar &);

      virtual TFree &Exp(const TVar &);

      virtual TFree &Intersection(const TVar &);

      virtual TFree &Mod(const TVar &);

      virtual TFree &Mult(const TVar &);

      virtual TFree &Or(const TVar &);

      virtual TFree &Sub(const TVar &);

      virtual TFree &SymmetricDiff(const TVar &);

      virtual TFree &Union(const TVar &);

      virtual TFree &Xor(const TVar &);

      virtual size_t GetHash() const;

      virtual Type::TType GetType() const;

      virtual void Touch();

      virtual void Write(std::ostream &) const;

      static TVar New(const Type::TType &type);

      private:

      TFree(const Type::TType &type) : Type(type) {}

      virtual ~TFree();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      Type::TType Type;

      friend class TVar;

    };  // TFree

  }  // Var

}  // Orly
