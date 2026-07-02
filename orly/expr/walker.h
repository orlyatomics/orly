/* <orly/expr/walker.h>

   `ForEachExpr(root, cb, include_inner_funcs)` -- recursive tree
   walker. Calls `cb(expr)` for every expr in the tree; if `cb`
   returns true, doesn't recurse into that subtree.

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

#include <orly/expr/expr.h>

namespace Orly {

  namespace Expr {

    /* Walks the expr tree starting at the root calling the callback with every expr in the tree. If the callback
       returns true, it does not terminate the tree walker, but rather causes it not to recurse further. */
    void ForEachExpr(
        const TExpr::TPtr &root,
        const std::function<bool (const TExpr::TPtr &expr)> &cb,
        bool include_inner_funcs = false);

  } // Expr

} // Orly