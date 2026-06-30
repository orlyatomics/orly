/* <orly/synth/test_def.h>

   Synth-layer node for a test definition
   (`test { t1: ...; t2: ...; }`). Carries each named test
   expression and lowers to a `Symbol::Test::TTest` -- which
   `TPackage` collects and emits as `TTestCase` in the code-gen
   layer.

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

#include <cassert>
#include <vector>

#include <base/class_traits.h>
#include <orly/orly.package.cst.h>
#include <orly/synth/scope_and_def.h>

namespace Orly {

  namespace Synth {

    /* Forward declarations */
    class TWithClause;
    class TTestCaseBlock;

    class TTestDef
        : public TDef {
      NO_COPY(TTestDef);
      public:

      TTestDef(TScope *scope, const Package::Syntax::TTestDef *test_def);

      virtual ~TTestDef();

      virtual TAction Build(int pass);

      virtual void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      virtual void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      static unsigned int TestId;

      static TName GenName(const TPosRange &pos_range);

      const Package::Syntax::TTestDef *TestDef;

      TWithClause *OptWithClause;

      TTestCaseBlock *TestCaseBlock;

    };  // TTestDef

  }  // Synth

}  // Orly
