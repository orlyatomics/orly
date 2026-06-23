/* <orly/code_gen/import_call.cc>

   Implements <orly/code_gen/import_call.h>.

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

#include <orly/code_gen/import_call.h>

#include <orly/code_gen/cpp_printer.h>
#include <orly/symbol/import_function.h>

using namespace Orly;
using namespace Orly::CodeGen;

TInline::TPtr TImportCall::New(const L0::TPackage *package, const Symbol::TImportFunction *import_func) {
  return TInline::TPtr(new TImportCall(package, import_func));
}

TImportCall::TImportCall(const L0::TPackage *package, const Symbol::TImportFunction *import_func)
    : TInline(package, import_func->GetReturnType()),
      PackageName(import_func->GetPackageName()),
      RemoteName(import_func->GetRemoteName()) {}

void TImportCall::WriteExpr(TCppPrinter &out) const {
  /* Leading :: for global scope; NS-prefixed package namespace; F-prefixed
     function; ctx is the implicit first arg of every top-level orly function. */
  out << "::" << PackageName << "::F" << RemoteName << "(ctx)";
}

void TImportCall::AppendDependsOn(std::unordered_set<TInline::TPtr> &) const {}
