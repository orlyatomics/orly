/* <orly/synth/type_def.cc>

   Implements <orly/synth/type_def.h>.

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

#include <orly/synth/type_def.h>

#include <vector>

#include <base/as_str.h>
#include <base/assert_true.h>
#include <base/no_default_case.h>
#include <orly/synth/context.h>
#include <orly/synth/new_type.h>
#include <orly/type/unroll.h>

using namespace Orly;
using namespace Orly::Synth;

/* The in-flight stack (issue #103): one entry per type def whose symbolic
   type is currently being computed, innermost last, each tracking how many
   variant binders the computation has entered so far. The compiler is
   single-threaded; thread_local for safety. */
struct TInFlightEntry {
  const TTypeDef *Def;
  size_t VariantDepth;
};  // TInFlightEntry
static thread_local std::vector<TInFlightEntry> InFlightStack;

TTypeDef::TTypeDef(TScope *scope, const Package::Syntax::TTypeDef *type_def)
    : TDef(scope, Base::AssertTrue(type_def)->GetName()),
      Type(NewType(type_def->GetType())) {}

TTypeDef::~TTypeDef() {
  delete Type;
}

const TTypeDef *TTypeDef::GetInnermostInFlight() {
  return InFlightStack.empty() ? nullptr : InFlightStack.back().Def;
}

bool TTypeDef::IsInFlight(const TTypeDef *def) {
  for (const auto &entry : InFlightStack) {
    if (entry.Def == def) {
      return true;
    }
  }
  return false;
}

size_t TTypeDef::GetCurVariantDepth() {
  return InFlightStack.empty() ? 0 : InFlightStack.back().VariantDepth;
}

TTypeDef::TVariantDepthIncr::TVariantDepthIncr() {
  if (!InFlightStack.empty()) {
    ++InFlightStack.back().VariantDepth;
  }
}

TTypeDef::TVariantDepthIncr::~TVariantDepthIncr() {
  if (!InFlightStack.empty()) {
    --InFlightStack.back().VariantDepth;
  }
}

void TTypeDef::NoteSelfRefMinted() {
  assert(!InFlightStack.empty());
  InFlightStack.back().Def->SelfRefMinted = true;
}

const Type::TType &TTypeDef::GetSymbolicType() const {
  InFlightStack.push_back(TInFlightEntry{this, 0});
  try {
    const Type::TType &result = Type->GetSymbolicType();
    InFlightStack.pop_back();
    return result;
  } catch (...) {
    InFlightStack.pop_back();
    throw;
  }
}

/* True iff every free self-reference in `t` sits where the native
   representation can box it (issues #103/#116): at the position itself,
   or below any chain of list / set / opt / dict-VALUE containers. Dict
   keys stay self-free. `opt` of self (`<{.next: t?}>`, a nullable child)
   works now that optional-of-variant construction and comparison are
   supported (#118). A free reference still may not thread through a
   nested variant or any other compound (the #116 remainder). */
static bool SelfRefPlacementOk(const Type::TType &t) {
  if (!Type::HasFreeSelfRef(t)) {
    return true;
  }
  if (t.Is<Type::TSelfRef>()) {
    return true;
  }
  if (const auto *list = t.TryAs<Type::TList>()) {
    return SelfRefPlacementOk(list->GetElem());
  }
  if (const auto *set = t.TryAs<Type::TSet>()) {
    return SelfRefPlacementOk(set->GetElem());
  }
  if (const auto *opt = t.TryAs<Type::TOpt>()) {
    return SelfRefPlacementOk(opt->GetElem());
  }
  if (const auto *dict = t.TryAs<Type::TDict>()) {
    return !Type::HasFreeSelfRef(dict->GetKey()) && SelfRefPlacementOk(dict->GetVal());
  }
  return false;
}

/* The placement rules for self-references (issues #103/#116), enforced
   only on a def that actually minted one (a def that merely *uses* some
   other recursive type is exempt): the def's type must be a variant at
   its top level -- the variant is the binder, so anything else means the
   reference doesn't denote the def -- and each FREE self-reference must
   sit at an arm payload's root, as a field of an arm's payload record,
   or under list/set/opt/dict-value containers within those positions.
   Free references through a NESTED variant, in a dict key, or under any
   other compound have no native representation yet. Reports errors
   against the def's name. */
static void ValidateSelfRefPlacement(const TTypeDef *def, const Type::TType &type) {
  const TPosRange &pos_range = def->GetName().GetPosRange();
  const Type::TVariant *variant = type.TryAs<Type::TVariant>();
  if (!variant) {
    GetContext().AddError(pos_range,
        "a recursive type definition must be a variant at its top level (issue #103)");
    return;
  }
  for (const auto &arm : variant->GetElems()) {
    const Type::TType &payload = arm.second;
    /* An open nested variant directly as an arm payload (#116 Phase 1).
       v1 supports a single nesting level: the nested variant's own arms
       must be tag-only / scalar / self-free, or a *direct* self-reference
       to an enclosing variant -- not a record, container, or further
       nested variant carrying a free self-reference. */
    if (const Type::TVariant *nested = payload.TryAs<Type::TVariant>()) {
      if (Type::HasFreeSelfRef(payload)) {
        bool ok = true;
        for (const auto &nv_arm : nested->GetElems()) {
          if (Type::HasFreeSelfRef(nv_arm.second) && !nv_arm.second.Is<Type::TSelfRef>()) {
            ok = false;
            break;
          }
        }
        if (!ok) {
          GetContext().AddError(pos_range,
              Base::AsStr("in arm \"", arm.first,
                          "\": a nested variant may currently recurse only via a direct "
                          "self-reference arm; records, containers, and deeper nested "
                          "variants holding a self-reference are not yet supported (#116)"));
        }
        continue;
      }
    }
    bool ok;
    /* One record layer is allowed at the payload root only. */
    const Type::TObj *rec = payload.TryAs<Type::TObj>();
    if (rec) {
      ok = true;
      for (const auto &field : rec->GetElems()) {
        if (!SelfRefPlacementOk(field.second)) {
          ok = false;
          break;
        }
      }
    } else {
      ok = SelfRefPlacementOk(payload);
    }
    if (!ok) {
      GetContext().AddError(pos_range,
          Base::AsStr("in arm \"", arm.first,
                      "\": a self-reference may only appear as an arm's payload, as a field "
                      "of the arm's payload record, or under list/set/opt/dict-value "
                      "containers within those positions (issues #103/#116)"));
    }
  }
}

TAction TTypeDef::Build(int pass) {
  switch (pass) {
    case 1: {
      /* Through the member (not Type-> directly) so the in-flight push
         wraps the computation and self-references can resolve. */
      const Type::TType &type = GetSymbolicType();
      if (SelfRefMinted) {
        ValidateSelfRefPlacement(this, type);
      }
      break;
    }
    NO_DEFAULT_CASE;
  }
  return Finish;
}

void TTypeDef::ForEachPred(int pass, const std::function<bool (TDef *)> &cb) {
  switch (pass) {
    case 1: {
      Type->ForEachRef([this, cb](TAnyRef &ref) -> bool {
        /* A def may depend on itself (direct recursion, #103); skip the
           self-edge so the build driver's cycle detector doesn't fire. */
        TDef *def = ref.GetDef();
        if (def != this) {
          cb(def);
        }
        return true;
      });
      break;
    }
    NO_DEFAULT_CASE;
  }
}

void TTypeDef::ForEachRef(const std::function<void (TAnyRef &)> &cb) {
  Type->ForEachRef(cb);
}

const char *TDef::TInfo<TTypeDef>::Name = "a type defintion";
