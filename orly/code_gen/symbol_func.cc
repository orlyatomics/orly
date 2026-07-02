/* <orly/code_gen/symbol_func.cc>

   Implements <orly/code_gen/symbol_func.h>

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

#include <orly/code_gen/symbol_func.h>

#include <orly/code_gen/import_call.h>
#include <orly/symbol/import_function.h>

using namespace Orly;
using namespace Orly::CodeGen;

void TSymbolFunc::Build() {
  if (GetBody()) {
    return;
  }
  /* An imported value (#171) has only a placeholder body in the symbol layer;
     emit the real cross-package call instead of building from that placeholder. */
  if (auto import_func = dynamic_cast<const Symbol::TImportFunction *>(Symbol.get())) {
    SetBody(TImportCall::New(Package, import_func));
    return;
  }
  TFunction::Build();
}

TFunction::TPtr TSymbolFunc::Find(const L0::TPackage *package, const Symbol::TFunction *symbol) {
  assert(package);
  TSymbolFunc *raw = package->TryFindSymbolFunc(symbol);
  assert(raw); // We build all functions before we ever call find, so it should never fail.

  //NOTE: The function should never not exist
  std::shared_ptr<TFunction> ptr = raw->shared_from_this();
  assert(ptr);
  return ptr;
}

std::string TSymbolFunc::GetName() const {
  return Symbol->GetName();
}

Type::TType TSymbolFunc::GetReturnType() const {
  return Symbol->GetReturnType();
}

Type::TType TSymbolFunc::GetType() const {
  return Symbol->GetType();
}

TSymbolFunc::TSymbolFunc(const L0::TPackage *package, const Symbol::TFunction::TPtr &symbol, const TIdScope::TPtr &id_scope)
      : TFunction(package, id_scope), Symbol(symbol) {
  PostCtor(symbol->GetParams(), symbol->GetExpr(), true);
  package->RegisterSymbolFunc(symbol.get(), this);
}