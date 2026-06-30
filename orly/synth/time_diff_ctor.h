/* <orly/synth/time_diff_ctor.h>

   Synth-layer node for time-diff literals (the
   `{day, hour, minute, ...}` record returned by `TimeDiff`-shape
   constructors). Carries each component expression and lowers to
   a typed `TBasicCtor` over the `TimeDiff` object record.

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
#include <orly/synth/type.h>
#include <orly/synth/scope_and_def.h>

namespace Orly {

  namespace Synth {

    class TTimeDiffCtor
        : public TExpr {
      NO_COPY(TTimeDiffCtor);
      public:

      TTimeDiffCtor(const Package::Syntax::TTimeDiffCtor *time_diff_ctor);

      virtual Expr::TExpr::TPtr Build() const;

      private:

      const Package::Syntax::TTimeDiffCtor *TimeDiffCtor;

    };  // TTimeDiffCtor

  }  // Synth

}  // Orly
