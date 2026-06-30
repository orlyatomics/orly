/* <orly/var/real.h>

   A Orly real, which is a double.

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

    class TReal
        : public TVar::TImpl {
      public:

      virtual Var::TVar &Index(const TVar &);

      virtual TReal &Add(const TVar &);

      virtual TReal &And(const TVar &);

      virtual TReal &Div(const TVar &);

      virtual TReal &Exp(const TVar &);

      virtual TReal &Intersection(const TVar &);

      /* The max / min merge-mutation operators (#213). */
      virtual TReal &Max(const TVar &);
      virtual TReal &Min(const TVar &);

      virtual TReal &Mod(const TVar &);

      virtual TReal &Mult(const TVar &);

      virtual TReal &Or(const TVar &);

      virtual TReal &Sub(const TVar &);

      virtual TReal &SymmetricDiff(const TVar &);

      virtual TReal &Union(const TVar &);

      virtual TReal &Xor(const TVar &);

      virtual size_t GetHash() const;

      double GetVal() const {
        return Val;
      }

      virtual Type::TType GetType() const;

      virtual void Touch();

      static TVar New(double that);

      virtual void Write(std::ostream &stream) const;

      private:

      TReal(double that);

      virtual ~TReal();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      double Val;

    };  // TReal

    template <>
    struct TVar::TDt<double> {

      double static As(const TVar &that) {
        TReal *ptr = dynamic_cast<TReal *>(that.Impl.get());
        if (ptr) {
          return ptr->GetVal();
        }
        std::cerr << "Var is a " << that.GetType() << std::endl;
        throw Rt::TSystemError(HERE, "Trying to cast dynamic Var to real. Var is not a real.");
      }

    };  // TDt<double>

  }  // Var

}  // Orly
