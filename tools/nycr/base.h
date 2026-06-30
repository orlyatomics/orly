/* <tools/nycr/base.h>

   A declaration of an abstract token.

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

#include <cassert>

#include <base/class_traits.h>
#include <tools/nycr/kind.h>

namespace Tools {

  namespace Nycr {

    class TBase;

    class TBase
        : public TKind {
      NO_COPY(TBase);
      public:

      TBase(const Syntax::TBase *decl);

      Symbol::TBase *GetSymbolAsBase() const {
        assert(Symbol);
        return Symbol;
      }

      virtual Symbol::TKind *GetSymbolAsKind() const;

      private:

      virtual bool Build(int pass);

      Symbol::TBase *Symbol;

    };  // TKind

    template <>
    struct TDecl::TInfo<TBase> {
      static const char *Name;
    };

  }  // Nycr

}  // Tools
