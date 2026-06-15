/* <orly/symbol/function.cc>

   Implements <orly/symbol/function.h>

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

#include <orly/symbol/function.h>

#include <base/assert_true.h>
#include <orly/error.h>
#include <orly/symbol/scope.h>
#include <orly/type/any.h>

using namespace Orly;
using namespace Orly::Symbol;

TFunction::TParamDef::TParamDef(const std::string &name, const TPosRange &pos_range)
    : TDef(name, pos_range) {}

TFunction::TPtr TFunction::New(const TScope::TPtr &scope, const std::string &name, const TPosRange &pos_range) {
  assert(scope);
  auto function = TFunction::TPtr(new TFunction(scope, name, pos_range));
  scope->Add(function);
  return function;
}

TFunction::TFunction(const TScope::TPtr &scope, const std::string &name, const TPosRange &pos_range)
    : TAnyFunction(name), IsRecursive(false), Scope(Base::AssertTrue(scope)), PosRange(pos_range) {}

TFunction::~TFunction() {
  auto scope = TryGetScope();
  if (scope) {
    scope->Remove(shared_from_this());
  }
}

void TFunction::Accept(const TVisitor &visitor) const {
  visitor(this);
}

void TFunction::Add(const TParamDef::TPtr &param_def) {
  assert(param_def);
  auto result = ParamDefs.insert(param_def);
  assert(result.second);
}

const TFunction::TParamDefSet &TFunction::GetParamDefs() const {
  return ParamDefs;
}

Type::TObj::TElems TFunction::GetParams() const {
  Type::TObj::TElems elems;
  for (auto param_def : ParamDefs) {
    auto result = elems.insert(std::make_pair(param_def->GetName(), param_def->GetType()));
    /* If this assertion fails, we have multiple parameters with the same name.
       Fix the synth layer to catch this error */
    assert(result.second);
  }
  return elems;
}

const TPosRange &TFunction::GetPosRange() const {
  return PosRange;
}

Type::TType TFunction::GetReturnType() const {
  Type::TType type;
  if (IsRecursive) {
    /* Re-entered through a recursive call: the return type is not yet known.
       TAny is the placeholder; a `when`/if-else with a concrete arm anchors
       it, and an all-recursive node now defers rather than throwing (#126). */
    type = Type::TAny::Get();
  } else {
    IsRecursive = true;
    type = GetExpr()->GetType();
    IsRecursive = false;
    /* If the whole body still resolves to TAny, every return path is a
       recursive call -- a genuine missing base case. No expression produces
       TAny except the recursive placeholder, so this is the one place the
       diagnostic belongs (moved here from the per-node join, #126). */
    if (type.Is<Type::TAny>()) {
      throw TExprError(HERE, PosRange,
          "this recursive function has no base case: every path is a recursive call");
    }
  }
  return type;
}

TScope::TPtr TFunction::GetScope() const {
  return Base::AssertTrue(TryGetScope());
}

void TFunction::Remove(const TParamDef::TPtr &param_def) {
  assert(param_def);
  ParamDefs.erase(param_def);
}

TScope::TPtr TFunction::TryGetScope() const {
  return Scope.lock();
}
