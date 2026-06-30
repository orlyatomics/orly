/* <orly/var/obj.h>

   A Orly object, which is a dictionary of vars.

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

#include <cassert>
#include <string>
#include <unordered_map>

#include <base/class_traits.h>
#include <orly/rt/obj.h>
#include <orly/rt/runtime_error.h>
#include <orly/type/impl.h>
#include <orly/type/obj.h>
#include <orly/var/impl.h>

namespace Orly {

  namespace Type {

    template <typename TCompound>
    template <typename TVal>
    void TObj::Meta<TCompound>::TField<TVal>::GetVal(const TCompound &that, Var::TVar &out) const {
      out = Var::TVar(that.*Member);
    }

  }

  namespace Var {

    class TObj
        : public TVar::TImpl {
      public:

      virtual Var::TVar &Index(const TVar &);

      virtual TObj &Add(const TVar &);

      virtual TObj &And(const TVar &);

      virtual TObj &Div(const TVar &);

      virtual TObj &Exp(const TVar &);

      virtual TObj &Intersection(const TVar &);

      virtual TObj &Mod(const TVar &);

      virtual TObj &Mult(const TVar &);

      virtual TObj &Or(const TVar &);

      virtual TObj &Sub(const TVar &);

      virtual TObj &SymmetricDiff(const TVar &);

      virtual TObj &Union(const TVar &);

      virtual TObj &Xor(const TVar &);

      template <typename TCompound, typename TVal>
      static void GetValFromField(const TCompound &compound, const typename Type::TObj::Meta<TCompound>::template TField<TVal> &field, TVar &out) {
        out = TVar(compound.*(field.Member));
      }

      typedef std::unordered_map<std::string, TVar> TFieldsByName;
      typedef TFieldsByName TElems;

      virtual size_t GetHash() const;

      virtual Type::TType GetType() const;

      virtual void Touch();

      virtual void Write(std::ostream &) const;

      const TFieldsByName &GetVal() const {
        return FieldsByName;
      }

      private:

      /* NOTE: This is left here in case someone wants to use the runtime object inspection.

      template <typename TCompound>
      TObj(const TCompound &that) {
        typedef typename TCompound::TDynamicMembers TFoobar;
        // If this assertion fails, it means you're trying to construct a TVar from a type for which there is no metadata.
        if (!Type::TObj::Meta<TCompound>::TClass::TryGetClass()) {
          throw Base::TNotImplementedError(HERE);
        }
        assert(Type::TObj::Meta<TCompound>::TClass::TryGetClass());
        for (auto field = Type::TObj::Meta<TCompound>::TAnyField::GetFirstField(); field; field = field->GetNextField()) {
          auto result = FieldsByName.insert(std::make_pair(field->GetName(), DefaultVar));
          // If this assertion fails, it means the metadata describing the fields of TCompound contains at least two fields with the same name.
          assert(result.second);
          field->GetVal(that, result.first->second);
          TypeMap[field->GetName()] = field->GetType();
        }
        SetHash();
      }
      */

      TObj(const TFieldsByName &that);

      virtual ~TObj();

      virtual void Accept(const TVisitor &visitor) const;

      virtual TVar Copy() const;

      void SetHash();

      TFieldsByName FieldsByName;

      size_t Hash;

      std::map<std::string, Type::TType> TypeMap;

      static const TVar DefaultVar;

      friend class TVar;

    };  // TObj

    /* See <orly/var.h>. */
    template <typename TCompound>
    TVar::TVar(const TCompound &that) {
      *this = that.AsVar();
    }

    template <typename TVal>
    TVal Var::TVar::TDt<TVal>::As(const TVar &that) {
      TObj *ptr = dynamic_cast<TObj *>(that.Impl.get());
      if (ptr) {
        return TVal(ptr->GetVal());
      }
      throw Rt::TSystemError(HERE, "Trying to cast dynamic Var to wrong type");
    }

  }  // Var

}  // Orly
