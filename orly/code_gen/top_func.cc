/* <orly/code_gen/top_func.cc>

   Implements <orly/code_gen/top_func.h>

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

#include <orly/code_gen/top_func.h>

#include <orly/code_gen/scope.h>

using namespace Orly;
using namespace Orly::CodeGen;

bool TTopFunc::IsTopLevel() const {
  return true;
}

void TTopFunc::WriteDecl(TCppPrinter &out) const {

  //NOTE: We prefix with 'F' to make the function name never conflict with a C++ builtin.
  out << GetReturnType() << ' ';
  WriteCcName(out);
  out << "(Orly::Package::TContext &ctx";
  if (HasArgs()) {
    out << ", ";
  }
  WriteArgs(out);
  out << ')';
}

void TTopFunc::WriteDef(TCppPrinter &out) const {

  WriteDecl(out);
  WriteBody(out);
}

TTopFunc::TTopFunc(const L0::TPackage *package) : TFunction(package, TIdScope::New()) {}