/* <orly/synth/test_kv_entry.h>

   Synth-layer node for one entry in a `with { ... }` clause of a
   test: the key expression on the lhs and the value expression on
   the rhs. Lowers to a single setup write in the test's emitted
   setup block.

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
#include <orly/symbol/stmt/new_and_delete.h>
#include <orly/synth/new_expr.h>
#include <orly/synth/scope_and_def.h>

namespace Orly {

  namespace Synth {

    class TTestKvEntry {
      NO_COPY(TTestKvEntry);
      public:

      TTestKvEntry(
          const TExprFactory *expr_factory,
          const Package::Syntax::TTestKvEntry *test_kv_entry);

      ~TTestKvEntry();

      Symbol::Stmt::TNew::TPtr Build() const;

      void ForEachInnerScope(const std::function<void (TScope *)> &cb);

      void ForEachRef(const std::function<void (TAnyRef &)> &cb);

      private:

      const Package::Syntax::TTestKvEntry *TestKvEntry;

      TExpr *Key;

      TExpr *Val;

    };  // TTestKvEntry

  }  // Synth

}  // Orly
