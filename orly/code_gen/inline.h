/* <orly/code_gen/inline.h>

   `TInline` is the abstract base for every code-gen expression node.
   `WriteExpr` emits the C++ source for this expression;
   `AppendDependsOn` declares which other inlines this one references
   (for the common-subexpression eliminator and dependency-order
   emission). `SetCommonSubexpressionId` is set by CSE when the same
   `TInline` appears more than once -- subsequent `Write` calls then
   emit the saved variable name instead of re-emitting the full
   expression.

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
#include <optional>
#include <unordered_set>
#include <orly/code_gen/id.h>
#include <orly/code_gen/package_base.h>
#include <orly/type/impl.h>

namespace Orly {

  namespace CodeGen {

    class TInline {
      NO_COPY(TInline);
      public:

      typedef std::shared_ptr<const TInline> TPtr;

      virtual ~TInline() = default;

      const TId<TIdKind::Var> &GetId() const;

      Type::TType GetReturnType() const;

      bool HasId() const;

      //Used by common sub expression eliminator.
      void SetCommonSubexpressionId(TId<TIdKind::Var> &&id) const;

      /* Writes the common subexpression eliminated variant of this inline. */
      void Write(TCppPrinter &out) const;

      /* Pure virtual inlines should provide which emits the inline's C++ expression. */
      virtual void WriteExpr(TCppPrinter &out) const = 0;

      /* Dependency graph */
      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const = 0;

      /* The one way implementations of AppendDependsOn should record a child: insert it and,
         only if it was newly inserted, recurse into it.  The insert-check keeps the walk linear
         in edges over shared DAGs (and sharing is exactly what CSE creates), and it breaks any
         cycle through a recursive call's body globally (#297/#298). */
      static void AppendDependency(const TPtr &child, std::unordered_set<TPtr> &dependency_set) {
        assert(child);
        if (dependency_set.insert(child).second) {
          child->AppendDependsOn(dependency_set);
        }
      }

      /* False for leaves whose emission is already a name or constant (literals, typed leaves,
         context vars, arg refs): materializing those as CSE locals is pure noise (#297). */
      virtual bool IsCseWorthy() const {
        return true;
      }

      /* We need to know if this expression is free */
      virtual bool IsFree() const {
        return false;
      }

      protected:
      TInline(const L0::TPackage *package, Type::TType type);

      const L0::TPackage *Package;

      private:
      Type::TType ReturnType; //The return type.
      //This is set by SetCommonSubExpressionId for common subexpression elimination.
      mutable std::optional<TId<TIdKind::Var>> Id;

    }; // TInline

    Orly::CodeGen::TCppPrinter &operator<<(Orly::CodeGen::TCppPrinter &out,
        const Orly::CodeGen::TInline::TPtr &ptr);

  } // CodeGen

} // Orly