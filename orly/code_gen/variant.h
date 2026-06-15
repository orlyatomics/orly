/* <orly/code_gen/variant.h>

   Interface for generating variant (tagged-union) headers.

   The variant code generator is the Phase-2 mirror of the record code
   generator (`orly/code_gen/obj.h`): for a `Type::TVariant` it emits a
   native C++ struct `Orly::Rt::Variants::TVariant<mangled>` that carries
   exactly one active arm, with a `AsVar()` that round-trips through the
   single-key-record encoding (`Var::TVar::Variant(tag, payload)`), plus
   `GetHash`/`EqEq`/`Neq`/`Match`/`MatchLess`, a `Type::TDt<...>::GetType()`
   specialization returning `Type::TVariant::Get(...)`, and a `std::hash`
   specialization.

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

#pragma once

#include <string>

#include <orly/code_gen/cpp_printer.h>
#include <orly/type/impl.h>

namespace Orly {

  namespace CodeGen {

    /* Emit the native C++ struct for a variant type into out_dir/<mangled>.h. */
    void GenVariantHeader(const std::string &out_dir, const Type::TType &variant_type);

    /* Emit the native C++ structs for a mutually-recursive variant GROUP
       (#116). All member classes are defined together in one header named
       by the group's canonical-first member; each other member's
       by-mangled-name header is a one-line shim that includes it. Routed to
       from GenVariantHeader when `variant_type` is a registered group
       member. Package-internal in v1 (no storage / marshaling). */
    void GenVariantGroupHeader(const std::string &out_dir, const Type::TType &member_type);

    /* Emit the #include that pulls in a generated variant header. */
    void GenVariantInclude(const Type::TType &variant_type, TCppPrinter &strm);

  } // CodeGen

} // Orly
