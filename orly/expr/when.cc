/* <orly/expr/when.cc>

   Implements <orly/expr/when.h>

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

#include <orly/expr/when.h>

#include <unordered_set>

#include <base/as_str.h>
#include <orly/error.h>
#include <orly/expr/visitor.h>
#include <orly/type.h>
#include <orly/type/equal_visitor.h>
#include <orly/type/impl.h>
#include <orly/type/unwrap.h>
#include <orly/type/variant.h>

using namespace Orly;
using namespace Orly::Expr;

static TWhen::TExprVec MakeContainer(const TExpr::TPtr &operand, const TWhen::TExprVec &bodies) {
  TWhen::TExprVec out;
  out.reserve(bodies.size() + 1U);
  out.push_back(Base::AssertTrue(operand));
  for (const auto &body : bodies) {
    out.push_back(Base::AssertTrue(body));
  }
  return out;
}

TWhen::TPtr TWhen::New(
    const TExpr::TPtr &operand,
    const std::vector<std::string> &tags,
    const TExprVec &bodies,
    const TPosRange &pos_range) {
  return TWhen::TPtr(new TWhen(operand, tags, bodies, pos_range));
}

TWhen::TWhen(
    const TExpr::TPtr &operand,
    const std::vector<std::string> &tags,
    const TExprVec &bodies,
    const TPosRange &pos_range)
    : TNAry(MakeContainer(operand, bodies), pos_range), Tags(tags) {
  assert(tags.size() == bodies.size());
}

void TWhen::Accept(const TVisitor &visitor) const {
  visitor(this);
}

size_t TWhen::GetArmWhich(size_t arm_idx) const {
  const Type::TVariant *variant = Type::Unwrap(GetOperand()->GetType()).TryAs<Type::TVariant>();
  if (!variant) {
    throw TExprError(HERE, GetPosRange(), "`when` operand is not a variant type");
  }
  const std::string &tag = Tags[arm_idx];
  size_t idx = 0;
  for (const auto &elem : variant->GetElems()) {
    if (elem.first == tag) {
      return idx;
    }
    ++idx;
  }
  std::string msg = Base::AsStr("`when` arm `", tag, "` names a tag that is not an arm of the operand variant type");
  throw TExprError(HERE, GetPosRange(), msg.c_str());
}

Type::TType TWhen::GetTypeImpl() const {
  /* Operand must be a variant. */
  const Type::TVariant *variant = Type::Unwrap(GetOperand()->GetType()).TryAs<Type::TVariant>();
  if (!variant) {
    throw TExprError(HERE, GetPosRange(), "`when` operand must be a variant type");
  }
  const Type::TVariantElems &elems = variant->GetElems();

  /* Exhaustiveness: every arm names a real tag, with no duplicates, and
     every tag of the variant has an arm. */
  std::unordered_set<std::string> seen;
  for (const auto &tag : Tags) {
    if (elems.find(tag) == elems.end()) {
      std::string msg = Base::AsStr("`when` arm `", tag, "` is not a tag of the operand variant type");
      throw TExprError(HERE, GetPosRange(), msg.c_str());
    }
    if (!seen.insert(tag).second) {
      std::string msg = Base::AsStr("`when` has a duplicate arm for tag `", tag, "`");
      throw TExprError(HERE, GetPosRange(), msg.c_str());
    }
  }
  for (const auto &elem : elems) {
    if (seen.find(elem.first) == seen.end()) {
      std::string msg = Base::AsStr("`when` is not exhaustive: missing arm for tag `", elem.first, "`");
      throw TExprError(HERE, GetPosRange(), msg.c_str());
    }
  }
  if (Tags.empty()) {
    throw TExprError(HERE, GetPosRange(), "`when` must have at least one arm");
  }

  /* Result type: the unified type of the arm bodies. Mirrors the join in
     TIfElse::GetTypeImpl (Type::TEqualVisitor). */
  class TWhenJoinVisitor : public Type::TEqualVisitor {
    NO_COPY(TWhenJoinVisitor);
    public:
    TWhenJoinVisitor(Type::TType &type, const TPosRange &pos_range) : TEqualVisitor(type, pos_range) {}
    virtual void operator()(const Type::TAny *, const Type::TAny *) const {
      throw TExprError(HERE, PosRange, "`when` arms do not resolve to a single result type");
    }
    virtual void operator()(const Type::TAny *, const Type::TAddr     *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny *, const Type::TBool     *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny *, const Type::TDict     *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny *, const Type::TId       *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny *, const Type::TInt      *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny *, const Type::TList     *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny *, const Type::TMutable  *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny *, const Type::TObj      *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny *, const Type::TReal     *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny *, const Type::TSet      *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny *, const Type::TSeq      *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny *, const Type::TStr      *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny *, const Type::TTimeDiff *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAny *, const Type::TTimePnt  *rhs) const { Type = rhs->AsType(); }
    virtual void operator()(const Type::TAddr     *lhs, const Type::TAny *) const final { Type = lhs->AsType(); }
    virtual void operator()(const Type::TBool     *lhs, const Type::TAny *) const final { Type = lhs->AsType(); }
    virtual void operator()(const Type::TDict     *lhs, const Type::TAny *) const final { Type = lhs->AsType(); }
    virtual void operator()(const Type::TId       *lhs, const Type::TAny *) const final { Type = lhs->AsType(); }
    virtual void operator()(const Type::TInt      *lhs, const Type::TAny *) const final { Type = lhs->AsType(); }
    virtual void operator()(const Type::TList     *lhs, const Type::TAny *) const final { Type = lhs->AsType(); }
    virtual void operator()(const Type::TMutable  *lhs, const Type::TAny *) const final { Type = lhs->AsType(); }
    virtual void operator()(const Type::TObj      *lhs, const Type::TAny *) const final { Type = lhs->AsType(); }
    virtual void operator()(const Type::TReal     *lhs, const Type::TAny *) const final { Type = lhs->AsType(); }
    virtual void operator()(const Type::TSet      *lhs, const Type::TAny *) const final { Type = lhs->AsType(); }
    virtual void operator()(const Type::TSeq      *lhs, const Type::TAny *) const final { Type = lhs->AsType(); }
    virtual void operator()(const Type::TStr      *lhs, const Type::TAny *) const final { Type = lhs->AsType(); }
    virtual void operator()(const Type::TTimeDiff *lhs, const Type::TAny *) const final { Type = lhs->AsType(); }
    virtual void operator()(const Type::TTimePnt  *lhs, const Type::TAny *) const final { Type = lhs->AsType(); }
    virtual void operator()(const Type::TAddr     *lhs, const Type::TAddr     *) const { Type = lhs->AsType(); }
    virtual void operator()(const Type::TBool     *lhs, const Type::TBool     *) const { Type = lhs->AsType(); }
    virtual void operator()(const Type::TDict     *lhs, const Type::TDict     *) const { Type = lhs->AsType(); }
    virtual void operator()(const Type::TId       *lhs, const Type::TId       *) const { Type = lhs->AsType(); }
    virtual void operator()(const Type::TInt      *lhs, const Type::TInt      *) const { Type = lhs->AsType(); }
    virtual void operator()(const Type::TList     *lhs, const Type::TList     *) const { Type = lhs->AsType(); }
    virtual void operator()(const Type::TObj      *lhs, const Type::TObj      *) const { Type = lhs->AsType(); }
    virtual void operator()(const Type::TReal     *lhs, const Type::TReal     *) const { Type = lhs->AsType(); }
    virtual void operator()(const Type::TSet      *lhs, const Type::TSet      *) const { Type = lhs->AsType(); }
    virtual void operator()(const Type::TStr      *lhs, const Type::TStr      *) const { Type = lhs->AsType(); }
    virtual void operator()(const Type::TTimeDiff *lhs, const Type::TTimeDiff *) const { Type = lhs->AsType(); }
    virtual void operator()(const Type::TTimePnt  *lhs, const Type::TTimePnt  *) const { Type = lhs->AsType(); }
  };  // TWhenJoinVisitor

  Type::TType type = GetArmBody(0)->GetType();
  for (size_t arm_idx = 1; arm_idx < Tags.size(); ++arm_idx) {
    Type::TType joined;
    Type::TType::Accept(type, GetArmBody(arm_idx)->GetType(), TWhenJoinVisitor(joined, GetPosRange()));
    type = joined;
  }
  return type;
}
