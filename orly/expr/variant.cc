/* <orly/expr/variant.cc>

   Implements <orly/expr/variant.h>

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

#include <orly/expr/variant.h>

#include <base/as_str.h>
#include <orly/error.h>
#include <orly/type/any.h>
#include <orly/type/dict.h>
#include <orly/type/list.h>
#include <orly/type/obj.h>
#include <orly/type/opt.h>
#include <orly/type/seq.h>
#include <orly/type/set.h>
#include <orly/type/unroll.h>
#include <orly/type/unwrap.h>
#include <orly/type/util.h>
#include <orly/type/variant.h>

using namespace Orly;
using namespace Orly::Expr;

namespace {

  /* True iff `actual` matches `expected` structurally, treating any `TAny`
     in `actual` as a wildcard matching the corresponding subterm of
     `expected`. `TAny` arises only from an as-yet-unresolved recursive-call
     result (TFunction::GetReturnType), so this defers verification of a
     recursive result nested ANYWHERE in a compound payload while still
     strictly checking the non-recursive parts -- the compound generalization
     of the shallow `Is<TAny>` deferral (#128 Option A). It is what lets a
     recursive variant be rebuilt by a fold, e.g. widening
     `Node(<{.next: rnar}>)` to `Node(<{.next: rwid}>)` where the `.next`
     field holds the recursive call's (TAny) result (#104).

     Recurses through the recursive-variant payload shapes (#116/#125):
     record, list/set/seq, dict value/key, opt, and nested variant. Any other
     kind falls to the interned-equality fast path -- a conservative reject if
     it isn't pointer-equal, never a false accept. */
  bool MatchesModuloAny(const Type::TType &actual, const Type::TType &expected) {
    if (actual == expected) {
      return true;  // interned: structurally identical (covers the no-TAny case)
    }
    if (actual.Is<Type::TAny>()) {
      return true;  // a recursive result -- the wildcard hole we defer on
    }
    /* Same-kind compound: recurse on children. A map-keyed compound (obj /
       variant) must have the same key set with each field matching. */
    auto match_map = [](const auto &a_elems, const auto &e_elems) {
      if (a_elems.size() != e_elems.size()) {
        return false;
      }
      auto ai = a_elems.begin();
      auto ei = e_elems.begin();
      for (; ai != a_elems.end(); ++ai, ++ei) {
        if (ai->first != ei->first || !MatchesModuloAny(ai->second, ei->second)) {
          return false;
        }
      }
      return true;
    };
    if (const auto *a = actual.TryAs<Type::TObj>()) {
      const auto *e = expected.TryAs<Type::TObj>();
      return e && match_map(a->GetElems(), e->GetElems());
    }
    if (const auto *a = actual.TryAs<Type::TVariant>()) {
      const auto *e = expected.TryAs<Type::TVariant>();
      return e && match_map(a->GetElems(), e->GetElems());
    }
    if (const auto *a = actual.TryAs<Type::TList>()) {
      const auto *e = expected.TryAs<Type::TList>();
      return e && MatchesModuloAny(a->GetElem(), e->GetElem());
    }
    if (const auto *a = actual.TryAs<Type::TSet>()) {
      const auto *e = expected.TryAs<Type::TSet>();
      return e && MatchesModuloAny(a->GetElem(), e->GetElem());
    }
    if (const auto *a = actual.TryAs<Type::TSeq>()) {
      const auto *e = expected.TryAs<Type::TSeq>();
      return e && MatchesModuloAny(a->GetElem(), e->GetElem());
    }
    if (const auto *a = actual.TryAs<Type::TOpt>()) {
      const auto *e = expected.TryAs<Type::TOpt>();
      return e && MatchesModuloAny(a->GetElem(), e->GetElem());
    }
    if (const auto *a = actual.TryAs<Type::TDict>()) {
      const auto *e = expected.TryAs<Type::TDict>();
      return e && MatchesModuloAny(a->GetKey(), e->GetKey())
               && MatchesModuloAny(a->GetVal(), e->GetVal());
    }
    return false;
  }

}  // namespace

TVariantCtor::TPtr TVariantCtor::New(const std::string &tag,
                                     const TExpr::TPtr &payload,
                                     const Type::TType &variant_type,
                                     const TPosRange &pos_range) {
  return TVariantCtor::TPtr(new TVariantCtor(tag, payload, variant_type, pos_range));
}

TVariantCtor::TVariantCtor(const std::string &tag,
                           const TExpr::TPtr &payload,
                           const Type::TType &variant_type,
                           const TPosRange &pos_range)
    : TUnary(payload, pos_range), Tag(tag), VariantType(variant_type) {}

void TVariantCtor::Accept(const TVisitor &visitor) const {
  visitor(this);
}

Type::TType TVariantCtor::GetTypeImpl() const {
  const Type::TVariant *variant = VariantType.TryAs<Type::TVariant>();
  if (!variant) {
    throw TExprError(HERE, GetPosRange(), "variant constructor's annotation is not a variant type");
  }
  const Type::TVariantElems &elems = variant->GetElems();
  auto iter = elems.find(Tag);
  if (iter == elems.end()) {
    std::string msg = Base::AsStr("variant constructor tag \"", Tag, "\" is not an arm of the declared variant type");
    throw TExprError(HERE, GetPosRange(), msg.c_str());
  }
  /* The payload expression's type must match the declared arm payload
     type, unrolled: for a recursive variant the declared payload contains
     self-references, while the payload expression's fields are typed by
     the variant itself (#103). (A tag-only arm's declared payload is the
     empty object and the synth layer supplies an empty-object payload
     expression, so this check covers both cases uniformly.) */
  Type::TType payload_type = Type::Unwrap(GetExpr()->GetType());
  /* The payload must match the declared (unrolled) arm type, but a TAny --
     an as-yet-unresolved recursive call (the placeholder from
     TFunction::GetReturnType) -- is deferred rather than rejected, so a
     function can construct with its own recursive result (e.g. a tree-to-tree
     transform or a recursive widening fold). MatchesModuloAny treats TAny as
     a wildcard ANYWHERE in the payload, so a recursive result nested in a
     record/container (the common `Node(<{.next: self}>)` shape) is deferred
     while the non-recursive parts stay strictly checked. This matches how
     operators already propagate TAny -- a recursive result is not verified in
     this position (#128 Option A, extended to compound payloads for #104
     recursive-variant widening). The ctor's own type is the variant
     regardless of the payload. */
  if (!MatchesModuloAny(payload_type, Type::Unroll(iter->second, VariantType))) {
    std::string msg = Base::AsStr("variant constructor payload type does not match the declared type of arm \"", Tag, "\"");
    throw TExprError(HERE, GetPosRange(), msg.c_str());
  }
  return VariantType;
}
