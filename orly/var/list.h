/* <orly/var/list.h>

   A Orly list, which is a std::vector.

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

    class TList
        : public TVar::TImpl {
      public:

      typedef std::vector<TVar> TListType;
      typedef TListType TElems;

      virtual Var::TVar &Index(const TVar &);

      virtual TList &Add(const TVar &);

      virtual TList &And(const TVar &);

      virtual TList &Div(const TVar &);

      virtual TList &Exp(const TVar &);

      virtual TList &Intersection(const TVar &);

      virtual TList &Mod(const TVar &);

      virtual TList &Mult(const TVar &);

      virtual TList &Or(const TVar &);

      virtual TList &Sub(const TVar &);

      virtual TList &SymmetricDiff(const TVar &);

      virtual TList &Union(const TVar &);

      virtual TList &Xor(const TVar &);

      const Type::TType &GetElemType() const {
        return Type;
      }

      const TListType &GetVal() const {
        return Val;
      }

      void Append(const TListType &other);

      virtual size_t GetHash() const;

      virtual Type::TType GetType() const;

      virtual void Touch();

      virtual void Write(std::ostream &) const;

      private:

      #if defined(ORLY_HOST)
      template <typename TVal>
      TList(const std::vector<TVal> &that) : Type(Type::TDt<TVal>::GetType()) {
        for (auto iter = that.begin(); iter != that.end(); ++iter) {
          Val.push_back(TVar(*iter));
        }
        SetHash();
      }
      #endif

      TList(const std::vector<TVar> &that, const Type::TType &type);

      virtual ~TList();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      void SetHash();

      TListType Val;

      Type::TType Type;

      size_t Hash;

      friend class TVar;

    };  // TList

    template <typename TVal>
    TVar TVar::List(const std::vector<TVal> &that) {
      std::vector<TVar> val;
      for (auto iter = that.begin(); iter != that.end(); ++iter) {
        val.push_back(TVar(*iter));
      }
      return (new TList(val, Type::TDt<TVal>::GetType()))->AsVar();
    }

    template<typename TVal>
    struct TVar::TDt<std::vector<TVal>> {

      std::vector<TVal> static As(const TVar &that) {
        TList *ptr = dynamic_cast<TList *>(that.Impl.get());
        if (ptr) {
          std::vector<TVal> vec;
          for (auto iter = ptr->GetVal().begin(); iter != ptr->GetVal().end(); ++iter) {
            vec.push_back(TVar::TDt<TVal>::As(*iter));
          }
          return vec;
        }
        std::cerr<< "TYPE NAME: " << typeid(*that.Impl.get()).name() << std::endl;
        std::cerr << "Var is a " << that.GetType() << std::endl;
        throw Rt::TSystemError(HERE, "Trying to cast dynamic Var to list. Var is not a list.");
      }

    };  // TDt<std::vector<TVal>>

  }  // Var

}  // Orly
