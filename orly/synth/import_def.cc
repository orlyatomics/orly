/* <orly/synth/import_def.cc>

   Implements <orly/synth/import_def.h>.

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

#include <orly/synth/import_def.h>

#include <base/assert_true.h>
#include <base/no_default_case.h>
#include <orly/expr/unknown.h>
#include <orly/symbol/import_function.h>
#include <orly/symbol/result_def.h>
#include <orly/synth/new_type.h>

using namespace Orly;
using namespace Orly::Synth;

static TPosRange ImportPosRange(const Package::Syntax::TImportDef *import_def) {
  return TPosRange(
      import_def->GetName()->GetLexeme().GetPosRange(),
      import_def->GetSemi()->GetLexeme().GetPosRange());
}

TImportDef::TImportDef(TScope *scope, const Package::Syntax::TImportDef *import_def)
    : TFuncDef(scope, TName(Base::AssertTrue(import_def)->GetName()), ImportPosRange(import_def)),
      DeclaredType(NewType(import_def->GetType())),
      /* Stage 1 binds against the declared type; the source symbol name (opt_name)
         and package ref are resolved in the codegen/driver stages. Default the
         remote name to the local name until then. */
      RemoteName(TName(import_def->GetName()).GetText()) {}

TImportDef::~TImportDef() {}

TAction TImportDef::Build(int pass) {
  TAction action;
  switch (pass) {
    case 1: {
      action = Continue;
      break;
    }
    case 2: {
      auto symbol = Symbol::TImportFunction::New(
                      GetOuterScope()->GetScopeSymbol(),   /* scope */
                      GetName().GetText(),                 /* name */
                      PosRange,                            /* pos_range */
                      DeclaredType->GetSymbolicType(),     /* declared result type */
                      RemoteName);                         /* source symbol name */
      Symbol::TResultDef::New(symbol, GetName().GetText(), PosRange);
      SetSymbol(symbol);
      action = Continue;
      break;
    }
    case 3: {
      action = Continue;
      break;
    }
    case 4: {
      /* Placeholder body so the many body-walking passes (typecheck, #104
         widening, codegen) have a real leaf to traverse: a typed-unknown of the
         declared type. The function's *result* type still comes from
         TImportFunction::GetReturnType() (the declared type, non-optional), not
         from this body -- so `answer` types as the declared type. The codegen
         stage (#171) replaces this with the real cross-package call to RemoteName. */
      GetSymbol()->SetExpr(Expr::TUnknown::New(DeclaredType->GetSymbolicType(), PosRange));
      action = Finish;
      break;
    }
    NO_DEFAULT_CASE;
  }
  return action;
}

void TImportDef::ForEachInnerScope(const std::function<void (TScope *)> &) {}

void TImportDef::ForEachRef(const std::function<void (TAnyRef &)> &) {}

const char *TDef::TInfo<TImportDef>::Name = "an import definition";
