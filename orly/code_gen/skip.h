/* <orly/code_gen/skip.h>

   `TSkip` emits the `skip` sequence operator: drop the first
   `count` elements of `seq`. Counterpart of `TTake`.

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

#include <orly/code_gen/function.h>
#include <orly/code_gen/inline.h>

namespace Orly {

  namespace CodeGen {

    class TSkip
        : public TInline {
      NO_COPY(TSkip);
      public:

      typedef std::shared_ptr<TSkip> TPtr;

      static TPtr New(
          const L0::TPackage *package,
          const Type::TType &ret_type,
          const TInline::TPtr &seq,
          const TInline::TPtr &count);

      void WriteExpr(TCppPrinter &out) const;

      /* Dependency graph */
      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const override {
        AppendDependency(Count, dependency_set);
        AppendDependency(Seq, dependency_set);
      }

      private:

      TSkip(const L0::TPackage *package,
            const Type::TType &ret_type,
            const TInline::TPtr &seq,
            const TInline::TPtr &count);

      TInline::TPtr Count;

      TInline::TPtr Seq;

    };  // TSkip

  }  // CodeGen

}  // Orly