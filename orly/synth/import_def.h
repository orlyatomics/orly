/* <orly/synth/import_def.h>

   Synth-layer node for a cross-package import definition
   (`answer is int get_answer from package <imports/lib>#1;`, issue #171).
   It binds a local value name of a *declared* type to a symbol in another
   package. Modeled as a TFuncDef so the ordinary value-reference path
   (`TryGetDef<TFuncDef>` -> GetSymbol() -> result def) resolves it, but it
   lowers to a Symbol::TImportFunction with the declared result type and no
   body rather than the default pass-2 function.

   Stage 1 (#171) implements binding + type-checking against the declared type;
   resolving/building/linking the source package is a later stage.

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

#include <functional>
#include <string>

#include <orly/orly.package.cst.h>
#include <orly/package/name.h>
#include <orly/synth/func_def.h>
#include <orly/synth/scope_and_def.h>
#include <orly/synth/type.h>

namespace Orly {

  namespace Synth {

    class TImportDef
        : public TFuncDef {
      NO_COPY(TImportDef);
      public:

      TImportDef(TScope *scope, const Package::Syntax::TImportDef *import_def);

      virtual ~TImportDef();

      private:

      /* Lower to a Symbol::TImportFunction with the declared result type. */
      virtual TAction Build(int pass);

      /* No body, hence no inner scopes and no references to bind. */
      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);
      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      /* The declared type the local name is bound at. */
      TType *DeclaredType;

      /* The source package (from a literal package_ref `package <a/b/c>#N`). */
      Package::TName PackageName;

      /* The symbol's name in the source package (opt_name, or the local name). */
      std::string RemoteName;

    };  // TImportDef

    template <>
    struct TDef::TInfo<TImportDef> {
      static const char *Name;
    };

  }  // Synth

}  // Orly
