/* <orly/var/dict.h>

   A Orly dict, which is a Rt::TDict.

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

    class TDict
        : public TVar::TImpl {
      public:

      typedef Rt::TDict<TVar, TVar> TDictType;
      typedef TDictType TElems;

      virtual Var::TVar &Index(const TVar &);

      virtual TDict &Add(const TVar &);

      virtual TDict &And(const TVar &);

      virtual TDict &Div(const TVar &);

      virtual TDict &Exp(const TVar &);

      virtual TDict &Intersection(const TVar &);

      virtual TDict &Mod(const TVar &);

      virtual TDict &Mult(const TVar &);

      virtual TDict &Or(const TVar &);

      virtual TDict &Sub(const TVar &);

      virtual TDict &SymmetricDiff(const TVar &);

      virtual TDict &Union(const TVar &);

      virtual TDict &Xor(const TVar &);

      const TDictType &GetVal() const {
        return Val;
      }

      void Insert(const TDictType::const_iterator &begin, const TDictType::const_iterator &end);

      void Remove(const Rt::TSet<TVar> &keys);

      const Type::TType &GetKeyType() const {
        return KeyType;
      }

      const Type::TType &GetValType() const {
        return ValType;
      }

      virtual void Touch();

      virtual size_t GetHash() const;

      virtual Type::TType GetType() const;

      virtual void Write(std::ostream &) const;

      private:

      #if defined(ORLY_HOST)
      template <typename TVal, typename TKey>
      TDict(const Rt::TDict<TKey, TVal> &that) : KeyType(Type::TDt<TKey>::GetType()), ValType(Type::TDt<TVal>::GetType()) {
        for (auto iter = that.begin(); iter != that.end(); ++iter) {
          Val[TVar(iter->first)] = TVar(iter->second);
        }
        SetHash();
      }
      #endif

      TDict(const Rt::TDict<TVar, TVar> &that, const Type::TType &key_type, const Type::TType &val_type);

      virtual ~TDict();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      void SetHash();

      TDictType Val;

      Type::TType KeyType;

      Type::TType ValType;

      size_t Hash;

      friend class TVar;

    };  // TDict

    template <typename TKey, typename TVal>
    TVar TVar::Dict(const Rt::TDict<TKey, TVal> &that) {
      Rt::TDict<TVar, TVar> val;
      for (auto iter = that.begin(); iter != that.end(); ++iter) {
        val[TVar(iter->first)] = TVar(iter->second);
      }
      return (new TDict(val, Type::TDt<TKey>::GetType(), Type::TDt<TVal>::GetType()))->AsVar();
    }

    template<typename TKey, typename TVal>
    struct TVar::TDt<Rt::TDict<TKey, TVal>> {

      Rt::TDict<TKey, TVal> static As(const TVar &that) {
        TDict *ptr = dynamic_cast<TDict *>(that.Impl.get());
        if (ptr) {
          Rt::TDict<TKey, TVal> map_;
          for (auto iter = ptr->GetVal().begin(); iter != ptr->GetVal().end(); ++iter) {
            map_[TVar::TDt<TKey>::As(iter->first)] = TVar::TDt<TVal>::As(iter->second);
          }
          return map_;
        }
        std::cerr << "Var is a " << that.GetType() << std::endl;
        throw Rt::TSystemError(HERE, "Trying to cast dynamic Var to map. Var is not a map.");
      }

    };  // TDt<Rt::TDict<TKey, TVal>>

  }  // Var

}  // Orly