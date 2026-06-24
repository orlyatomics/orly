/* <orly/code_gen/union_map.h>

   `TUnionMap` emits a `seq union_map elem` expression: map each
   element of `Seq` to a set via `Func` (a `TImplicitFunc` over the
   `that` element) and union the results, from an empty-set start.

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

namespace Orly {

  namespace CodeGen {

    class TImplicitFunc;

    class TUnionMap : public TInline {
      NO_COPY(TUnionMap);
      public:

      typedef std::shared_ptr<TUnionMap> TPtr;
      typedef std::shared_ptr<TImplicitFunc> TFuncPtr;

      static TPtr New(const L0::TPackage *package,
                      const Type::TType &ret_type,
                      const TInline::TPtr &seq,
                      const TFuncPtr &map_func);

      void WriteExpr(TCppPrinter &out) const;

      /* Dependency graph */
      virtual void AppendDependsOn(std::unordered_set<TInline::TPtr> &dependency_set) const override;

      private:
      TUnionMap(const L0::TPackage *package,
                const Type::TType &ret_type,
                const TInline::TPtr &seq,
                const TFuncPtr &map_func);

      TFuncPtr Func;
      TInline::TPtr Seq;
    }; // TUnionMap

  } // CodeGen

} // Orly
