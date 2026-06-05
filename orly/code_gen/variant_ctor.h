/* <orly/code_gen/variant_ctor.h>

   Code-gen inline node for a variant / tagged-union constructor
   (#95 Phase 3). Emits the native value-construction expression
   `Orly::Rt::Variants::TVariant<mangled>::<Tag>(<payload>)` -- the
   per-arm static factory emitted by GenVariantHeader (Phase 2). A
   near-mirror of `TObjCtor` (orly/code_gen/obj.h).

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
#include <orly/code_gen/inline.h>
#include <orly/type/impl.h>

namespace Orly {

  namespace CodeGen {

    class TVariantCtor : public TInline {
      NO_COPY(TVariantCtor);
      public:

      TVariantCtor(const L0::TPackage *package,
                   const Type::TType &type,
                   const std::string &tag,
                   const TInline::TPtr &payload);

      void WriteExpr(TCppPrinter &out) const;

      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const override {
        dependency_set.insert(Payload);
        Payload->AppendDependsOn(dependency_set);
      }

      private:
        std::string Tag;
        TInline::TPtr Payload;
    }; // TVariantCtor

  } // CodeGen

} // Orly
