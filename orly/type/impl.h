/* <orly/type/impl.h>

   The static type system's foundation. `TType` is the public-facing
   handle (a `shared_ptr<TImpl>`); `TImpl` is the abstract base each
   concrete type derives from. `TVisitor` dispatches on one type,
   `TDoubleVisitor` on a pair -- the same pattern every operator
   type-check visitor in this directory extends. Forward-declares
   every concrete type leaf so other headers don't need to include
   them transitively.

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
#include <memory>
#include <ostream>

#include <base/class_traits.h>
#include <base/impossible_error.h>
#include <orly/pos_range.h>

namespace Orly {

  namespace Type {

    /* TODO */
    class TAddr;
    class TAny;
    class TBool;
    class TDict;
    class TErr;
    class TFunc;
    class TId;
    class TInt;
    class TList;
    class TMutable;
    class TObj;
    class TOpt;
    class TReal;
    class TSeq;
    class TSet;
    class TStr;
    class TTimeDiff;
    class TTimePnt;
    class TUnknown;
    class TVariant;

    /* TOOD */
    class TType {
      public:

      /* TODO */
      class TVisitor;

      /* TODO */
      class TDoubleVisitor;

      /* TODO */
      class TImpl : public std::enable_shared_from_this<TImpl> {
        NO_COPY(TImpl);


        public:

        /* TODO */
        TType AsType() const {
          return TType(shared_from_this());
        }

        protected:

        /* TODO */
        TImpl() {}

        /* TODO */
        virtual ~TImpl() {}

        /* TODO */
        virtual void Accept(const TVisitor &visitor) const = 0;

        /* TODO */
        virtual void Write(std::ostream &) const = 0;

        /* TODO */
        friend class TType;

      };  // TImpl

      /* TODO */
      TType() {
        Init();
      }

      /* TODO */
      TType(TType &&that) {
        Init();
        std::swap(Impl, that.Impl);
      }

      /* TODO */
      TType(const TType &that) {
        Impl = that.Impl;
      }

      /* TODO */
      ~TType() {}

      /* TODO */
      TType &operator=(TType &&that) {
        std::swap(Impl, that.Impl);
        return *this;
      }

      /* TODO */
      TType &operator=(const TType &that) {
        return *this = TType(that);
      }

      /* TODO */
      bool operator==(const TType &that) const {
        assert(Impl);
        assert(that.Impl);
        return Impl.get() == that.Impl.get();
      }

      /* TODO */
      bool operator!=(const TType &that) const {
        assert(Impl);
        assert(that.Impl);
        return Impl.get() != that.Impl.get();
      }

      /* TODO */
      operator bool() const;

      /* TODO */
      void Accept(const TVisitor &visitor) const {
        assert(Impl);
        Impl->Accept(visitor);
      }

      /* TODO */
      static void Accept(const TType &lhs, const TType &rhs, const TDoubleVisitor &double_visitor);

      template <typename TTarget>
      const TTarget *As() const;

      /* TODO */
      size_t GetHash() const {
        assert(Impl);
        return reinterpret_cast<size_t>(Impl.get());
      }

      /* TODO: Write a demangler */
      /* Mangling rules
         NOTES:
           - All compound types have a capital letter.
           - All compounds which contain more than one type have a count of types following the compound identifier.
      Addr -> 'A' size (('a'|'d') type)*
      Any  -> 'a'
      Bool -> 'b'
      Dict -> 'D' key_type val_type
      Err  -> 'E' err_type
      Func -> 'F' ret_type num_args MangleElemMap(args)
      Id   -> 'I'
      Int  -> 'i'
      List -> 'L' elem_type
      Mutable -> "M2"addr_type value_type
      Obj  -> 'O' MangleElemMap(members)
      Opt  -> 'P' inner_type
      Real -> 'r'
      seq  -> 'Q' inner_type
      set  -> 'S' inner_type
      str  -> 's'
      time_diff -> 't'
      time_pnt  -> 'T'
      Variant -> 'V' MangleElemMap(tags)
      */
      std::string GetMangledName() const;

      template <typename TTarget>
      bool Is() const;

      template <typename TTarget>
      const TTarget *TryAs() const;

      /* TODO */
      void Write(std::ostream &stream) const {
        Impl->Write(stream);
      }

      private:

      /* TODO */
      TType(const std::shared_ptr<const TImpl> &impl) : Impl(impl) {
        assert(impl);
      }

      /* TODO */
      void Init();

      /* TODO */
      std::shared_ptr<const TImpl> Impl;

    };  // TType

    /* TODO */
    template <typename TVal>
    struct TDt;

    /* TODO */
    class TType::TVisitor {
      public:

      /* TODO */
      virtual ~TVisitor();

      virtual void operator()(const TAddr     *that) const = 0;
      virtual void operator()(const TAny      *that) const = 0;
      virtual void operator()(const TBool     *that) const = 0;
      virtual void operator()(const TDict     *that) const = 0;
      virtual void operator()(const TErr      *that) const = 0;
      virtual void operator()(const TFunc     *that) const = 0;
      virtual void operator()(const TId       *that) const = 0;
      virtual void operator()(const TInt      *that) const = 0;
      virtual void operator()(const TList     *that) const = 0;
      virtual void operator()(const TMutable  *that) const = 0;
      virtual void operator()(const TObj      *that) const = 0;
      /* NOTE: TVariant is NOT pure-virtual (unlike the other leaves). #95 added it as a purely
               additive type with no orlyscript surface yet, so making it non-pure means the many
               existing TVisitor subclasses don't each need a case. Visitors that genuinely handle
               variants (orlyify, gen_code, get_prec, has_optional, object_collector, unwrap, the
               type-level sabot, ...) override it explicitly; everyone else inherits this throw,
               mirroring the long-standing TUnknown treatment below. */
      virtual void operator()(const TVariant  *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TOpt      *that) const = 0;
      virtual void operator()(const TReal     *that) const = 0;
      virtual void operator()(const TSeq      *that) const = 0;
      virtual void operator()(const TSet      *that) const = 0;
      virtual void operator()(const TStr      *that) const = 0;
      virtual void operator()(const TTimeDiff *that) const = 0;
      virtual void operator()(const TTimePnt  *that) const = 0;
      void operator()(const TUnknown *) const {throw Base::TImpossibleError(HERE);}

      protected:

      /* TODO */
      TVisitor();

    };  // TType::TVisitor;

    /* TODO */
    class TType::TDoubleVisitor {
      public:

      /* TODO */
      virtual ~TDoubleVisitor() {}

      /* TODO */
      virtual void operator()(const TAddr *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TAddr *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TAddr *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TAddr *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TAny *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TAny *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TAny *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TAny *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TBool *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TBool *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TBool *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TBool *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TDict *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TDict *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TDict *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TDict *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TErr *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TErr *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TErr *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TErr *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TFunc *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TFunc *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TFunc *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TFunc *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TId *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TId *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TId *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TId *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TInt *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TInt *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TInt *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TInt *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TList *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TList *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TList *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TList *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TMutable *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TMutable *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TMutable *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TMutable *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TOpt *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TOpt *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TOpt *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TOpt *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TObj *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TObj *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TObj *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TObj *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TAddr *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TAny *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TBool *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TDict *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TErr *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TFunc *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TId *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TInt *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TList *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TMutable *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TOpt *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TObj *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TReal *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TSet *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TSeq *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TStr *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TTimeDiff *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TVariant *lhs, const TTimePnt *rhs) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TVariant *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TReal *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TReal *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TReal *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TReal *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TSet *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TSet *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TSet *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TSet *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TSeq *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TSeq *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TSeq *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TSeq *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TStr *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TStr *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TStr *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TStr *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TTimeDiff *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TTimeDiff *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TTimeDiff *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TTimeDiff *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TTimePnt *lhs, const TAddr *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TAny *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TBool *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TDict *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TErr *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TFunc *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TId *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TInt *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TList *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TMutable *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TOpt *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TObj *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TVariant *rhs) const {throw Base::TImpossibleError(HERE);}
      virtual void operator()(const TTimePnt *lhs, const TReal *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TSet *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TSeq *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TStr *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TTimeDiff *rhs) const = 0;
      virtual void operator()(const TTimePnt *lhs, const TTimePnt *rhs) const = 0;
      void operator()(const TTimePnt *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TAddr *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TAny *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TBool *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TDict *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TErr *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TFunc *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TId *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TInt *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TList *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TMutable *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TOpt *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TObj *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TVariant *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TReal *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TSet *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TSeq *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TStr *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TTimeDiff *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TTimePnt *) const {throw Base::TImpossibleError(HERE);}
      void operator()(const TUnknown *, const TUnknown *) const {throw Base::TImpossibleError(HERE);}

      protected:

      /* TODO */
      TDoubleVisitor() {}

    };  // TDoubleVisitor

  }  // Type

}  // Orly

namespace std {

  /* TODO */
  inline ostream &operator<<(ostream &stream, const Orly::Type::TType &that) {
    that.Write(stream);
    return stream;
  }

  /* A standard hasher for Orly::Type::TType. */
  template <>
  struct hash<Orly::Type::TType> {

    typedef size_t result_type;
    typedef Orly::Type::TType argument_type;

    result_type operator()(const argument_type &that) const {
      return that.GetHash();
    }

  };  // hash<Orly::Type::TType>

}  // std
