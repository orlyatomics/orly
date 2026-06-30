/* <orly/var/bool.h>

   A Orly bool, which is a bool.

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

#include <iostream>

#include <orly/rt/runtime_error.h>
#include <orly/var/impl.h>

namespace Orly {

  namespace Var {

    class TBool
        : public TVar::TImpl {
      public:

      virtual size_t GetHash() const;

      virtual Var::TVar &Index(const TVar &);

      virtual TBool &Add(const TVar &);

      virtual TBool &And(const TVar &);

      virtual TBool &Div(const TVar &);

      virtual TBool &Exp(const TVar &);

      virtual TBool &Intersection(const TVar &);

      virtual TBool &Mod(const TVar &);

      virtual TBool &Mult(const TVar &);

      virtual TBool &Or(const TVar &);

      virtual TBool &Sub(const TVar &);

      virtual TBool &SymmetricDiff(const TVar &);

      virtual TBool &Union(const TVar &);

      virtual TBool &Xor(const TVar &);

      bool GetVal() const {
        return Val;
      }

      virtual Type::TType GetType() const;

      virtual void Touch();

      static TVar New(bool that);

      virtual void Write(std::ostream &stream) const;

      private:

      TBool(bool that);

      virtual ~TBool();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      bool Val;

    };  // TBool

    template <>
    struct TVar::TDt<bool> {

      bool static As(const TVar &that) {
        TBool *ptr = dynamic_cast<TBool *>(that.Impl.get());
        if (ptr) {
          return ptr->GetVal();
        }
        std::cerr << "Var is a " << that.GetType() << std::endl;
        throw Rt::TSystemError(HERE, "Trying to cast dynamic Var to bool. Var is not a bool.");
      }

    };  // TDt<bool>

  }  // Var

}  // Orly
