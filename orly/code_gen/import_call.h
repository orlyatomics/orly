/* <orly/code_gen/import_call.h>

   The code-gen body of an imported value (#171): a call into the source
   package's generated symbol, e.g. `::NSimports::NSlib::Fget_answer(ctx)`.
   TSymbolFunc installs this as an import function's body in place of the
   placeholder expr, and TPackage records the source package in NeededPackages
   so its header is included and linked.

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
#include <unordered_set>

#include <orly/code_gen/inline.h>
#include <orly/package/name.h>

namespace Orly {

  namespace Symbol {
    class TImportFunction;
  }

  namespace CodeGen {

    class TImportCall : public TInline {
      NO_COPY(TImportCall);
      public:

      static TInline::TPtr New(const L0::TPackage *package, const Symbol::TImportFunction *import_func);

      /* Emits `::NS<pkg>::F<remote>(ctx)`. */
      virtual void WriteExpr(TCppPrinter &out) const override;

      /* A leaf call -- depends on no other inlines. */
      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const override;

      private:

      TImportCall(const L0::TPackage *package, const Symbol::TImportFunction *import_func);

      Package::TName PackageName;

      std::string RemoteName;

    };  // TImportCall

  }  // CodeGen

}  // Orly
