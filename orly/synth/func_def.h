/* <orly/synth/func_def.h>

   Synth-layer node for a function definition (`f = (body) where
   { ... }`). Carries the parsed function symbol, the parameter
   list, the where-clause scope, and the body expression. Drives
   the multi-pass build: type-check the body, register the function
   in its scope, lower to `Symbol::TFunction`.

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
#include <unordered_set>

#include <orly/orly.package.cst.h>
#include <orly/pos_range.h>
#include <orly/symbol/function.h>
#include <orly/symbol/param_def.h>
#include <orly/symbol/result_def.h>
#include <orly/synth/expr.h>
#include <orly/synth/scope_and_def.h>
#include <orly/synth/type.h>

namespace Orly {

  namespace Synth {

    /* Forward declarations. */
    class TExprFactory;
    class TParamFuncDef;
    class TPureFuncDef;

    class TFuncDef
        : public TDef {
      NO_COPY(TFuncDef);
      public:

      typedef std::unordered_set<TName> TParamNameSet;

      virtual ~TFuncDef();

      void AddParamName(const TName &name);

      Symbol::TFunction::TPtr GetSymbol() const;

      protected:

      TFuncDef(TScope *scope, const Package::Syntax::TFuncDef *func_def);

      /* Build a function def that has no backing CST node -- a synthetic
         binding such as a `when`-arm payload binder (`Tag(n): ...`), whose
         name, position, and body are supplied directly rather than parsed.
         See <orly/synth/when_expr.cc>. */
      TFuncDef(TScope *scope, const TName &name, const TPosRange &pos_range);

      virtual void BuildSecondarySymbol();

      void SetExpr(TExpr *expr);

      /* Install the lowered function symbol directly. Used by a subclass whose
         symbol is not a plain TFunction built in the default pass-2 path -- e.g.
         an import def, which lowers to a TImportFunction with a declared result
         type and no body (#171). GetSymbol() then returns it for the ordinary
         function-reference path. */
      void SetSymbol(const Symbol::TFunction::TPtr &symbol);

      /* The backing CST node, or null for a synthetic def (see the
         CST-less constructor above). */
      const Package::Syntax::TFuncDef *FuncDef;

      /* The def's source position -- from the CST node when there is one,
         else supplied directly. */
      TPosRange PosRange;

      private:

      virtual TAction Build(int pass);

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      TExpr *Expr;

      TParamNameSet ParamNames;

      Symbol::TFunction::TPtr Symbol;

    };  // TFuncDef

    template <>
    struct TDef::TInfo<TFuncDef> {
      static const char *Name;
    };

    class TParamFuncDef
        : public TFuncDef {
      NO_COPY(TParamFuncDef);
      public:

      TParamFuncDef(
          const TExprFactory *expr_factory,
          const Package::Syntax::TFuncDef *func_def,
          const Package::Syntax::TGivenExpr *given_expr);

      const Symbol::TParamDef::TPtr &GetParamDefSymbol() const;

      TType *GetType() const;

      private:

      void BuildSecondarySymbol();

      TFuncDef *EnclosingFunc;

      const Package::Syntax::TGivenExpr *GivenExpr;

      Symbol::TParamDef::TPtr ParamDefSymbol;

      TType *Type;

    };  // TParamFuncDef

    template <>
    struct TDef::TInfo<TParamFuncDef> {
      static const char *Name;
    };

    class TPureFuncDef
        : public TFuncDef {
      NO_COPY(TPureFuncDef);
      public:

      TPureFuncDef(const TExprFactory *expr_factory, const Package::Syntax::TFuncDef *func_def);

    };  // TPureFuncDef

    template <>
    struct TDef::TInfo<TPureFuncDef> {
      static const char *Name;
    };

  }  // Synth

}  // Orly
