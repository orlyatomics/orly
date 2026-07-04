/* <orly/compiler.h>

   The controller for the orly compiler. Runs all the phases, exits on first phase failure.

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

#include <iostream>
#include <string>

#include <base/path.h>
#include <base/thrower.h>
#include <jhm/naming.h>
#include <orly/package/name.h>

namespace Orly {

  namespace Compiler {

    /* Thrown when we compilation fails. Errors will have been printed to the out-stream. */
    DEFINE_ERROR(TCompileFailure, std::runtime_error, "compile failure");

    /* How to compile (#312). The modes nest: SyntaxOnly stops after parse +
       synthesis (no type-check), SemanticOnly stops after type-check (no
       emission, no gcc); with neither set the compile runs to a linked .so.
       TransientCc removes the generated C++ intermediates after a successful
       link (and disables the staleness cache -- there is nothing left to
       reuse). DebugCc compiles the generated C++ with -g and also disables
       the staleness cache, so a debug run always regenerates. */
    struct TOptions {
      bool DebugCc = false;
      bool MachineMode = false;
      bool SemanticOnly = false;
      bool SyntaxOnly = false;
      bool TransientCc = false;
    };  // TOptions

    Package::TVersionedName Compile(
        Base::TPath core_file,
        const Jhm::TTree &out_tree,
        const TOptions &options,
        std::ostream &out_strm = std::cout);

  }  // Compiler

}  // Orly
