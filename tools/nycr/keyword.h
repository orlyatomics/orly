/* <tools/nycr/keyword.h>

   A declaration of a keyword of token.

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

#include <base/class_traits.h>
#include <tools/nycr/atom.h>
#include <tools/nycr/symbol/keyword.h>

namespace Tools {

  namespace Nycr {

    class TKeyword
        : public TAtom {
      NO_COPY(TKeyword);
      public:

      TKeyword(const Syntax::TKeyword *decl);

      virtual Symbol::TAtom *GetSymbolAsAtom() const;

      Symbol::TKeyword *GetSymbolAsKeyword() const {
        assert(Symbol);
        return Symbol;
      }

      private:

      virtual bool Build(int pass);

      Symbol::TKeyword *Symbol;

    };  // TKeyword

    template <>
    struct TDecl::TInfo<TKeyword> {
      static const char *Name;
    };

  }  // Nycr

}  // Tools
