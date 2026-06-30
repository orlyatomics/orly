/* <tools/nycr/kind.h>

   A declaration of a kind of token.

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
#include <tools/nycr/decl.h>
#include <tools/nycr/symbol/base.h>

namespace Tools {

  namespace Nycr {

    class TBase;

    class TKind
        : public TDecl {
      NO_COPY(TKind);
      public:

      virtual Symbol::TKind *GetSymbolAsKind() const = 0;

      Symbol::TBase *TryGetBaseSymbol() const;

      protected:

      TKind(const Syntax::TName *name, const Syntax::TOptSuper *opt_super);

      virtual void ForEachPred(int pass, const std::function<void (TDecl *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      TRef<TBase> Super;

    };  // TKind

    template <>
    struct TDecl::TInfo<TKind> {
      static const char *Name;
    };

  }  // Nycr

}  // Tools
