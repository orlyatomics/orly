/* <orly/var/impl.h>

   Forward declarations for every concrete `TVar` subclass
   (`TBool`, `TInt`, `TList`, `TSet`, `TDict`, `TObj`, ...). `TVar`
   is the runtime's dynamic value type -- a tagged variant covering
   every orlyscript value. Full class definitions live in
   `orly/var/all.h` and friends; this header just declares the
   forwards consumers need.

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

#include <algorithm>
#include <cassert>
#include <chrono>
#include <memory>
#include <stddef.h>
#include <unordered_map>

#include <base/chrono.h>
#include <base/class_traits.h>
#include <base/impossible_error.h>
#include <base/uuid.h>
#include <orly/shared_enum.h>
//NOTE: We would include <orly/rt.h> but that includes <orly/rt/built_in.h> which includes this
#include <orly/rt/containers.h>
#include <orly/type/impl.h>

namespace Orly {

  namespace Var {

    class TAddr;
    class TBool;
    class TDict;
    class TErr;
    class TFree;
    class TId;
    class TInt;
    class TList;
    class TMutable;
    class TObj;
    class TOpt;
    class TReal;
    class TSet;
    class TStr;
    class TTimeDiff;
    class TTimePnt;
    class TUnknown;
    class TVariant;

    // template <typename TVal> struct TRead;

    class TVar {
      public:

      template <typename TVal>
      struct TDt {

        TVal static As(const TVar &);

      };

      class TVisitor;

      class TDoubleVisitor;

      class TImpl {
        NO_COPY(TImpl);
        public:

        virtual Var::TVar &Index(const TVar &) = 0;

        virtual TImpl &Add(const TVar &) = 0;

        virtual TImpl &And(const TVar &) = 0;

        virtual TImpl &Div(const TVar &) = 0;

        virtual TImpl &Exp(const TVar &) = 0;

        virtual TImpl &Intersection(const TVar &) = 0;

        virtual TImpl &Mod(const TVar &) = 0;

        /* The min / max merge-mutation operators (#213). Unlike the
           operators above these are NOT pure: only TInt / TReal override
           them; the base throws (see impl.cc), so every other TVar type
           rejects `<?=` / `>?=` without a per-type stub. */
        virtual TImpl &Max(const TVar &);
        virtual TImpl &Min(const TVar &);

        virtual TImpl &Mult(const TVar &) = 0;

        virtual TImpl &Or(const TVar &) = 0;

        virtual TImpl &Sub(const TVar &) = 0;

        virtual TImpl &SymmetricDiff(const TVar &) = 0;

        virtual TImpl &Union(const TVar &) = 0;

        virtual TImpl &Xor(const TVar &) = 0;

        protected:

        typedef TVar::TVisitor TVisitor;

        TImpl() {}

        virtual ~TImpl();

        TVar AsVar() {
          return TVar(this);
        }

        virtual void Accept(const TVisitor &visitor) const = 0;

        virtual TVar Copy() const = 0;

        virtual size_t GetHash() const = 0;

        virtual Type::TType GetType() const = 0;

        virtual void Touch() = 0;

        virtual void Write(std::ostream &) const = 0;

        private:

        static void Delete(TImpl *impl);

        friend class TVar;

      };  // TImpl

      /* Construct a new TVar as an unknown.
         See <orly/var/unknown.h>. */
      TVar();

      ~TVar();

      /* Construct a new TVar from a bool.
         See <orly/var/bool.h>. */
      TVar(const bool &that);

      /* Construct a new TVar from an int.
         See <orly/var/int.h>. */
      TVar(const int64_t &that);
      TVar(const int &that);

      /* Construct a new TVar from a double.
         See <orly/var/real.h>. */
      TVar(const double &that);

      /* Construct a new TVar from a std::string.
         See <orly/var/str.h>. */
      TVar(const std::string &that);

      /* Construct a new TVar from a uuid.
         See <orly/var/id.h>. */
      TVar(const Base::TUuid &that);

      /* Construct a new TVar from a time_diff.
         See <orly/var/time_diff.h>. */
      TVar(const Base::Chrono::TTimeDiff &that);

      /* Construct a new TVar from a time_pnt.
         See <orly/var/time_pnt.h>. */
      TVar(const Base::Chrono::TTimePnt &that);

      template <typename TVal>
      TVar(const Rt::TSet<TVal> &that) {
        *this = Set(that);
      }

      template <typename TVal>
      TVar(const std::vector<TVal> &that) {
        *this = List(that);
      }

      /*  Construct a new TVar from a rt mutable.
         See <orly/var/mutable.h>. */
      template <typename TAddr, typename TVal>
      TVar(const Rt::TMutable<TAddr, TVal> &that) {
        *this = Mutable(that);
      }

      template <typename TKey, typename TVal>
      TVar(const Rt::TDict<TKey, TVal> &that) {
        *this = Dict(that);
      }

      /* TODO(#384)
      template <typename... TElements>
      TVar(const Rt::TAddr<TElements...> &that) {
        *this = Addr(that);
      } */

      template <typename TVal>
      TVar(const Rt::TOpt<TVal> &that) {
        *this = Opt(that);
      }

      /* Construct a new TVar from anything else.  There are various explicit
         specializations (after the TVar class definition) for the elementary
         runtime types and templates specialized thereon.  This totally wide-open
         version is used for compound types.
         See <orly/var/obj.h>. */
      template <typename TCompound>
      explicit TVar(const TCompound &that);

      TVar(TVar &&that);

      TVar(const TVar &that);

      static TVar Addr(const std::vector<std::pair<TAddrDir, TVar>> &that);

      /* TODO(#384)
      template <typename... TElements>
      static TVar Addr(const Rt::TAddr<TElements...> &that);
      */

      static TVar Dict(const Rt::TDict<TVar, TVar> &that, const Type::TType &key_type, const Type::TType &val_type);

      template <typename TKey, typename TVal>
      static TVar Dict(const Rt::TDict<TKey, TVal> &that);

      static TVar Free(const Type::TType &type);

      static TVar List(const std::vector<TVar> &that, const Type::TType &type);

      template <typename TVal>
      static TVar List(const std::vector<TVal> &that);

      /*  Construct a new TVar from a mutable.
         See <orly/var/mutable.h>. */
      template <typename TAddr, typename TVal>
      static TVar Mutable(const Rt::TMutable<TAddr, TVal> &that);

      static TVar Obj(const std::unordered_map<std::string, TVar> &map_);

      static TVar Opt(const Rt::TOpt<TVar> &opt, const Type::TType &type);

      template <typename TVal>
      static TVar Opt(const Rt::TOpt<TVal> &that);

      static TVar Set(const Rt::TSet<TVar> &that, const Type::TType &type);

      template <typename TVal>
      static TVar Set(const Rt::TSet<TVal> &that);

      /* Construct a new variant TVar from its full declared type, the active
         tag, and its payload value.  See <orly/var/variant.h>. */
      static TVar Variant(const Type::TType &variant_type, const std::string &tag, const TVar &payload);

      TVar Copy() const;

      template <typename TTarget>
      const TTarget *As() const;

      template <typename TTarget>
      bool Is() const;

      template <typename TTarget>
      const TTarget *TryAs() const;

      TVar &operator=(TVar &&that);

      TVar &operator=(const TVar &that);

      bool operator==(const TVar &that) const;

      bool operator!=(const TVar &that) const {
        return !(*this == that);
      }

      bool operator<(const TVar &that) const;

      bool operator<=(const TVar &that) const {
        return (*this < that) || (*this == that);
      }

      bool operator>(const TVar &that) const;

      bool operator>=(const TVar &that) const {
        return (*this > that) || (*this == that);
      }

      explicit operator bool() const;

      Var::TVar &Index(const TVar &key) {
        return Impl->Index(key);
      }

      Var::TVar &Add(const TVar &rhs) {
        assert(*this);
        Impl->Add(rhs);
        return *this;
      }

      Var::TVar &And(const TVar &rhs) {
        assert(*this);
        Impl->And(rhs);
        return *this;
      }

      Var::TVar &Div(const TVar &rhs) {
        assert(*this);
        Impl->Div(rhs);
        return *this;
      }

      Var::TVar &Exp(const TVar &rhs) {
        assert(*this);
        Impl->Exp(rhs);
        return *this;
      }

      Var::TVar &Intersection(const TVar &rhs) {
        assert(*this);
        Impl->Intersection(rhs);
        return *this;
      }

      Var::TVar &Mod(const TVar &rhs) {
        assert(*this);
        Impl->Mod(rhs);
        return *this;
      }

      Var::TVar &Max(const TVar &rhs) {
        assert(*this);
        Impl->Max(rhs);
        return *this;
      }

      Var::TVar &Min(const TVar &rhs) {
        assert(*this);
        Impl->Min(rhs);
        return *this;
      }

      Var::TVar &Mult(const TVar &rhs) {
        assert(*this);
        Impl->Mult(rhs);
        return *this;
      }

      Var::TVar &Or(const TVar &rhs) {
        assert(*this);
        Impl->Or(rhs);
        return *this;
      }

      Var::TVar &Sub(const TVar &rhs) {
        assert(*this);
        Impl->Sub(rhs);
        return *this;
      }

      Var::TVar &SymmetricDiff(const TVar &rhs) {
        assert(*this);
        Impl->SymmetricDiff(rhs);
        return *this;
      }

      Var::TVar &Union(const TVar &rhs) {
        assert(*this);
        Impl->Union(rhs);
        return *this;
      }

      Var::TVar &Xor(const TVar &rhs) {
        assert(*this);
        Impl->Xor(rhs);
        return *this;
      }

      void Accept(const TVisitor &visitor) const {
        assert(Impl);
        Impl->Accept(visitor);
      }

      int Compare(const TVar &that) const;

      size_t GetHash() const {
        return Impl->GetHash();
      }

      Type::TType GetType() const {
        return Impl->GetType();
      }

      TVar &Reset() {
        return *this = TVar();
      }

      void Touch() {
        Impl->Touch();
      }

      static void Accept(
          const TVar &lhs, const TVar &rhs,
          const TDoubleVisitor &double_visitor);

      void Write(std::ostream &stream) const {
        Impl->Write(stream);
      }

      private:

      TVar(TImpl *impl)
          : Impl(impl, TImpl::Delete) {
        assert(impl);
      }

      void Init();

      std::shared_ptr<TImpl> Impl;

      template <typename TVal> friend TVal DynamicCast(const TVar &var);

      template <typename TVal> friend struct TRead;

    };  // TVar

    template <>
    struct TVar::TDt<TVar> {

      TVar static As(const TVar &rhs) {
        return rhs;
      }

    };

    class TVar::TVisitor {
      public:

      virtual ~TVisitor();

      virtual void operator()(const Var::TAddr *that) const = 0;
      virtual void operator()(const Var::TBool *that) const = 0;
      virtual void operator()(const Var::TDict *that) const = 0;
      virtual void operator()(const Var::TErr *that) const = 0;
      virtual void operator()(const Var::TFree *that) const = 0;
      virtual void operator()(const Var::TId *that) const = 0;
      virtual void operator()(const Var::TInt *that) const = 0;
      virtual void operator()(const Var::TList *that) const = 0;
      virtual void operator()(const Var::TMutable *that) const = 0;
      virtual void operator()(const Var::TObj *that) const = 0;
      virtual void operator()(const Var::TOpt *that) const = 0;
      virtual void operator()(const Var::TReal *that) const = 0;
      virtual void operator()(const Var::TSet *that) const = 0;
      virtual void operator()(const Var::TStr *that) const = 0;
      virtual void operator()(const Var::TTimeDiff *that) const = 0;
      virtual void operator()(const Var::TTimePnt *that) const = 0;
      virtual void operator()(const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      /* NOTE: TVariant is a non-pure default that throws, following the
         TUnknown precedent. Variant-aware visitors (compare, jsonify,
         orlyify, new_sabot, and the TLhsVisitor/TRhsVisitor dispatch in
         impl.cc) override it; the many other visitor subclasses need no
         per-file change for a type that has no orlyscript surface yet
         (Phase 3 of #95). */
      virtual void operator()(const Var::TVariant *) const {throw Base::TImpossibleError(HERE);}

      protected:

      TVisitor() {}

    };  // TVisitor

    class TVar::TDoubleVisitor {
      public:

      virtual ~TDoubleVisitor();

      virtual void operator()(const Var::TAddr *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TAddr *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TAddr *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TBool *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TBool *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TDict *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TDict *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TErr *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TErr *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TFree *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TFree *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TId *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TId *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TInt *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TInt *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TList *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TList *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TMutable *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TMutable *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TOpt *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TOpt *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TObj *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TObj *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TReal *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TReal *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TSet *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TSet *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TStr *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TStr *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TTimeDiff *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TTimePnt *rhs) const = 0;
      void operator()(const Var::TTimePnt *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TAddr *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TBool *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TDict *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TErr *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TFree *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TId *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TInt *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TList *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TMutable *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TOpt *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TObj *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TReal *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TSet *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TStr *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TTimeDiff *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TTimePnt *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const Var::TUnknown *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      /* NOTE: The TVariant row and column are virtual with throwing
         defaults (rather than pure) so that the only subclass --
         TCompareDoubleVisitor (impl.cc) -- can override just the cells
         it needs (the (TVariant, TVariant) diagonal and the cross-type
         CompareType cells). A variant is only ever compared against a
         variant or another type when it lives in a Var-level set, so the
         meaningful cells are the diagonal (ordering) and cross-type
         (type-precedence) ones; everything else defaults to throw,
         mirroring the TUnknown row/column. */
      virtual void operator()(const Var::TVariant *lhs, const Var::TAddr *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TBool *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TDict *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TErr *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TFree *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TId *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TInt *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TList *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TMutable *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TOpt *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TObj *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TReal *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TSet *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TStr *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TTimeDiff *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TTimePnt *rhs) const = 0;
      virtual void operator()(const Var::TVariant *lhs, const Var::TVariant *rhs) const = 0;
      void operator()(const Var::TVariant *, const Var::TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const Var::TAddr *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TBool *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TDict *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TErr *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TFree *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TId *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TInt *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TList *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TMutable *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TOpt *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TObj *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TReal *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TSet *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TStr *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TTimeDiff *lhs, const Var::TVariant *rhs) const = 0;
      virtual void operator()(const Var::TTimePnt *lhs, const Var::TVariant *rhs) const = 0;
      void operator()(const Var::TUnknown *, const Var::TVariant *) const {throw Base::TImpossibleError(HERE);}

      protected:

      TDoubleVisitor() {}

    };  // TDoubleVisitor


  inline std::ostream &operator<<(std::ostream &stream, const TVar &var) {
    var.Write(stream);
    return stream;
  }

  }  // Var

}  // Orly

namespace std {

  /* A standard hasher for Orly::Var::TVar. */
  template <>
  struct hash<Orly::Var::TVar> {

    typedef size_t result_type;
    typedef Orly::Var::TVar argument_type;

    result_type operator()(const argument_type &that) const {
      return that.GetHash();
    }

  };  // hash<Orly::Var::TVar>

}  // std
