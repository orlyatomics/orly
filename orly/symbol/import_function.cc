/* <orly/symbol/import_function.cc>

   Implements <orly/symbol/import_function.h>.

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

#include <orly/symbol/import_function.h>

#include <base/assert_true.h>
#include <orly/symbol/scope.h>

using namespace Orly;
using namespace Orly::Symbol;

TFunction::TPtr TImportFunction::New(const TScopePtr &scope,
                                     const std::string &name,
                                     const TPosRange &pos_range,
                                     const Type::TType &declared_return_type,
                                     const Package::TName &package_name,
                                     const std::string &remote_name) {
  assert(scope);
  auto function = TPtr(new TImportFunction(scope, name, pos_range, declared_return_type, package_name, remote_name));
  scope->Add(function);
  return function;
}

TImportFunction::TImportFunction(const TScopePtr &scope,
                                 const std::string &name,
                                 const TPosRange &pos_range,
                                 const Type::TType &declared_return_type,
                                 const Package::TName &package_name,
                                 const std::string &remote_name)
    : TFunction(scope, name, pos_range),
      DeclaredReturnType(declared_return_type),
      PackageName(package_name),
      RemoteName(remote_name) {}

TImportFunction::~TImportFunction() {}

Type::TType TImportFunction::GetReturnType() const {
  return DeclaredReturnType;
}

void TImportFunction::VerifyRecursiveReturns() const {}
