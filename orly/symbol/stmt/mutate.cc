/* <orly/symbol/stmt/mutate.cc>

   Implements <orly/symbol/stmt/mutate.h>

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

#include <orly/symbol/stmt/mutate.h>

#include <string_view>

#include <base/as_str.h>
#include <orly/error.h>
#include <orly/type/add_visitor.h>
#include <orly/type/comp_visitor.h>
#include <orly/type/div_visitor.h>
#include <orly/type/equal_visitor.h>
#include <orly/type/exp_visitor.h>
#include <orly/type/infix_visitor.h>
#include <orly/type/logical_ops_visitor.h>
#include <orly/type/min_max_visitor.h>
#include <orly/type/mod_visitor.h>
#include <orly/type/mult_visitor.h>
#include <orly/type/set_ops_visitor.h>
#include <orly/type/sub_visitor.h>
#include <orly/type/unwrap.h>

using namespace Orly;
using namespace Orly::Symbol;
using namespace Orly::Symbol::Stmt;

template <typename TDoubleVisitor>
class TMutateTypeVisitor
    : public TDoubleVisitor {
  public:

  static_assert(std::is_base_of<Type::TInfixVisitor, TDoubleVisitor>::value, "TDoubleVisitor must be inherited from TInfixVisitor");

  TMutateTypeVisitor(Type::TType &type, const TPosRange &pos_range)
      : TDoubleVisitor(type, pos_range) {}

  virtual void operator()(const Type::TAddr *, const Type::TOpt *) const { ThrowOptError(); }
  virtual void operator()(const Type::TBool *, const Type::TOpt *) const { ThrowOptError(); }
  virtual void operator()(const Type::TDict *, const Type::TOpt *) const { ThrowOptError(); }
  virtual void operator()(const Type::TId   *, const Type::TOpt *) const { ThrowOptError(); }
  virtual void operator()(const Type::TInt  *, const Type::TOpt *) const { ThrowOptError(); }
  virtual void operator()(const Type::TList *, const Type::TOpt *) const { ThrowOptError(); }
  virtual void operator()(const Type::TObj  *, const Type::TOpt *) const { ThrowOptError(); }
  virtual void operator()(const Type::TReal *, const Type::TOpt *) const { ThrowOptError(); }
  virtual void operator()(const Type::TSet  *, const Type::TOpt *) const { ThrowOptError(); }
  virtual void operator()(const Type::TStr  *, const Type::TOpt *) const { ThrowOptError(); }
  virtual void operator()(const Type::TTimeDiff *, const Type::TOpt *) const { ThrowOptError(); }
  virtual void operator()(const Type::TTimePnt  *, const Type::TOpt *) const { ThrowOptError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TAddr *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TBool *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TDict *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TId   *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TInt  *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TList *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TMutable *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TObj  *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TOpt  *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TReal *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TSet  *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TSeq  *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TStr  *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TTimeDiff *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSeq  *, const Type::TTimePnt  *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TAddr *, const Type::TSeq *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TBool *, const Type::TSeq *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TDict *, const Type::TSeq *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TId   *, const Type::TSeq *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TInt  *, const Type::TSeq *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TList *, const Type::TSeq *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TObj  *, const Type::TSeq *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TOpt  *, const Type::TSeq *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TReal *, const Type::TSeq *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TSet  *, const Type::TSeq *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TStr  *, const Type::TSeq *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TTimeDiff *, const Type::TSeq *) const { ThrowSeqError(); }
  virtual void operator()(const Type::TTimePnt  *, const Type::TSeq *) const { ThrowSeqError(); }

  private:

  void ThrowOptError() const {
    throw TExprError(HERE, TDoubleVisitor::PosRange, "Performing a mutation to a non-optional value with an optional is not allowed.");
  }

  void ThrowSeqError() const {
    throw TExprError(HERE, TDoubleVisitor::PosRange, "Performing a mutation involving sequences is not allowed.");
  }

};  // TMutateTypeVisitor

TMutate::TPtr TMutate::New(
    const TStmtArg::TPtr &lhs,
    TMutator mutator,
    const TStmtArg::TPtr &rhs,
    const TPosRange &pos_range) {
  return TMutate::TPtr(new TMutate(lhs, mutator, rhs, pos_range));
}

void TMutate::Accept(const TVisitor &visitor) const {
  visitor(this);
}

const TMutator &TMutate::GetMutator() const {
  return Mutator;
}

void TMutate::TypeCheck() const {
  Type::TType dummy;
  /* NOTE: It would be nice to use a templatized helper function for this but that would require
           the helper function and the TMutateTypeVisitor to be in the header.
           I could try to figure out how to use extern templates... not worth it at the moment. */
  switch (Mutator) {
    case TMutator::Add: {
      Type::TType::Accept(
          GetLhs()->GetExpr()->GetType(),
          GetRhs()->GetExpr()->GetType(),
          TMutateTypeVisitor<Type::TAddVisitor>(dummy, GetPosRange()));
      break;
    }
    case TMutator::And: {
      Type::TType::Accept(
          GetLhs()->GetExpr()->GetType(),
          GetRhs()->GetExpr()->GetType(),
          TMutateTypeVisitor<Type::TLogicalOpsVisitor>(dummy, GetPosRange()));
      break;
    }
    case TMutator::Assign: {
      const Type::TType lhs_type = GetLhs()->GetExpr()->GetType();
      const Type::TType rhs_type = GetRhs()->GetExpr()->GetType();
      try {
        /* TEqCompVisitor is the correct check: assignment requires the new
           value's type to equal the stored value's type. */
        Type::TType::Accept(lhs_type, rhs_type,
            TMutateTypeVisitor<Type::TEqCompVisitor>(dummy, GetPosRange()));
      } catch (const TExprError &ex) {
        /* The visitor's specific diagnostics (optionals in addresses,
           sequences, object subsets, ...) pass through untouched; only the
           generic mismatch is upgraded to name both types (issue #314). */
        if (!std::string_view(ex.what()).ends_with(TExprError::DefaultMessage)) {
          throw;
        }
        throw TExprError(HERE, GetPosRange(), Base::AsStr(
            "cannot assign a value of type ", Type::UnwrapMutable(rhs_type),
            " to a mutable holding ", Type::UnwrapMutable(lhs_type)).c_str());
      }
      break;
    }
    case TMutator::Div: {
      Type::TType::Accept(
          GetLhs()->GetExpr()->GetType(),
          GetRhs()->GetExpr()->GetType(),
          TMutateTypeVisitor<Type::TDivVisitor>(dummy, GetPosRange()));
      break;
    }
    case TMutator::Exp: {
      Type::TType::Accept(
          GetLhs()->GetExpr()->GetType(),
          GetRhs()->GetExpr()->GetType(),
          TMutateTypeVisitor<Type::TExpVisitor>(dummy, GetPosRange()));
      break;
    }
    case TMutator::Intersection: {
      Type::TType::Accept(
          GetLhs()->GetExpr()->GetType(),
          GetRhs()->GetExpr()->GetType(),
          TMutateTypeVisitor<Type::TSetOpsVisitor>(dummy, GetPosRange()));
      break;
    }
    case TMutator::Max:
    case TMutator::Min: {
      Type::TType::Accept(
          GetLhs()->GetExpr()->GetType(),
          GetRhs()->GetExpr()->GetType(),
          TMutateTypeVisitor<Type::TMinMaxVisitor>(dummy, GetPosRange()));
      break;
    }
    case TMutator::Mod: {
      Type::TType::Accept(
          GetLhs()->GetExpr()->GetType(),
          GetRhs()->GetExpr()->GetType(),
          TMutateTypeVisitor<Type::TModVisitor>(dummy, GetPosRange()));
      break;
    }
    case TMutator::Mult: {
      Type::TType::Accept(
          GetLhs()->GetExpr()->GetType(),
          GetRhs()->GetExpr()->GetType(),
          TMutateTypeVisitor<Type::TMultVisitor>(dummy, GetPosRange()));
      break;
    }
    case TMutator::Or: {
      Type::TType::Accept(
          GetLhs()->GetExpr()->GetType(),
          GetRhs()->GetExpr()->GetType(),
          TMutateTypeVisitor<Type::TLogicalOpsVisitor>(dummy, GetPosRange()));
      break;
    }
    case TMutator::Sub: {
      Type::TType::Accept(
          GetLhs()->GetExpr()->GetType(),
          GetRhs()->GetExpr()->GetType(),
          TMutateTypeVisitor<Type::TSubVisitor>(dummy, GetPosRange()));
      break;
    }
    case TMutator::SymmetricDiff: {
      Type::TType::Accept(
          GetLhs()->GetExpr()->GetType(),
          GetRhs()->GetExpr()->GetType(),
          TMutateTypeVisitor<Type::TSetOpsVisitor>(dummy, GetPosRange()));
      break;
    }
    case TMutator::Union: {
      Type::TType::Accept(
          GetLhs()->GetExpr()->GetType(),
          GetRhs()->GetExpr()->GetType(),
          TMutateTypeVisitor<Type::TSetOpsVisitor>(dummy, GetPosRange()));
      break;
    }
    case TMutator::Xor: {
      Type::TType::Accept(
          GetLhs()->GetExpr()->GetType(),
          GetRhs()->GetExpr()->GetType(),
          TMutateTypeVisitor<Type::TLogicalOpsVisitor>(dummy, GetPosRange()));
      break;
    }
  }  // switch (Mutator)
}

TMutate::TMutate(
    const TStmtArg::TPtr &lhs,
    TMutator mutator,
    const TStmtArg::TPtr &rhs,
    const TPosRange &pos_range)
      : TBinary(lhs, rhs, pos_range),
        Mutator(mutator) {}
