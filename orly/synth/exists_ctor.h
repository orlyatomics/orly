/* <orly/synth/exists_ctor.h>

   Synth-layer node for `<[key]>::(T) is known` / `... is unknown`
   predicates. Holds the address key expression and the dereferenced
   value type. Lowers to `Expr::TExists`, which code-gen turns into
   a `TExists` runtime check.

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

#include <functional>

#include <base/class_traits.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/expr.h>
#include <orly/synth/name.h>
#include <orly/synth/new_expr.h>
#include <orly/synth/type.h>
#include <orly/synth/scope_and_def.h>

namespace Orly {

  namespace Synth {

    class TExistsCtor
        : public TExpr {
      NO_COPY(TExistsCtor);
      public:

      TExistsCtor(const TExprFactory *expr_factory, const Package::Syntax::TPrefixExists *exists_ctor);

      virtual ~TExistsCtor();

      virtual Expr::TExpr::TPtr Build() const;

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      const Package::Syntax::TPrefixExists *ExistsCtor;

      TExpr *Expr;

      TType *ValueType;

    };  // TExistsCtor

  }  // Synth

}  // Orly