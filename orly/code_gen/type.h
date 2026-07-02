/* <orly/code_gen/type.h>

   `GenCode(stream, type)`: writes the C++ source representation of
   an orly `TType` (e.g. `Orly::Rt::TDict<int64_t, std::string>` for
   a `dict<int64_t, str>`). Called by orlyc when emitting code-gen
   output. Lived in orly/type/ as gen_code.h until #381; it emits
   code, so it belongs to the code-gen layer.

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

#include <orly/type.h>

#include <ostream>

namespace Orly {

  namespace CodeGen {

    void GenCode(std::ostream &strm, const Type::TType &type);

  }  // CodeGen

}  // Orly