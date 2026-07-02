/* <orly/var/addr.h>

   A Orly addr, which is a Orly::Rt::TAddr.

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
#include <orly/shared_enum.h>
#include <orly/type/rt.h>
#include <orly/var/impl.h>

namespace Orly {

  namespace Var {

    class TAddr
        : public TVar::TImpl {
      public:

      typedef TAddrDir TDir;

      typedef std::vector<std::pair<TDir, TVar>> TAddrType;
      typedef TAddrType TElems;

      virtual Var::TVar &Index(const TVar &);

      virtual TAddr &Add(const TVar &);

      virtual TAddr &And(const TVar &);

      virtual TAddr &Div(const TVar &);

      virtual TAddr &Exp(const TVar &);

      virtual TAddr &Intersection(const TVar &);

      virtual TAddr &Mod(const TVar &);

      virtual TAddr &Mult(const TVar &);

      virtual TAddr &Or(const TVar &);

      virtual TAddr &Sub(const TVar &);

      virtual TAddr &SymmetricDiff(const TVar &);

      virtual TAddr &Union(const TVar &);

      virtual TAddr &Xor(const TVar &);

      const TAddrType &GetVal() const {
        return Val;
      }

      virtual size_t GetHash() const;

      virtual Type::TType GetType() const;

      virtual void Touch();

      virtual void Write(std::ostream &strm) const;

      private:

      /* TODO(#384)
      template <typename... TElements>
      TAddr(const Rt::TAddr<TElements...> &that) {
        Unroll(that.GetSuper());
        SetHash();
      }*/

      TAddr(const TAddrType &that);

      virtual ~TAddr();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      void SetHash();

      /* TODO(#384)
      template <size_t Pos, typename... TElements>
      void Unroll(const Rt::TAddrImpl<Pos, TElements...> &that);

      template <size_t Pos>
      void Unroll(const Rt::TAddrImpl<Pos> &) {
      }

      template <size_t Pos, typename THead, typename... TTail>
      void Unroll(const Rt::TAddrImpl<Pos, THead, TTail...> &that) {
        Val.push_back(std::make_pair(that.GetDir(), TVar(that.GetHead())));
        TypeVec.push_back(Type::TDt<THead>::GetType());
        Unroll(that.GetTail());
      }

      template <size_t Pos>
      static void Unroll(TAddrType &, const Rt::TAddrImpl<Pos> &) {}

      template <size_t Pos, typename THead, typename... TTail>
      static void Unroll(TAddrType &val, const Rt::TAddrImpl<Pos, THead, TTail...> &that) {
        val.push_back(std::make_pair(that.GetDir(), TVar(that.GetHead())));
        Unroll(val, that.GetTail());
      }
      */

      TAddrType Val;

      std::vector<std::pair<TDir, Type::TType>> TypeVec; //TODO(#384): Can I get this from TType?

      size_t Hash;

      friend class TVar;

    };  // TAddr

    /* TODO(#384)
    template <typename... TElements>
    TVar TVar::Addr(const Rt::TAddr<TElements...> &that) {
      TAddr::TAddrType val;
      TAddr::Unroll(val, that.GetSuper());
      return (new TAddr(val))->AsVar();
    } */

  }  // Var

  namespace Rt {

    /* TODO(#384)
    template <size_t Pos, typename THead_, typename... TTail>
    TAddrImpl<Pos, THead_, TTail...>::TAddrImpl(const Var::TAddr::TAddrType &vec)
        : TSuper(vec), Head(Var::TVar::TDt<typename THead_::TElem>::As(vec[Pos].second)) {}

    template <typename THead, typename... TTail>
    TAddr<THead, TTail...>::TAddr(const Var::TAddr::TAddrType &vec) : TSuper(vec) {}
    */

  }  // Rt

  namespace Var {

    /* TODO(#384)
    template<typename... TElements>
    struct TVar::TDt<Rt::TAddr<TElements...>> {

      static Rt::TAddr<TElements...> As(const TVar &that) {
        TAddr *ptr = dynamic_cast<TAddr *>(that.Impl.get());
        if (ptr) {
          return Rt::TAddr<TElements...>(ptr->GetVal());
        }
        std::cerr << "Var is a " << that.GetType() << std::endl;
        throw Rt::TSystemError(HERE, "Trying to cast dynamic Var to addr. Var is not an addr.");
      }

    };  // TDt<std::vector<TVal>>
    */

  }  // Var

}  // Orly