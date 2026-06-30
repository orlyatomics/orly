/* <orly/synth/time_pnt_ctor.h>

   Synth-layer node for time-point literals (the
   `{year, month, day, ...}` record returned by `TimePnt`-shape
   constructors). Same pattern as `time_diff_ctor.h` but for the
   absolute-time record.

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

    class TTimePntCtor
        : public TExpr {
      NO_COPY(TTimePntCtor);
      public:

      TTimePntCtor(const Package::Syntax::TTimePntCtor *time_pnt_ctor);

      virtual Expr::TExpr::TPtr Build() const;

      private:

      const Package::Syntax::TTimePntCtor *TimePntCtor;

    };  // TTimePntCtor

  }  // Synth

}  // Orly
