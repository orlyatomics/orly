/* <orly/code_gen/union_map.cc>

   Implements <orly/code_gen/union_map.h>

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

#include <orly/code_gen/union_map.h>

#include <orly/code_gen/implicit_func.h>

using namespace Orly::CodeGen;


TUnionMap::TPtr TUnionMap::New(const L0::TPackage *package,
                               const Type::TType &ret_type,
                               const TInline::TPtr &seq,
                               const TImplicitFunc::TPtr &map_func) {
  return TPtr(new TUnionMap(package, ret_type, seq, map_func));
}

void TUnionMap::WriteExpr(TCppPrinter &out) const {
  /* The trailing `RetType()` is the empty-set start: it gives the runtime
     UnionMap its result type by deduction (the map func alone can't), and
     IS the union identity. */
  out << "UnionMap(" << Seq << ", ";
  Func->WriteName(out);
  out << ", " << GetReturnType() << "())";
}

TUnionMap::TUnionMap(const L0::TPackage *package,
                     const Type::TType &ret_type,
                     const TInline::TPtr &seq,
                     const TImplicitFunc::TPtr &map_func)
  : TInline(package, ret_type),
    Func(map_func),
    Seq(seq) {}

void TUnionMap::AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const {
  AppendDependency(Seq, dependency_set);
  AppendDependency(Func->GetBody(), dependency_set);
}
