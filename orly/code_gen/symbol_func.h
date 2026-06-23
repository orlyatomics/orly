/* <orly/code_gen/symbol_func.h>

   `TSymbolFunc` is the base for any code-gen function that maps
   back to a `Symbol::TFunction` from the language frontend. Holds
   the symbol pointer, the body inline, and a static
   `Symbol::TFunction*` -> `TSymbolFunc*` map so calls into the
   same named function reuse the existing code-gen.

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

#include <orly/code_gen/function.h>
#include <orly/symbol/function.h>

namespace Orly {

  namespace CodeGen {

    class TSymbolFunc : public virtual TFunction {
      NO_COPY(TSymbolFunc);
      public:

      static TFunction::TPtr Find(const Symbol::TFunction *symbol);

      std::string GetName() const;
      Type::TType GetReturnType() const;
      Type::TType GetType() const;

      /* For an imported value (#171), emit a cross-package call as the body
         instead of walking the placeholder expr; otherwise build normally. */
      virtual void Build() override;

      protected:
      TSymbolFunc(const L0::TPackage *package, const Symbol::TFunction::TPtr &symbol, const TIdScope::TPtr &id_scope);

      private:

      //TODO: Kill the static. We can really handle this using proper scoping and a tree of function maps.
      static std::unordered_map<const Symbol::TFunction*, TSymbolFunc*> Functions;

      //Note we would use an InlineScope here, but we need to attach the function arguments to the CodeScope.
      TInline::TPtr Body;

      Symbol::TFunction::TPtr Symbol;

    }; // TSymbolFunc

  } // CodeGen

} // Orly