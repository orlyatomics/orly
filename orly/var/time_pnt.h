/* <orly/var/time_pnt.h>

   A Orly time_pnt, which is Orly::Base::Chrono::TTimePnt

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

#include <base/chrono.h>
#include <orly/rt/runtime_error.h>
#include <orly/var/impl.h>

namespace Orly {

  namespace Var {

    class TTimePnt
        : public TVar::TImpl {
      public:

      virtual Var::TVar &Index(const TVar &);

      virtual TTimePnt &Add(const TVar &);

      virtual TTimePnt &And(const TVar &);

      virtual TTimePnt &Div(const TVar &);

      virtual TTimePnt &Exp(const TVar &);

      virtual TTimePnt &Intersection(const TVar &);

      virtual TTimePnt &Mod(const TVar &);

      virtual TTimePnt &Mult(const TVar &);

      virtual TTimePnt &Or(const TVar &);

      virtual TTimePnt &Sub(const TVar &);

      virtual TTimePnt &SymmetricDiff(const TVar &);

      virtual TTimePnt &Union(const TVar &);

      virtual TTimePnt &Xor(const TVar &);

      Base::Chrono::TTimePnt GetVal() const {
        return Val;
      }

      virtual size_t GetHash() const;

      virtual Type::TType GetType() const;

      virtual void Touch();

      virtual void Write(std::ostream &strm) const;

      static TVar New(const Base::Chrono::TTimePnt &that);

      private:

      TTimePnt(const Base::Chrono::TTimePnt &that);

      virtual ~TTimePnt();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      Base::Chrono::TTimePnt Val;

    };  // TTimePnt

    template <>
    struct TVar::TDt<Base::Chrono::TTimePnt> {

      Base::Chrono::TTimePnt static As(const TVar &that) {
        TTimePnt *ptr = dynamic_cast<TTimePnt *>(that.Impl.get());
        if (ptr) {
          return ptr->GetVal();
        }
        std::cerr << "Var is a " << that.GetType() << std::endl;
        throw Rt::TSystemError(HERE, "Trying to cast dynamic Var to time_pnt. Var is not a time_pnt.");
      }

    };  // TDt<Base::Chrono::TTimePnt>

  }  // Var

}  // Orly
