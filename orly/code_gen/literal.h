/* <orly/code_gen/literal.h>

   `TLiteral` wraps a runtime `Var::TVar` and emits its C++ literal
   form (via the `<< Var` stream overload). Used for compile-time-
   known values appearing in expressions. The in-file comment about
   not common-subexpression-eliminating literals is a planned tweak --
   literals are cheap enough to inline freely.

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

#include <orly/code_gen/inline.h>
#include <orly/var/impl.h>

namespace Orly {

  namespace CodeGen {

    class TLiteral : public TInline {
      NO_COPY(TLiteral);

      public:

      TLiteral(const L0::TPackage *package, const Var::TVar &var) : TInline(package, var.GetType()), Var(var) {}

      void WriteExpr(TCppPrinter &out) const final {
        out << Var;
      }

      /* Dependency graph */
      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &/*dependency_set*/) const override {
      }

      /* A literal's emission is already a constant; a CSE local for it is noise (#297). */
      virtual bool IsCseWorthy() const override {
        return false;
      }

      private:
      Var::TVar Var;

    }; // TLiteral


  } // CodeGen

} // Orly