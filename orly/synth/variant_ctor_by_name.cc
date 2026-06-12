/* <orly/synth/variant_ctor_by_name.cc>

   Implements <orly/synth/variant_ctor_by_name.h>.

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

#include <orly/synth/variant_ctor_by_name.h>

#include <base/assert_true.h>
#include <orly/expr/obj.h>
#include <orly/expr/variant.h>
#include <orly/synth/get_pos_range.h>
#include <orly/synth/name.h>

using namespace Orly;
using namespace Orly::Synth;

/* The callee obj-member when the call has our shape, else null. */
static const Package::Syntax::TPostfixObjMember *TryGetCalleeMember(
    const Package::Syntax::TPostfixCall *postfix_call) {
  auto *member = dynamic_cast<const Package::Syntax::TPostfixObjMember *>(postfix_call->GetExpr());
  if (member && dynamic_cast<const Package::Syntax::TRefExpr *>(member->GetExpr())) {
    return member;
  }
  return nullptr;
}

bool TVariantCtorByName::Claims(const Package::Syntax::TPostfixCall *postfix_call) {
  assert(postfix_call);
  const auto *args = postfix_call->GetOptCallArgs();
  if (!dynamic_cast<const Package::Syntax::TPositionalCallArgs *>(args) &&
      !dynamic_cast<const Package::Syntax::TNoCallArgs *>(args)) {
    return false;
  }
  return TryGetCalleeMember(postfix_call) != nullptr;
}

TVariantCtorByName::TVariantCtorByName(const TExprFactory *expr_factory,
                                       const Package::Syntax::TPostfixCall *postfix_call)
    : PostfixCall(Base::AssertTrue(postfix_call)),
      Payload(nullptr) {
  assert(expr_factory);
  const auto *member = Base::AssertTrue(TryGetCalleeMember(postfix_call));
  const auto *ref_expr = Base::AssertTrue(
      dynamic_cast<const Package::Syntax::TRefExpr *>(member->GetExpr()));
  TypeDef.SetName(ref_expr->GetName());
  Tag = TName(member->GetName()).GetText();
  const auto *positional = dynamic_cast<const Package::Syntax::TPositionalCallArgs *>(
      postfix_call->GetOptCallArgs());
  if (positional) {
    Payload = expr_factory->NewExpr(positional->GetExpr());
  }
}

TVariantCtorByName::~TVariantCtorByName() {
  delete Payload;
}

Expr::TExpr::TPtr TVariantCtorByName::Build() const {
  Type::TType variant_type = TypeDef->GetSymbolicType();
  /* A tag-only arm (`tree.Nil()`) carries the empty object (the unit
     value); Expr::TVariantCtor::GetTypeImpl validates the type is a
     variant, the tag is an arm, and the payload type matches. */
  Expr::TExpr::TPtr payload = Payload
      ? Payload->Build()
      : Expr::TObj::New(Expr::TObj::TMemberMap{}, GetPosRange(PostfixCall));
  return Expr::TVariantCtor::New(Tag, payload, variant_type, GetPosRange(PostfixCall));
}

void TVariantCtorByName::ForEachInnerScope(const std::function<void (TScope *)> &cb) {
  assert(cb);
  if (Payload) {
    Payload->ForEachInnerScope(cb);
  }
}

void TVariantCtorByName::ForEachRef(const std::function<void (TAnyRef &)> &cb) {
  assert(cb);
  cb(TypeDef);
  if (Payload) {
    Payload->ForEachRef(cb);
  }
}
