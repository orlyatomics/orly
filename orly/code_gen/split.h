/* <orly/code_gen/split.h>

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

    class TSplit
        : public TInline {
      NO_COPY(TSplit);
      public:

      typedef std::shared_ptr<TSplit> TPtr;

      static TPtr New(
          const L0::TPackage *package,
          const TInline::TPtr &split_text,
          const TInline::TPtr &regex);

      void WriteExpr(TCppPrinter &out) const;

      /* Dependency graph.  #298 audit: Regex was inserted without recursing into its own
         dependencies -- the only two implementations that broke the insert-then-recurse
         convention; AppendDependency gives it the standard treatment. */
      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const override {
        AppendDependency(SplitThisText, dependency_set);
        AppendDependency(Regex, dependency_set);
      }

      private:

      TSplit(const L0::TPackage *package,
             const TInline::TPtr &split_text,
             const TInline::TPtr &regex);

      static Type::TType GetReturnType();

      TInline::TPtr SplitThisText;

      TInline::TPtr Regex;

    };  // TSplit

  }  // CodeGen

}  // Orly