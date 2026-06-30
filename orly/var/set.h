/* <orly/var/set.h>

   A Orly set, which is a Rt::TSet.

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

#include <orly/rt/containers.h>
#include <orly/rt/runtime_error.h>
#include <orly/type/rt.h>
#include <orly/var/impl.h>

namespace Orly {

  namespace Var {

    class TSet
        : public TVar::TImpl {
      public:

      typedef Rt::TSet<TVar> TSetType;

      virtual Var::TVar &Index(const TVar &);

      virtual TSet &Add(const TVar &);

      virtual TSet &And(const TVar &);

      virtual TSet &Div(const TVar &);

      virtual TSet &Exp(const TVar &);

      virtual TSet &Intersection(const TVar &);

      virtual TSet &Mod(const TVar &);

      virtual TSet &Mult(const TVar &);

      virtual TSet &Or(const TVar &);

      virtual TSet &Sub(const TVar &);

      virtual TSet &SymmetricDiff(const TVar &);

      virtual TSet &Union(const TVar &);

      virtual TSet &Xor(const TVar &);

      const Type::TType &GetElemType() const {
        return Type;
      }

      const TSetType &GetVal() const {
        return Val;
      }

      virtual size_t GetHash() const;

      virtual void Write(std::ostream &) const;

      virtual Type::TType GetType() const;

      virtual void Touch();

      private:

      #if defined(ORLY_HOST)
      template <typename TVal>
      TSet(const Rt::TSet<TVal> &that) : Type(Type::TDt<TVal>::GetType()) {
        for (auto iter = that.begin(); iter != that.end(); ++iter) {
          Val.insert(TVar(*iter));
        }
        SetHash();
      }
      #endif

      TSet(const Rt::TSet<TVar> &that, const Type::TType &type);

      virtual ~TSet();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      void SetHash();

      TSetType Val;

      Type::TType Type;

      size_t Hash;

      friend class TVar;

    };  // TSet

    template <typename TVal>
    TVar TVar::Set(const Rt::TSet<TVal> &that) {
      Rt::TSet<TVar> val;
      for (auto iter = that.begin(); iter != that.end(); ++iter) {
        val.insert(TVar(*iter));
      }
      return (new TSet(val, Type::TDt<TVal>::GetType()))->AsVar();
    }

    template<typename TVal>
    struct TVar::TDt<Rt::TSet<TVal>> {

      Rt::TSet<TVal> static As(const TVar &that) {
        TSet *ptr = dynamic_cast<TSet *>(that.Impl.get());
        if (ptr) {
          Rt::TSet<TVal> set_;
          for (auto iter = ptr->GetVal().begin(); iter != ptr->GetVal().end(); ++iter) {
            set_.insert(TVar::TDt<TVal>::As(*iter));
          }
          return set_;
        }
        std::cerr << "Var is a " << that.GetType() << std::endl;
        throw Rt::TSystemError(HERE, "Trying to cast dynamic Var to set. Var is not a set.");
      }

    };  // TDt<Rt::TSet<TVal>>

  }  // Var

}  // Orly