/* <orly/var/opt.h>

   A Orly opt, which is a Rt::TOpt.

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
#include <orly/type/rt.h>
#include <orly/var/impl.h>

namespace Orly {

  namespace Var {

    class TOpt
        : public TVar::TImpl {
      public:

      typedef Rt::TOpt<TVar> TOptType;

      virtual Var::TVar &Index(const TVar &);

      virtual TOpt &Add(const TVar &);

      virtual TOpt &And(const TVar &);

      virtual TOpt &Div(const TVar &);

      virtual TOpt &Exp(const TVar &);

      virtual TOpt &Intersection(const TVar &);

      virtual TOpt &Mod(const TVar &);

      virtual TOpt &Mult(const TVar &);

      virtual TOpt &Or(const TVar &);

      virtual TOpt &Sub(const TVar &);

      virtual TOpt &SymmetricDiff(const TVar &);

      virtual TOpt &Union(const TVar &);

      virtual TOpt &Xor(const TVar &);

      const TOptType &GetVal() const {
        return Val;
      }

      virtual size_t GetHash() const;

      virtual Type::TType GetType() const;

      Type::TType GetInnerType() const;


      virtual void Touch();

      virtual void Write(std::ostream &) const;

      private:

      #if defined(ORLY_HOST)
      template <typename TVal>
      TOpt(const Rt::TOpt<TVal> &that) : Type(Type::TDt<TVal>::GetType()) {
        if (that) {
          Val = TVar(*that);
        }
        SetHash();
      }
      #endif

      TOpt(const Rt::TOpt<TVar> &that, const Type::TType &type);

      virtual ~TOpt();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      void SetHash();

      TOptType Val;

      Type::TType Type;

      size_t Hash;

      friend class TVar;

    };  // TOpt

    template <typename TVal>
    TVar TVar::Opt(const Rt::TOpt<TVal> &that) {
      TOpt::TOptType val;
      if (that.IsKnown()) {
        val = TVar(that.GetVal());
      }
      return (new TOpt(val, Type::TDt<TVal>::GetType()))->AsVar();
    }

    template <typename TVal>
    struct TVar::TDt<Rt::TOpt<TVal>> {

      Rt::TOpt<TVal> static As(const TVar &that) {
        TOpt *ptr = dynamic_cast<TOpt *>(that.Impl.get());
        if (ptr) {
          if (ptr->GetVal().IsKnown()) {
            return Rt::TOpt<TVal>(TVar::TDt<TVal>::As(ptr->GetVal().GetVal()));
          } else {
            return *Rt::TOpt<TVal>::Unknown;
          }
        }
        std::cerr << "Var is a " << that.GetType() << std::endl;
        throw Rt::TSystemError(HERE, "Trying to cast dynamic Var to opt. Var is not an opt.");
      }

    };  // TDt<Rt::TOpt<TVal>>

  }  // Var

}  // Orly

