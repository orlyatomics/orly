/* <orly/code_gen/variant_access.cc>

   Implements <orly/code_gen/variant_access.h>.

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

#include <orly/code_gen/variant_access.h>

#include <orly/type/unroll.h>
#include <orly/type/unwrap.h>
#include <orly/type/variant.h>

using namespace Orly;
using namespace Orly::CodeGen;

TVariantIs::TVariantIs(const L0::TPackage *package,
                       const Type::TType &type,
                       const TInline::TPtr &operand,
                       size_t which)
    : TInline(package, type), Operand(operand), Which(which) {}

void TVariantIs::WriteExpr(TCppPrinter &out) const {
  /* GetWhich() returns the active arm's asciibetical index; the predicate
     is true iff it equals this tag's index. */
  out << "((" << Operand << ").GetWhich() == " << Which << ')';
}

TVariantMember::TVariantMember(const L0::TPackage *package,
                               const Type::TType &type,
                               const TInline::TPtr &operand,
                               const std::string &tag)
    : TInline(package, type), Operand(operand), Tag(tag) {}

void TVariantMember::WriteExpr(TCppPrinter &out) const {
  /* GetV<Tag>() returns the active arm's payload and asserts the arm is
     active -- callers gate this with `is <Tag>`. For a RECURSIVE arm the
     generated getter is a member template over the unrolled payload type
     (which is this inline's own result type), supplied explicitly here.
     "Recursive" covers self-references (#103) and references into a
     mutually-recursive group (#116). */
  const Type::TVariant *variant =
      Type::Unwrap(Operand->GetReturnType()).TryAs<Type::TVariant>();
  bool recursive_arm = false;
  if (variant) {
    auto iter = variant->GetElems().find(Tag);
    recursive_arm = iter != variant->GetElems().end()
        && (Type::HasFreeSelfRef(iter->second) || Type::HasGroupRef(iter->second));
  }
  out << '(' << Operand << ").GetV" << Tag;
  if (recursive_arm) {
    out << '<' << GetReturnType() << '>';
  }
  out << "()";
}

TVariantWhen::TVariantWhen(const L0::TPackage *package,
                           const Type::TType &type,
                           const TInline::TPtr &operand,
                           const TArmVec &arms)
    : TInline(package, type), Operand(operand), Arms(arms) {}

void TVariantWhen::WriteExpr(TCppPrinter &out) const {
  /* Nested ternary on the operand's active arm. All arms but the last are
     guarded; the last is the exhaustive fall-through. Each arm is a
     TInlineScope (see the builder's TWhen visitor, #297) -- a lambda invoked
     only when its guard selects it, so arm-local CSE definitions (payload
     accessors above all) stay behind the dispatch. */
  out << '(';
  for (size_t arm_idx = 0; arm_idx + 1 < Arms.size(); ++arm_idx) {
    out << "((" << Operand << ").GetWhich() == " << Arms[arm_idx].first
        << ") ? (" << Arms[arm_idx].second << ")() : ";
  }
  out << '(' << Arms.back().second << ")())";
}

TOptWhen::TOptWhen(const L0::TPackage *package,
                   const Type::TType &type,
                   const TInline::TPtr &operand,
                   const TInline::TPtr &known_body,
                   const TInline::TPtr &unknown_body)
    : TInline(package, type), Operand(operand), KnownBody(known_body), UnknownBody(unknown_body) {}

void TOptWhen::WriteExpr(TCppPrinter &out) const {
  /* Presence selects the arm; the `Known(v)` binder reads `(op).GetVal()`
     inside the known body (an `optional.Known` accessor). Both bodies are
     TInlineScope lambdas invoked at their guard site (#297), keeping any
     CSE'd payload access behind the presence check. */
  out << "((" << Operand << ").IsKnown() ? (" << KnownBody << ")() : (" << UnknownBody << ")())";
}

TVariantWiden::TVariantWiden(const L0::TPackage *package,
                             const Type::TType &type,
                             const TInline::TPtr &operand,
                             const TArmVec &arms)
    : TInline(package, type), Operand(operand), Arms(arms) {}

void TVariantWiden::WriteExpr(TCppPrinter &out) const {
  /* Nested ternary on the source's active arm, each branch rebuilding the
     value via the destination (wide) struct's `Mk<Tag>` factory.
     GetReturnType() prints the destination's mangled struct name (as in
     TVariantCtor); `Mk` prefixes the tag to dodge macro collisions (#119).
     The last arm is the unconditional fall-through (the source's arm set is
     total, so one branch always applies). */
  out << '(';
  for (size_t arm_idx = 0; arm_idx + 1 < Arms.size(); ++arm_idx) {
    out << "((" << Operand << ").GetWhich() == " << Arms[arm_idx].first << ") ? "
        << GetReturnType() << "::Mk" << Arms[arm_idx].second
        << "((" << Operand << ").GetV" << Arms[arm_idx].second << "()) : ";
  }
  out << GetReturnType() << "::Mk" << Arms.back().second
      << "((" << Operand << ").GetV" << Arms.back().second << "()))";
}
