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
#include <tuple>
#include <utility>

#include <orly/desc.h>
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

      TAddr(const TAddrType &that);

      virtual ~TAddr();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      void SetHash();

      TAddrType Val;

      size_t Hash;

      friend class TVar;

    };  // TAddr

    /* One tuple element -> one (dir, var) pair: a TDesc<T>-wrapped element marks the
       descending direction and contributes its unwrapped value (#384). */
    template <typename TElem>
    inline std::pair<TAddrDir, TVar> MakeAddrElem(const TElem &elem) {
      return std::make_pair(TAddrDir::Asc, TVar(elem));
    }

    template <typename TVal>
    inline std::pair<TAddrDir, TVar> MakeAddrElem(const TDesc<TVal> &elem) {
      return std::make_pair(TAddrDir::Desc, TVar(*elem));
    }

    /* The runtime address type is a plain std::tuple (orly/rt/tuple.h), with TDesc<T>
       wrapping descending elements -- the old variadic Rt::TAddr this file's disabled
       machinery targeted no longer exists (#384). */
    template <typename... TElems>
    TVar TVar::Addr(const std::tuple<TElems...> &that) {
      TAddr::TAddrType val;
      val.reserve(sizeof...(TElems));
      std::apply(
          [&val](const auto &... elem) {
            (val.push_back(MakeAddrElem(elem)), ...);
          },
          that);
      return Addr(val);
    }

    template <typename... TElems>
    TVar::TVar(const std::tuple<TElems...> &that) {
      *this = Addr(that);
    }

    /* Element extraction for the reverse cast: a TDesc<T> element rewraps after converting
       the underlying value. */
    template <typename TElem>
    struct TAddrElemDt {
      static TElem As(const std::pair<TAddrDir, TVar> &elem) {
        return TVar::TDt<TElem>::As(elem.second);
      }
    };

    template <typename TVal>
    struct TAddrElemDt<TDesc<TVal>> {
      static TDesc<TVal> As(const std::pair<TAddrDir, TVar> &elem) {
        return TDesc<TVal>(TVar::TDt<TVal>::As(elem.second));
      }
    };

    template <typename... TElems>
    struct TVar::TDt<std::tuple<TElems...>> {

      static std::tuple<TElems...> As(const TVar &that) {
        TAddr *ptr = dynamic_cast<TAddr *>(that.Impl.get());
        if (ptr) {
          const TAddr::TAddrType &val = ptr->GetVal();
          if (val.size() != sizeof...(TElems)) {
            throw Rt::TSystemError(HERE, "Trying to cast dynamic Var to an addr of a different arity.");
          }
          return [&val]<std::size_t... Idx>(std::index_sequence<Idx...>) {
            return std::tuple<TElems...>(TAddrElemDt<TElems>::As(val[Idx])...);
          }(std::index_sequence_for<TElems...>{});
        }
        std::cerr << "Var is a " << that.GetType() << std::endl;
        throw Rt::TSystemError(HERE, "Trying to cast dynamic Var to addr. Var is not an addr.");
      }

    };  // TDt<std::tuple<TElems...>>

  }  // Var

}  // Orly