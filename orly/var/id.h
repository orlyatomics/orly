/* <orly/var/id.h>

   A Orly id, which is Base::TUuid.

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

#include <base/as_str.h>
#include <orly/rt/runtime_error.h>
#include <orly/var/impl.h>

namespace Orly {

  namespace Var {

    class TId
        : public TVar::TImpl {
      public:

      virtual Var::TVar &Index(const TVar &);

      virtual TId &Add(const TVar &);

      virtual TId &And(const TVar &);

      virtual TId &Div(const TVar &);

      virtual TId &Exp(const TVar &);

      virtual TId &Intersection(const TVar &);

      virtual TId &Mod(const TVar &);

      virtual TId &Mult(const TVar &);

      virtual TId &Or(const TVar &);

      virtual TId &Sub(const TVar &);

      virtual TId &SymmetricDiff(const TVar &);

      virtual TId &Union(const TVar &);

      virtual TId &Xor(const TVar &);

      Base::TUuid GetVal() const {
        return Val;
      }

      virtual size_t GetHash() const;

      virtual Type::TType GetType() const;

      virtual void Touch();

      virtual void Write(std::ostream &) const;

      static TVar New(const Base::TUuid &that);

      private:

      TId(const Base::TUuid &that);

      virtual ~TId();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      Base::TUuid Val;

    };  // TId

    template <>
    struct TVar::TDt<Base::TUuid> {

      Base::TUuid static As(const TVar &that) {
        TId *ptr = dynamic_cast<TId *>(that.Impl.get());
        if (ptr) {
          return ptr->GetVal();
        }
        throw Rt::TSystemError(
            HERE, Base::AsStr("Trying to dynamic cast Var to id. \"", that.GetType(), "\" is not an id.").c_str());
      }

    };  // TDt<Base::TUuid>

  }  // Var

}  // Orly
