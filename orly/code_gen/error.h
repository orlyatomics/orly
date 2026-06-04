/* <orly/code_gen/error.h>

   Single error class for the code-gen layer: `TCgError`. Defined via
   the `DEFINE_ERROR` macro to inherit `std::runtime_error`. Thrown
   by `TCppPrinter` when it can't open an output file, and by helpers
   that hit unexpected state during emission.

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

#include <base/thrower.h>

namespace Orly {

  namespace CodeGen {

    DEFINE_ERROR(TCgError, std::runtime_error, "code gen error");

  } // CodeGen

} // Orly
