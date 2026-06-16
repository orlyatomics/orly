/* <orly/symbol/stmt/new_and_delete.cc>

   Implements <orly/symbol/stmt/new_and_delete.h>

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

#include <orly/symbol/stmt/new_and_delete.h>

#include <orly/error.h>
#include <orly/type/unroll.h>
#include <orly/type/unwrap_visitor.h>

using namespace Orly;
using namespace Orly::Symbol;
using namespace Orly::Symbol::Stmt;

class TAddressTypeVisitor
    : public Type::TUnwrapVisitor {
  NO_COPY(TAddressTypeVisitor);
  public:
  TAddressTypeVisitor(Type::TType &type, const TPosRange &pos_range)
      : Type::TUnwrapVisitor(type, pos_range) {}
  virtual void operator()(const Type::TAddr     *) const { /* DO NOTHING */ }
  virtual void operator()(const Type::TBool     *) const { throw TExprError(HERE, PosRange); }
  virtual void operator()(const Type::TDict     *) const { throw TExprError(HERE, PosRange); }
  virtual void operator()(const Type::TId       *) const { throw TExprError(HERE, PosRange); }
  virtual void operator()(const Type::TInt      *) const { throw TExprError(HERE, PosRange); }
  virtual void operator()(const Type::TList     *) const { throw TExprError(HERE, PosRange); }
  virtual void operator()(const Type::TMutable  *) const { throw TExprError(HERE, PosRange); }
  virtual void operator()(const Type::TObj      *) const { throw TExprError(HERE, PosRange); }
  virtual void operator()(const Type::TReal     *) const { throw TExprError(HERE, PosRange); }
  virtual void operator()(const Type::TSet      *) const { throw TExprError(HERE, PosRange); }
  virtual void operator()(const Type::TStr      *) const { throw TExprError(HERE, PosRange); }
  virtual void operator()(const Type::TTimeDiff *) const { throw TExprError(HERE, PosRange); }
  virtual void operator()(const Type::TTimePnt  *) const { throw TExprError(HERE, PosRange); }
};  // TAddressTypeVisitor

TDelete::TPtr TDelete::New(const TStmtArg::TPtr &stmt_arg, Type::TType value_type, const TPosRange &pos_range) {
  return TDelete::TPtr(new TDelete(stmt_arg, value_type, pos_range));
}

void TDelete::Accept(const TVisitor &visitor) const {
  visitor(this);
}

void TDelete::TypeCheck() const {
  Type::TType dummy;
  GetStmtArg()->GetExpr()->GetType().Accept(TAddressTypeVisitor(dummy, GetPosRange()));
}

TDelete::TDelete(const TStmtArg::TPtr &stmt_arg, Type::TType value_type, const TPosRange &pos_range)
    : TUnary(stmt_arg, pos_range), ValueType(value_type) {}

TNew::TPtr TNew::New(
    const TStmtArg::TPtr &lhs,
    const TStmtArg::TPtr &rhs,
    const TPosRange &pos_range) {
  return TNew::TPtr(new TNew(lhs, rhs, pos_range));
}

void TNew::Accept(const TVisitor &visitor) const {
  visitor(this);
}

void TNew::TypeCheck() const {
  Type::TType dummy;
  GetLhs()->GetExpr()->GetType().Accept(TAddressTypeVisitor(dummy, GetPosRange()));
  /* NOTE: Maybe this should be type check rather than get type. But for now,
           GetType() calls ComputeType() which does the type check. */
  GetRhs()->GetExpr()->GetType();
  /* Recursive variants -- self-recursive (Type::TSelfRef) and mutually-recursive
     (Type::TGroupRef) alike -- are storable (issue #115): a self-recursive value
     closes the cycle with the finite sabot TSelfRef leaf, and a mutual-group
     value is lowered by inlining the member to its de Bruijn form
     (orly/type/new_sabot.cc), so it rides the same machinery. Either way the
     value read/writes through the boxed payload representation. A mutual payload
     shape codegen does not yet handle (a group ref under a nested variant, etc.)
     is reported with a clear message at code generation. */
}

TNew::TNew(
    const TStmtArg::TPtr &lhs,
    const TStmtArg::TPtr &rhs,
    const TPosRange &pos_range)
      : TBinary(lhs, rhs, pos_range) {}
