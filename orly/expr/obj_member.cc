/* <orly/expr/obj_member.cc>

   Implements <orly/expr/obj_member.h>

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

#include <orly/expr/obj_member.h>

#include <base/as_str.h>
#include <orly/error.h>
#include <orly/expr/visitor.h>
#include <orly/pos_range.h>
#include <orly/type/impl.h>
#include <orly/type/part.h>
#include <orly/type/unroll.h>
#include <orly/type/unwrap.h>
#include <orly/type/unwrap_visitor.h>
#include <orly/type/variant.h>

using namespace Orly;
using namespace Orly::Expr;

TObjMember::TPtr TObjMember::New(const TExpr::TPtr &expr, const std::string &name, const TPosRange &pos_range) {
  return TObjMember::TPtr(new TObjMember(expr, name, pos_range));
}

TObjMember::TObjMember(const TExpr::TPtr &expr, const std::string &name, const TPosRange &pos_range)
    : TUnary(expr, pos_range), Name(name) {}

void TObjMember::Accept(const TVisitor &visitor) const {
  visitor(this);
}

Type::TType TObjMember::GetTypeImpl() const {
  /* `e.<Tag>` is overloaded: on a record operand it is field access
     (handled by the visitor below); on a variant operand it is the
     #95 guarded payload accessor -- it yields the `<Tag>` arm's declared
     payload type. The runtime value is valid only when the active arm is
     `<Tag>` (the generated native struct's GetV<Tag>() asserts this), so
     this is normally gated by `e is <Tag>`. We disambiguate on the
     operand type here rather than in a separate grammar production
     (which would collide with postfix_obj_member as `expr dot name`). */
  if (const Type::TVariant *variant = Type::Unwrap(GetExpr()->GetType()).TryAs<Type::TVariant>()) {
    const Type::TVariantElems &elems = variant->GetElems();
    auto iter = elems.find(Name);
    if (iter == elems.end()) {
      std::string msg = Base::AsStr("variant payload accessor \".", Name, "\" names a tag that is not an arm of the operand variant type");
      throw TExprError(HERE, GetPosRange(), msg.c_str());
    }
    /* For a recursive variant the declared payload contains self-
       references; the surfaced type is one unrolling of the mu-type
       (every self-reference becomes the variant type itself, #103). */
    return Type::Unroll(iter->second, variant->AsType());
  }
  class TObjMemberTypeVisitor
      : public Type::TUnwrapVisitor {
    NO_COPY(TObjMemberTypeVisitor);
    public:
    TObjMemberTypeVisitor(Type::TType &type, const std::string &name, const TPosRange &pos_range)
        : Type::TUnwrapVisitor(type, pos_range), Name(name) {}
    virtual void operator()(const Type::TAddr     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TBool     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TDict     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TId       *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TInt      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TList     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TMutable  *that) const {
      auto parts = that->GetParts();
      parts.push_back(Type::TObjMember::Get(Name));
      that->GetVal().Accept(*this);
      Type = Type::TMutable::Get(that->GetAddr(), parts, Type, that->GetSrcAtAddr());
    }
    virtual void operator()(const Type::TObj      *that) const {
      auto &elem_map = that->GetElems();
      auto iter = elem_map.find(Name);
      if (iter == elem_map.end()) {
        throw TExprError(HERE, PosRange, "No such field in object");
      }
      Type = iter->second;
    }
    virtual void operator()(const Type::TReal     *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TSet      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TStr      *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimeDiff *) const { throw TExprError(HERE, PosRange); }
    virtual void operator()(const Type::TTimePnt  *) const { throw TExprError(HERE, PosRange); }
    private:
    const std::string &Name;
  };  // TObjMemberTypeVisitor
  Type::TType type;
  GetExpr()->GetType().Accept(TObjMemberTypeVisitor(type, Name, GetPosRange()));
  return type;
}

const std::string &TObjMember::GetName() const {
  return Name;
}
