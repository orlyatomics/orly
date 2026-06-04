/* <orly/code_gen/inner_func.h>

   `TInnerFunc` is a named lambda defined inside a top-level
   function: inherits both `TInlineFunc` (so it emits inside another
   function's body) and `TSymbolFunc` (so calls bind through the
   symbol map). Used for orlyscript's `f = (...) where { ... }`
   definitions inside function bodies.

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

#include <orly/code_gen/inline_func.h>
#include <orly/code_gen/symbol_func.h>

namespace Orly {

  namespace CodeGen {

    class TInnerFunc : public TInlineFunc, public TSymbolFunc {
      public:

      typedef std::shared_ptr<TInnerFunc> TPtr;

      static TPtr New(const L0::TPackage *package, const Symbol::TFunction::TPtr &symbol, const TIdScope::TPtr &id_scope);

      private:
      TInnerFunc(const L0::TPackage *package, const Symbol::TFunction::TPtr &symbol, const TIdScope::TPtr &id_scope);
    }; // TInnerFunc


  } // CodeGen

} // Orly