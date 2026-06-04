/* <orly/expr/startable.h>

   `TStartable` -- mixin base for `TExpr`s that introduce a
   `start` binding (`TReduce`, `TCollatedBy`). Holds the
   `TStart::TPtr` set during synth-build so subsequent code-gen
   can find it.

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

#include <memory>

#include <base/class_traits.h>
#include <orly/expr/start.h>
#include <orly/type/impl.h>

namespace Orly {

  namespace Expr {

    class TStartable {
      NO_COPY(TStartable);
      public:

      using TPtr = std::shared_ptr<TStartable>;

      virtual ~TStartable() {}

      const TStart::TPtr &GetStart() const {
        assert(Start);
        return Start;
      }

      const TStart::TPtr &TryGetStart() const {
        return Start;
      }

      void SetStart(const TStart::TPtr &start) {
        assert(start);
        assert(!Start);
        Start = start;
      }

      protected:

      TStartable()
          : Start(nullptr) {}

      TStart::TPtr Start;

    };  // TStartable

  }  // Expr

}  // Orly
