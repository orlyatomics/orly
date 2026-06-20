/* <orly/expr/as.cc>

   Implements <orly/expr/as.h>

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

#include <orly/expr/as.h>

#include <base/code_location.h>
#include <orly/error.h>
#include <orly/expr/dict.h>
#include <orly/expr/visitor.h>
#include <orly/pos_range.h>
#include <orly/type.h>
#include <orly/type/infix_visitor.h>
#include <orly/type/unroll.h>
#include <orly/type/unwrap.h>
#include <orly/type/unwrap_visitor.h>
#include <orly/type/variant.h>

using namespace Orly;
using namespace Orly::Expr;

TAs::TPtr TAs::New(const TExpr::TPtr &expr, const Type::TType &type, const TPosRange &pos_range) {
  return TAs::TPtr(new TAs(expr, type, pos_range));
}

TAs::TAs(const TExpr::TPtr &expr, const Type::TType &type, const TPosRange &pos_range)
    : TUnary(expr, pos_range), Type(type) {}

void TAs::Accept(const TVisitor &visitor) const {
  visitor(this);
}

Type::TType TAs::GetTypeImpl() const {
  class TAsTypeVisitor
      : public Type::TInfixVisitor {
    NO_COPY(TAsTypeVisitor);
    public:
    TAsTypeVisitor(Type::TType &type, const TPosRange &pos_range)
        : Type::TInfixVisitor(type, pos_range) {}
    virtual void operator()(const Type::TAddr     *lhs, const Type::TAddr     *rhs) const {
      if (lhs->GetElems().size() != rhs->GetElems().size()) {
        throw TExprError(HERE, PosRange, "Cannot cast an address to another address of different length.");
      }
      auto lhs_iter = lhs->GetElems().begin();
      auto rhs_iter = rhs->GetElems().begin();
      for (; lhs_iter != lhs->GetElems().end(); ++lhs_iter, ++rhs_iter) {
        Type::TType::Accept(lhs_iter->second, rhs_iter->second, *this);
      }
      Type = rhs->AsType();
    }
    virtual void operator()(const Type::TAddr     *, const Type::TBool     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TAddr     *, const Type::TDict     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TAddr     *, const Type::TId       *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TAddr     *, const Type::TInt      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TAddr     *, const Type::TList     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TAddr     *, const Type::TObj      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TAddr     *, const Type::TReal     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TAddr     *, const Type::TSet      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TAddr     *, const Type::TStr      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TAddr     *, const Type::TTimeDiff *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TAddr     *, const Type::TTimePnt  *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TAny      *, const Type::TAddr     *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny      *, const Type::TBool     *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny      *, const Type::TDict     *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny      *, const Type::TId       *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny      *, const Type::TInt      *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny      *, const Type::TList     *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny      *, const Type::TObj      *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny      *, const Type::TReal     *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny      *, const Type::TSet      *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny      *, const Type::TStr      *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny      *, const Type::TTimeDiff *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny      *, const Type::TTimePnt  *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TBool     *, const Type::TAddr     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TBool     *, const Type::TBool     *) const {
      Type = Type::TBool::Get();
    }
    virtual void operator()(const Type::TBool     *, const Type::TDict     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TBool     *, const Type::TId       *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TBool     *, const Type::TInt      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TBool     *, const Type::TList     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TBool     *, const Type::TObj      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TBool     *, const Type::TReal     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TBool     *, const Type::TSet      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TBool     *, const Type::TStr      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TBool     *, const Type::TTimeDiff *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TBool     *, const Type::TTimePnt  *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TDict     *, const Type::TAddr     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TDict     *, const Type::TBool     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TDict     *lhs, const Type::TDict     *rhs) const {
      Type::TType::Accept(lhs->GetKey(), rhs->GetKey(), *this);
      Type::TType::Accept(lhs->GetVal(), rhs->GetVal(), *this);
      Type = rhs->AsType();
    }
    virtual void operator()(const Type::TDict     *, const Type::TId       *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TDict     *, const Type::TInt      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TDict     *, const Type::TList     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TDict     *, const Type::TObj      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TDict     *, const Type::TReal     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TDict     *, const Type::TSet      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TDict     *, const Type::TStr      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TDict     *, const Type::TTimeDiff *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TDict     *, const Type::TTimePnt  *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TId       *, const Type::TAddr     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TId       *, const Type::TBool     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TId       *, const Type::TDict     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TId       *, const Type::TId       *) const {
      Type = Type::TId::Get();
    }
    virtual void operator()(const Type::TId       *, const Type::TInt      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TId       *, const Type::TList     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TId       *, const Type::TObj      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TId       *, const Type::TReal     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TId       *, const Type::TSet      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TId       *, const Type::TStr      *) const {
      Type = Type::TStr::Get();
    }
    virtual void operator()(const Type::TId       *, const Type::TTimeDiff *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TId       *, const Type::TTimePnt  *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TInt      *, const Type::TAddr     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TInt      *, const Type::TBool     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TInt      *, const Type::TDict     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TInt      *, const Type::TId       *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TInt      *, const Type::TInt      *) const {
      Type = Type::TInt::Get();
    }
    virtual void operator()(const Type::TInt      *, const Type::TList     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TInt      *, const Type::TObj      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TInt      *, const Type::TReal     *) const {
      Type = Type::TReal::Get();
    }
    virtual void operator()(const Type::TInt      *, const Type::TSet      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TInt      *, const Type::TStr      *) const {
      Type = Type::TStr::Get();
    }
    virtual void operator()(const Type::TInt      *, const Type::TTimeDiff *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TInt      *, const Type::TTimePnt  *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TList     *, const Type::TAddr     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TList     *, const Type::TBool     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TList     *, const Type::TDict     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TList     *, const Type::TId       *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TList     *, const Type::TInt      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TList     *lhs, const Type::TList     *rhs) const {
      Type::TType::Accept(lhs->GetElem(), rhs->GetElem(), *this);
      Type = rhs->AsType();
    }
    virtual void operator()(const Type::TList     *, const Type::TObj      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TList     *, const Type::TReal     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TList     *, const Type::TSet      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TList     *, const Type::TStr      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TList     *, const Type::TTimeDiff *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TList     *, const Type::TTimePnt  *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TObj      *, const Type::TAddr     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TObj      *, const Type::TBool     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TObj      *, const Type::TDict     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TObj      *, const Type::TId       *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TObj      *, const Type::TInt      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TObj      *, const Type::TList     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TObj      *lhs, const Type::TObj      *rhs) const {
      bool is_optional = false;
      if (!rhs->IsSubsetOf(lhs, is_optional)) {
        throw TExprError(HERE, PosRange, "The target object must be a subset of the source object");
      }
      Type = rhs->AsType();
    }
    /* A variant cast is either an identity cast or a widening (#104):
       `narrow as wide`, legal iff every arm of the source appears
       identically in the target (TVariant::IsWidenableTo -- tag set widens,
       payloads invariant). IsWidenableTo is reflexive, so it also covers the
       identity cast, whose original purpose was widening to an optional
       variant: `v as op_t?` -- the infix machinery unwraps the optional
       target to reach (variant, variant) here, then re-wraps the result,
       which is the only way to build a KNOWN optional variant (#118). The
       v1-scope guards (recursive variants, optional-target widening) are
       applied once, after the visitor, in GetTypeImpl. */
    virtual void operator()(const Type::TVariant  *lhs, const Type::TVariant  *rhs) const {
      if (!lhs->IsWidenableTo(rhs)) {
        throw TExprError(HERE, PosRange,
            "a variant can only be cast to its own type or widened to a superset variant type");
      }
      Type = rhs->AsType();
    }
    virtual void operator()(const Type::TObj      *, const Type::TReal     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TObj      *, const Type::TSet      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TObj      *, const Type::TStr      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TObj      *, const Type::TTimeDiff *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TObj      *, const Type::TTimePnt  *) const {
      Type = Type::TTimePnt::Get();
    }
    virtual void operator()(const Type::TReal     *, const Type::TAddr     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TReal     *, const Type::TBool     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TReal     *, const Type::TDict     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TReal     *, const Type::TId       *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TReal     *, const Type::TInt      *) const {
      Type = Type::TInt::Get();
    }
    virtual void operator()(const Type::TReal     *, const Type::TList     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TReal     *, const Type::TObj      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TReal     *, const Type::TReal     *) const {
      Type = Type::TReal::Get();
    }
    virtual void operator()(const Type::TReal     *, const Type::TSet      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TReal     *, const Type::TStr      *) const {
      Type = Type::TStr::Get();
    }
    virtual void operator()(const Type::TReal     *, const Type::TTimeDiff *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TReal     *, const Type::TTimePnt  *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSet      *, const Type::TAddr     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSet      *, const Type::TBool     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSet      *, const Type::TDict     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSet      *, const Type::TId       *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSet      *, const Type::TInt      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSet      *, const Type::TList     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSet      *, const Type::TObj      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSet      *, const Type::TReal     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSet      *lhs, const Type::TSet      *rhs) const {
      Type::TType::Accept(lhs->GetElem(), rhs->GetElem(), *this);
      Type = rhs->AsType();
    }
    virtual void operator()(const Type::TSet      *, const Type::TStr      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSet      *, const Type::TTimeDiff *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSet      *, const Type::TTimePnt  *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSeq      *, const Type::TAddr     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSeq      *, const Type::TBool     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSeq      *lhs, const Type::TDict     *rhs) const {
      class TTypeVisitor
          : public Type::TUnwrapVisitor {
        NO_COPY(TTypeVisitor);
        public:
        TTypeVisitor(
            Type::TType &type,
            const Type::TDict *dict,
            const TAsTypeVisitor &as_type_visitor,
            const TPosRange &pos_range)
              : Type::TUnwrapVisitor(type, pos_range),
                Dict(dict),
                AsTypeVisitor(as_type_visitor) {}
        virtual void operator()(const Type::TAddr     *that) const {
          auto &elems = that->GetElems();
          if (elems.size() != 2) {
            ThrowExprError(HERE);
          }
          Type::TType::Accept(elems[0].second, Dict->GetKey(), AsTypeVisitor);
          Type::TType::Accept(elems[1].second, Dict->GetVal(), AsTypeVisitor);
        }
        virtual void operator()(const Type::TBool     *) const { ThrowExprError(HERE); }
        virtual void operator()(const Type::TDict     *) const { ThrowExprError(HERE); }
        virtual void operator()(const Type::TId       *) const { ThrowExprError(HERE); }
        virtual void operator()(const Type::TInt      *) const { ThrowExprError(HERE); }
        virtual void operator()(const Type::TList     *) const { ThrowExprError(HERE); }
        virtual void operator()(const Type::TObj      *) const { ThrowExprError(HERE); }
        virtual void operator()(const Type::TOpt      *) const { ThrowExprError(HERE); }
        virtual void operator()(const Type::TReal     *) const { ThrowExprError(HERE); }
        virtual void operator()(const Type::TSet      *) const { ThrowExprError(HERE); }
        virtual void operator()(const Type::TSeq      *) const { ThrowExprError(HERE); }
        virtual void operator()(const Type::TStr      *) const { ThrowExprError(HERE); }
        virtual void operator()(const Type::TTimeDiff *) const { ThrowExprError(HERE); }
        virtual void operator()(const Type::TTimePnt  *) const { ThrowExprError(HERE); }
        private:
        void ThrowExprError(const Base::TCodeLocation &code_location) const {
          throw TExprError(code_location, PosRange,
              "Cast from sequence to a dictionary requires the elements of the sequence to be two-tuples");
        }
        const Type::TDict *Dict;
        const TAsTypeVisitor &AsTypeVisitor;
      };  // TTypeVisitor
      lhs->GetElem().Accept(TTypeVisitor(Type, rhs, *this, PosRange));
      Type = rhs->AsType();
    }
    virtual void operator()(const Type::TSeq      *, const Type::TId       *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSeq      *, const Type::TInt      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSeq      *lhs, const Type::TList     *rhs) const {
      Type::TType::Accept(lhs->GetElem(), rhs->GetElem(), *this);
      Type = rhs->AsType();
    }
    virtual void operator()(const Type::TSeq      *, const Type::TObj      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSeq      *, const Type::TReal     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSeq      *lhs, const Type::TSet      *rhs) const {
      Type::TType::Accept(lhs->GetElem(), rhs->GetElem(), *this);
      Type = rhs->AsType();
    }
    virtual void operator()(const Type::TSeq      *, const Type::TStr      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSeq      *, const Type::TTimeDiff *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSeq      *, const Type::TTimePnt  *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TStr      *, const Type::TAddr     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TStr      *, const Type::TBool     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TStr      *, const Type::TDict     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TStr      *, const Type::TId       *) const {
      Type = Type::TId::Get();
    }
    virtual void operator()(const Type::TStr      *, const Type::TInt      *) const {
      Type = Type::TInt::Get();
    }
    virtual void operator()(const Type::TStr      *, const Type::TList     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TStr      *, const Type::TObj      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TStr      *, const Type::TReal     *) const {
      Type = Type::TReal::Get();
    }
    virtual void operator()(const Type::TStr      *, const Type::TSet      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TStr      *, const Type::TStr      *) const {
      Type = Type::TStr::Get();
    }
    virtual void operator()(const Type::TStr      *, const Type::TTimeDiff *) const {
      Type = Type::TTimeDiff::Get();
    }
    virtual void operator()(const Type::TStr      *, const Type::TTimePnt  *) const {
      Type = Type::TTimePnt::Get();
    }
    virtual void operator()(const Type::TTimeDiff *, const Type::TAddr     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimeDiff *, const Type::TBool     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimeDiff *, const Type::TDict     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimeDiff *, const Type::TId       *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimeDiff *, const Type::TInt      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimeDiff *, const Type::TList     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimeDiff *, const Type::TObj      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimeDiff *, const Type::TReal     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimeDiff *, const Type::TSet      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimeDiff *, const Type::TStr      *) const {
      Type = Type::TStr::Get();
    }
    virtual void operator()(const Type::TTimeDiff *, const Type::TTimeDiff *) const {
      Type = Type::TTimeDiff::Get();
    }
    virtual void operator()(const Type::TTimeDiff *, const Type::TTimePnt  *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimePnt  *, const Type::TAddr     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimePnt  *, const Type::TBool     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimePnt  *, const Type::TDict     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimePnt  *, const Type::TId       *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimePnt  *, const Type::TInt      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimePnt  *, const Type::TList     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimePnt  *, const Type::TObj      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimePnt  *, const Type::TReal     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimePnt  *, const Type::TSet      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimePnt  *, const Type::TStr      *) const {
      Type = Type::TStr::Get();
    }
    virtual void operator()(const Type::TTimePnt  *, const Type::TTimeDiff *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimePnt  *, const Type::TTimePnt  *) const {
      Type = Type::TTimePnt::Get();
    }
  };  // TAsTypeVisitor
  Type::TType type;
  Type::TType::Accept(GetExpr()->GetType(), Type, TAsTypeVisitor(type, GetPosRange()));
  /* v1-scope guards for variant widening (#104). A genuine widening is a
     cast whose source and result both unwrap to a variant, but to DIFFERENT
     variant types (an identity cast -- including `v as v_t?` -- has the same
     unwrapped variant on both sides and is unaffected). Two corners are
     deferred past v1 and rejected here with a clear message rather than
     producing code that cannot be lowered:
       - recursive / mutually-recursive variants (#103/#116/#125): widening
         would have to rebuild through the boxed self-edge representation;
       - widening to an optional target (`narrow as wide?`): the rebuild
         produces the bare wide struct, with no optional-wrap path yet. */
  const Type::TVariant *src_variant = Type::Unwrap(GetExpr()->GetType()).TryAs<Type::TVariant>();
  /* The result is the bare wide variant, or `wide?` when the cast targeted
     an optional (`narrow as wide?`); reach the variant through the optional
     so the widen is detected either way. Unwrap (mutable/seq) does not strip
     optional -- that needs UnwrapOptional. */
  Type::TType result_inner = type.Is<Type::TOpt>() ? Type::UnwrapOptional(type) : type;
  const Type::TVariant *dst_variant = Type::Unwrap(result_inner).TryAs<Type::TVariant>();
  if (src_variant && dst_variant && src_variant != dst_variant) {
    /* A variant's self-reference is bound by the variant itself, so it is
       "free" only at the arm-payload level -- check each arm's payload, the
       same level code_gen tests for the recursive `GetV<Tag>` template
       (orly/code_gen/variant_access.cc). */
    auto is_recursive = [](const Type::TVariant *v) {
      for (const auto &arm : v->GetElems()) {
        if (Type::HasFreeSelfRef(arm.second) || Type::HasGroupRef(arm.second)) {
          return true;
        }
      }
      return false;
    };
    if (is_recursive(src_variant) || is_recursive(dst_variant)) {
      throw TExprError(HERE, GetPosRange(),
          "widening of recursive variants is not yet supported (#104)");
    }
    if (type.Is<Type::TOpt>()) {
      throw TExprError(HERE, GetPosRange(),
          "widening to an optional variant type is not yet supported (#104)");
    }
  }
  return type;
}
