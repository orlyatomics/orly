/* <orly/synth/variant_ctor.cc>

   Implements <orly/synth/variant_ctor.h>.

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

#include <orly/synth/variant_ctor.h>

#include <base/assert_true.h>
#include <orly/expr/obj.h>
#include <orly/expr/variant.h>
#include <orly/synth/get_pos_range.h>
#include <orly/synth/name.h>
#include <orly/synth/variant_type.h>

using namespace Orly;
using namespace Orly::Synth;

TVariantCtor::TVariantCtor(const TExprFactory *expr_factory, const Package::Syntax::TVariantCtor *variant_ctor)
    : VariantCtor(Base::AssertTrue(variant_ctor)),
      VariantTypeNode(nullptr),
      Payload(nullptr) {
  assert(expr_factory);
  try {
    VariantTypeNode = new TVariantType(variant_ctor->GetVariantType());
    Tag = TName(variant_ctor->GetName()).GetText();
  } catch (...) {
    delete VariantTypeNode;
    throw;
  }
}

TVariantCtor::TVariantCtor(const TExprFactory *expr_factory,
                           const Package::Syntax::TVariantCtor *variant_ctor,
                           const Package::Syntax::TExpr *payload)
    : VariantCtor(Base::AssertTrue(variant_ctor)),
      VariantTypeNode(nullptr),
      Payload(nullptr) {
  assert(expr_factory);
  assert(payload);
  try {
    VariantTypeNode = new TVariantType(variant_ctor->GetVariantType());
    Tag = TName(variant_ctor->GetName()).GetText();
    Payload = expr_factory->NewExpr(payload);
  } catch (...) {
    delete Payload;
    delete VariantTypeNode;
    throw;
  }
}

TVariantCtor::~TVariantCtor() {
  delete Payload;
  delete VariantTypeNode;
}

Expr::TExpr::TPtr TVariantCtor::Build() const {
  Type::TType variant_type = VariantTypeNode->GetSymbolicType();
  /* A tag-only arm carries the empty object (the unit value); the
     type-checker (Expr::TVariantCtor::GetTypeImpl) verifies the payload
     type against the declared arm type for both cases uniformly. */
  Expr::TExpr::TPtr payload = Payload
      ? Payload->Build()
      : Expr::TObj::New(Expr::TObj::TMemberMap{}, GetPosRange(VariantCtor));
  return Expr::TVariantCtor::New(Tag, payload, variant_type, GetPosRange(VariantCtor));
}

void TVariantCtor::ForEachInnerScope(const std::function<void (TScope *)> &cb) {
  assert(cb);
  if (Payload) {
    Payload->ForEachInnerScope(cb);
  }
}

void TVariantCtor::ForEachRef(const std::function<void (TAnyRef &)> &cb) {
  assert(cb);
  VariantTypeNode->ForEachRef(cb);
  if (Payload) {
    Payload->ForEachRef(cb);
  }
}
