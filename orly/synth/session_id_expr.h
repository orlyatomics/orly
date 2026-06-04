/* <orly/synth/session_id_expr.h>

   Synth-layer node for the `session_id` keyword. Returns the
   calling session's UUID. Singleton-shape with no operands. Lowers
   to `Expr::TSessionId`.

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
#include <orly/orly.package.cst.h>
#include <orly/synth/expr.h>

namespace Orly {

  namespace Synth {

    /* TODO */
    class TSessionIdExpr
        : public TExpr {
      NO_COPY(TSessionIdExpr);
      public:

      /* TODO */
      TSessionIdExpr(const Package::Syntax::TSessionIdExpr *session_id_expr);

      /* TODO */
      virtual Expr::TExpr::TPtr Build() const;

      private:

      /* TODO */
      const Package::Syntax::TSessionIdExpr *SessionIdExpr;

    };  // TSessionIdExpr

  }  // Synth

}  // Orly
