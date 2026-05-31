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

#include <base/assert_true.h>
#include <base/no_default_case.h>
#include <orly/synth/new_type.h>

using namespace Orly;
using namespace Orly::Synth;

TTypeDef::TTypeDef(TScope *scope, const Package::Syntax::TTypeDef *type_def)
    : TDef(scope, Base::AssertTrue(type_def)->GetName()),
      Type(NewType(type_def->GetType())) {}

TTypeDef::~TTypeDef() {
  delete Type;
}

TAction TTypeDef::Build(int pass) {
  switch (pass) {
    case 1: {
      Type->GetSymbolicType();
      break;
    }
    NO_DEFAULT_CASE;
  }
  return Finish;
}

void TTypeDef::ForEachPred(int pass, const std::function<bool (TDef *)> &cb) {
  switch (pass) {
    case 1: {
      Type->ForEachRef([cb](TAnyRef &ref) -> bool {
        cb(ref.GetDef());
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

const Type::TType &TTypeDef::GetSymbolicType() const {
  return Type->GetSymbolicType();
}

const char *TDef::TInfo<TTypeDef>::Name = "a type defintion";
