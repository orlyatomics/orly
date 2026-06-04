/* <orly/expr/where.h>

   `TWhere` -- the `expr where { defs }` IR node. Inherits
   `TUnary` over the body expression; the definitions live in an
   attached scope. Lowered to `CodeGen::TInlineScope` with the
   body as its returned expression.

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
#include <orly/expr/unary.h>
#include <orly/symbol/scope.h>

namespace Orly {

  namespace Expr {

    class TWhere
        : public Symbol::TScope, public TUnary {
      NO_COPY(TWhere);
      public:

      typedef std::shared_ptr<TWhere> TPtr;

      static TPtr New(const TPosRange &pos_range);

      virtual void Accept(const TVisitor &visitor) const;

      virtual Type::TType GetTypeImpl() const override;

      private:

      TWhere(const TPosRange &pos_range);

    };  // TWhere

  }  // Expr

}  // Orly
