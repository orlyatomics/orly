/* <orly/synth/new_expr.h>

   Synth-layer node for `new <[key]> <- val` expressions (the
   creating-a-new-entry form, used outside effecting blocks).
   Lowers to `Expr::TNew`.

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

#include <orly/orly.package.cst.h>
#include <orly/synth/expr.h>
#include <orly/synth/func_def.h>
#include <orly/synth/scope_and_def.h>

namespace Orly {

  namespace Synth {

    class TParamFuncDef;
    class TLhsRhsableExpr;
    class TStartableExpr;
    class TThatableExpr;

    class TExprFactory {
      public:

      TExprFactory(
          TScope *outer_scope,
          TFuncDef *enclosing_func = nullptr,
          TParamFuncDef *param_func = nullptr,
          TLhsRhsableExpr *sort_expr = nullptr,
          TStartableExpr *startable_expr = nullptr,
          TThatableExpr *thatable_expr = nullptr);

      TExpr *NewExpr(const Package::Syntax::TExpr *root) const;

      TFuncDef *EnclosingFunc;

      TScope *OuterScope;

      TParamFuncDef *ParamFunc;

      TLhsRhsableExpr *LhsRhsableExpr;

      TStartableExpr *StartableExpr;

      TThatableExpr *ThatableExpr;

    };  // TExprFactory

  }  // Synth

}  // Orly
