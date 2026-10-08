/* <orly/code_gen/keys.cc>

   Implements <orly/code_gen/keys.h>

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

#include <orly/code_gen/keys.h>

#include <base/split.h>
#include <orly/type/int.h>
#include <orly/type/seq.h>
#include <orly/type/unwrap.h>

using namespace Base;
using namespace Orly;
using namespace Orly::CodeGen;

TKeys::TKeys(const L0::TPackage *package,
             const Type::TType &seq_type,
             const Type::TType &val_type,
             TAddrElems &&addr_elems,
             const TInline::TPtr &bound,
             bool bound_is_inclusive,
             bool count_only)
    : TInline(package, count_only ? Type::TInt::Get() : seq_type),
      AddrElems(std::move(addr_elems)),
      ValType(val_type),
      AddrType(Type::UnwrapSequence(seq_type)),
      Bound(bound),
      BoundIsInclusive(bound_is_inclusive),
      CountOnly(count_only) {}

void TKeys::WriteExpr(TCppPrinter &out) const {

  if (CountOnly) {
    out << "ctx.CountKeys(ctx.GetFlux(), ";
  } else {
    out << "ctx.New<" << AddrType << ">(ctx.GetFlux(), ";
  }
  /* this is where we put the index id */ {
    const Base::TUuid &index_id = Package->GetIndexIdFor(AddrType, ValType);
    char uuid[37];
    index_id.FormatUnderscore(uuid);
    out << Package->GetName() << "::My" << uuid << " ,";
  }
  out
    << "std::tuple<"
    << Join(AddrElems,
            ", ",
            [](TCppPrinter &out, TAddrElems::const_reference it) {
              if (!it.second->IsFree()) {
                switch (it.first) {
                  case Orly::TAddrDir::Asc: {
                    out << it.second->GetReturnType();
                    break;
                  }
                  case Orly::TAddrDir::Desc: {
                    out << "Orly::TDesc<" << it.second->GetReturnType() << ">";
                    break;
                  }
                }
              } else {
                switch (it.first) {
                  case Orly::TAddrDir::Asc: {
                    out << "Native::TFree<" << it.second->GetReturnType() << ">";
                    break;
                  }
                  case Orly::TAddrDir::Desc: {
                    out << "Native::TFree<Orly::TDesc<" << it.second->GetReturnType() << ">>";
                    break;
                  }
                }
              }
            })
    << ">("
    << Join(AddrElems,
            ", ",
            [](TCppPrinter &out, TAddrElems::const_reference it) {
              //NOTE: This sometimes will cause
              if (!it.second->IsFree()) {
                switch (it.first) {
                  case Orly::TAddrDir::Asc: {
                    out << it.second->GetReturnType();
                    break;
                  }
                  case Orly::TAddrDir::Desc: {
                    out << "Orly::TDesc<" << it.second->GetReturnType() << ">";
                    break;
                  }
                }
              } else {
                switch (it.first) {
                  case Orly::TAddrDir::Asc: {
                    out << "Native::TFree<" << it.second->GetReturnType() << ">";
                    break;
                  }
                  case Orly::TAddrDir::Desc: {
                    out << "Native::TFree<Orly::TDesc<" << it.second->GetReturnType() << ">>";
                    break;
                  }
                }
              }
              out << "(" << it.second << ")";
            })
    << ")";
  /* Keyset paging (#735): the bound key, then whether it is inclusive. An
     unbounded `keys` emits exactly what it always did. */
  if (Bound && !CountOnly) {
    out << ", " << Bound << ", " << (BoundIsInclusive ? "true" : "false");
  }
  out << ")";
}
