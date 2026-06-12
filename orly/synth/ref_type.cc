/* <orly/synth/ref_type.cc>

   Implements <orly/synth/ref_type.h>.

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

#include <orly/synth/ref_type.h>

#include <base/assert_true.h>
#include <orly/synth/context.h>
#include <orly/type/any.h>
#include <orly/type/self_ref.h>

using namespace Orly;
using namespace Orly::Synth;

TRefType::TRefType(const Package::Syntax::TRefType *ref_type)
    : TypeDef(Base::AssertTrue(ref_type)->GetName()) {}

void TRefType::ForEachRef(const std::function<void (TAnyRef &)> &cb) {
  cb(TypeDef);
}

Type::TType TRefType::ComputeSymbolicType() const {
  /* A reference to the type def currently computing its symbolic type is
     a recursive type (issue #103): mint a de Bruijn self-reference bound
     by the nearest enclosing variant instead of recursing forever. Only
     direct recursion through a variant arm is supported in v1. */
  TTypeDef *def = *TypeDef;
  if (def && TTypeDef::IsInFlight(def)) {
    const TPosRange &pos_range = TypeDef.GetName().GetPosRange();
    if (def != TTypeDef::GetInnermostInFlight()) {
      GetContext().AddError(pos_range,
          "mutually or transitively recursive type definitions are not supported (issue #103)");
      return Type::TAny::Get();
    }
    size_t variant_depth = TTypeDef::GetCurVariantDepth();
    if (!variant_depth) {
      GetContext().AddError(pos_range,
          "a recursive type reference must appear inside a variant arm (issue #103)");
      return Type::TAny::Get();
    }
    TTypeDef::NoteSelfRefMinted();
    return Type::TSelfRef::Get(variant_depth - 1);
  }
  return TypeDef->GetSymbolicType();
}
