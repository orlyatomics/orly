/* <orly/symbol/function.h>

   User-defined orlyscript function. Lives in a `TScope`, owns its
   `TParamDefSet`, holds the body expression via the inherited
   `TRoot`. Distinct from `TBuiltInFunction` (compiler-provided) but
   visible through the shared `TAnyFunction` base.

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

#include <memory>

#include <orly/pos_range.h>
#include <orly/symbol/any_function.h>
#include <orly/symbol/root.h>

namespace Orly {

  namespace Symbol {

    class TScope;

    class TFunction
        : public TRoot,
          public TAnyFunction,
          public std::enable_shared_from_this<TFunction> {
      NO_COPY(TFunction);
      public:

      typedef std::shared_ptr<TFunction> TPtr;

      class TParamDef
          : public TDef {
        NO_COPY(TParamDef);
        public:

        typedef std::shared_ptr<TParamDef> TPtr;

        virtual Type::TType GetType() const = 0;

        protected:

        TParamDef(const std::string &name, const TPosRange &pos_range);

      };  // TParamDef

      typedef std::shared_ptr<TScope> TScopePtr;

      typedef std::set<TParamDef::TPtr> TParamDefSet;

      static TPtr New(const TScopePtr &scope, const std::string &name, const TPosRange &pos_range);

      virtual ~TFunction();

      virtual void Accept(const TVisitor &visitor) const;

      void Add(const TParamDef::TPtr &param_def);

      const TParamDefSet &GetParamDefs() const;

      virtual Type::TObj::TElems GetParams() const;

      const TPosRange &GetPosRange() const;

      virtual Type::TType GetReturnType() const;

      TScopePtr GetScope() const;

      /* Verify recursive-call results in propagating positions (#128 Option B).
         GetReturnType() defers a recursive call to a TAny placeholder while the
         return type is being inferred, so a payload/argument/operator check on
         that result is skipped (Option A). This re-evaluates the body with the
         inferred return type substituted for the placeholder so those checks
         fire against a concrete type and catch genuine errors -- e.g. a
         recursive result fed to `+` against a `str`, or into a ctor arm that
         wants a scalar. A two-pass fixpoint (re-evaluation cleared between
         passes); self-recursion converges in one extra pass. Harmless for a
         non-recursive function -- the re-evaluation just reproduces the
         inferred type and raises nothing. Run once per function during
         TypeCheck, after the post-build widening passes have settled the
         body. */
      /* Virtual so an import function (#171), whose result type is declared and
         which has only a placeholder body, can skip the body re-evaluation. */
      virtual void VerifyRecursiveReturns() const;

      void Remove(const TParamDef::TPtr &param_def);

      TScopePtr TryGetScope() const;

      protected:

      TFunction(const TScopePtr &scope, const std::string &name, const TPosRange &pos_range);

      private:

      /* Drop the memoized type of every node in the body expr tree (but NOT of
         inner/where-bound function bodies -- they are their own roots and
         fixpoint independently). Used by VerifyRecursiveReturns between passes
         so the re-evaluation actually recomputes nodes that cached a concrete
         type from a TAny operand. */
      void ClearBodyCachedTypes() const;

      /* Re-entrancy guard for return-type inference: set while computing the
         body type so a recursive call returns the placeholder rather than
         recursing forever. */
      mutable bool IsRecursive;

      /* Set while VerifyRecursiveReturns re-evaluates the body: a recursive
         call then returns RecursiveEstimate (the current concrete return-type
         estimate) instead of the TAny placeholder, so the strict checks fire.
         (#128 Option B.) */
      mutable bool Verifying;

      /* The concrete return-type estimate handed back to a recursive call
         during verification. Only meaningful while Verifying is true. */
      mutable Type::TType RecursiveEstimate;

      TParamDefSet ParamDefs;

      std::weak_ptr<TScope> Scope;

      const TPosRange PosRange;

    };  // TFunction

  }  // Symbol

}  // Orly
