/* <tools/nycr/language.h>

   A declaration of a language of token.

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

#include <optional>
#include <string>
#include <vector>

#include <base/class_traits.h>
#include <tools/nycr/compound.h>
#include <tools/nycr/symbol/language.h>

namespace Tools {

  namespace Nycr {

    class TBase;

    class TLanguage
        : public TCompound {
      NO_COPY(TLanguage);
      public:

      TLanguage(const Syntax::TLanguage *decl);

      virtual Symbol::TCompound *GetSymbolAsCompound() const;

      Symbol::TLanguage *GetSymbolAsLanguage() const {
        assert(Symbol);
        return Symbol;
      }

      private:

      virtual bool Build(int pass);

      std::vector<Symbol::TName> Namespaces;

      std::optional<int> Sr, Rr;

      Symbol::TLanguage *Symbol;

    };  // TLanguage

    template <>
    struct TDecl::TInfo<TLanguage> {
      static const char *Name;
    };

  }  // Nycr

}  // Tools
