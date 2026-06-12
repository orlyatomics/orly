/* <orly/synth/type_def.h>

   Synth-layer node for a type definition (`type Name = ...;`).
   Holds the parsed type expression and registers itself in the
   enclosing scope as a `TDef<TTypeDef>` that `TRefType` looks up
   by name.

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

#include <orly/orly.package.cst.h>
#include <orly/type.h>
#include <orly/synth/scope_and_def.h>
#include <orly/synth/type.h>

namespace Orly {

  namespace Synth {

    /* TODO */
    class TTypeDef
        : public TDef {
      public:

      /* TODO */
      TTypeDef(TScope *scope, const Package::Syntax::TTypeDef *type_def);

      /* TODO */
      virtual ~TTypeDef();

      /* Computes (and caches) the def's symbolic type. While the
         computation runs the def is "in flight": a TRefType naming it
         resolves to a Type::TSelfRef instead of recursing, which is what
         lets a variant type def refer to itself (issue #103). */
      const Type::TType &GetSymbolicType() const;

      /* The innermost def currently computing its symbolic type, or null.
         Only the innermost may be self-referenced (direct recursion). */
      static const TTypeDef *GetInnermostInFlight();

      /* True iff the def is anywhere on the in-flight stack. A reference
         to an in-flight def that is NOT the innermost one is transitive /
         mutual recursion, which v1 does not support. */
      static bool IsInFlight(const TTypeDef *def);

      /* The number of variant binders entered since the innermost
         in-flight def's computation began. Zero when no def is in flight
         (or no variant has been entered): a self-reference is then
         outside any variant arm and cannot be bound. */
      static size_t GetCurVariantDepth();

      /* RAII used by TVariantType::ComputeSymbolicType to track variant
         nesting inside the innermost in-flight def's type expression.
         A no-op when no def is in flight (variant literals in expression
         contexts). */
      class TVariantDepthIncr {
        NO_COPY(TVariantDepthIncr);
        public:
        TVariantDepthIncr();
        ~TVariantDepthIncr();
      };  // TVariantDepthIncr

      /* Called by TRefType when it mints a TSelfRef for the innermost
         in-flight def. The def remembers it is recursive, which is what
         triggers the placement validation in Build() -- a def that merely
         *uses* some other recursive type never minted anything and is
         exempt. */
      static void NoteSelfRefMinted();

      private:

      /* Sticky: set when computing this def's symbolic type minted at
         least one self-reference. */
      mutable bool SelfRefMinted = false;

      /* TODO */
      virtual TAction Build(int pass);

      /* TODO */
      virtual void ForEachPred(int pass, const std::function<bool (TDef *)> &cb);

      /* TODO */
      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      /* TODO */
      TType *Type;

    };  // TTypeDef

    /* TODO */
    template <>
    struct TDef::TInfo<TTypeDef> {
      static const char *Name;
    };

  }  // Synth

}  // Orly
