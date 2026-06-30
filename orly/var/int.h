/* <orly/var/int.h>

   A Orly integer, which is 64 bits and signed.

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
#include <typeinfo>

#include <orly/rt/runtime_error.h>
#include <orly/var/impl.h>

namespace Orly {

  namespace Var {

    class TInt
        : public TVar::TImpl {
      public:

      virtual Var::TVar &Index(const TVar &);

      virtual TInt &Add(const TVar &);

      virtual TInt &And(const TVar &);

      virtual TInt &Div(const TVar &);

      virtual TInt &Exp(const TVar &);

      virtual TInt &Intersection(const TVar &);

      /* The max / min merge-mutation operators (#213). */
      virtual TInt &Max(const TVar &);
      virtual TInt &Min(const TVar &);

      virtual TInt &Mod(const TVar &);

      virtual TInt &Mult(const TVar &);

      virtual TInt &Or(const TVar &);

      virtual TInt &Sub(const TVar &);

      virtual TInt &SymmetricDiff(const TVar &);

      virtual TInt &Union(const TVar &);

      virtual TInt &Xor(const TVar &);

      int64_t GetVal() const {
        return Val;
      }

      virtual size_t GetHash() const;

      virtual Type::TType GetType() const;

      virtual void Touch();

      static TVar New(int64_t that);

      virtual void Write(std::ostream &stream) const;

      private:

      TInt(int64_t that);

      virtual ~TInt();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      int64_t Val;

    };  // TInt

    template <>
    struct TVar::TDt<int64_t> {

      int64_t static As(const TVar &that) {
        const Orly::Var::TInt *ptr = dynamic_cast<const TInt *>(that.Impl.get());
        if (ptr) {
          return ptr->GetVal();
        }
        std::cerr << "TYPE NAME: " << typeid(*that.Impl.get()).name() << std::endl;
        std::cerr << "Var is a " << that.GetType() << std::endl;
        throw Rt::TSystemError(HERE, "Trying to cast dynamic Var to int. Var is not an int.");
      }

    };  // TDt<int64_t>

  }  // Var

}  // Orly
