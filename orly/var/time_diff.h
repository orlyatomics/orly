/* <orly/var/time_diff.h>

   A Orly time_diff, which is Orly::Base::Chrono::TTimeDiff.

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

    class TTimeDiff
        : public TVar::TImpl {
      public:

      virtual Var::TVar &Index(const TVar &);

      virtual TTimeDiff &Add(const TVar &);

      virtual TTimeDiff &And(const TVar &);

      virtual TTimeDiff &Div(const TVar &);

      virtual TTimeDiff &Exp(const TVar &);

      virtual TTimeDiff &Intersection(const TVar &);

      virtual TTimeDiff &Mod(const TVar &);

      virtual TTimeDiff &Mult(const TVar &);

      virtual TTimeDiff &Or(const TVar &);

      virtual TTimeDiff &Sub(const TVar &);

      virtual TTimeDiff &SymmetricDiff(const TVar &);

      virtual TTimeDiff &Union(const TVar &);

      virtual TTimeDiff &Xor(const TVar &);

      Base::Chrono::TTimeDiff GetVal() const {
        return Val;
      }

      virtual size_t GetHash() const;

      virtual Type::TType GetType() const;

      virtual void Touch();

      virtual void Write(std::ostream &strm) const;

      static TVar New(const Base::Chrono::TTimeDiff &that);

      private:

      TTimeDiff(const Base::Chrono::TTimeDiff &that);

      virtual ~TTimeDiff();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      Base::Chrono::TTimeDiff Val;

    };  // TTimeDiff

    template <>
    struct TVar::TDt<Base::Chrono::TTimeDiff> {

      Base::Chrono::TTimeDiff static As(const TVar &that) {
        TTimeDiff *ptr = dynamic_cast<TTimeDiff *>(that.Impl.get());
        if (ptr) {
          return ptr->GetVal();
        }
        std::cerr << "Var is a " << that.GetType() << std::endl;
        throw Rt::TSystemError(HERE, "Trying to cast dynamic Var to time_diff. Var is not a time_diff.");
      }

    };  // TDt<Base::Chrono::TTimeDiff>

  }  // Var

}  // Orly
