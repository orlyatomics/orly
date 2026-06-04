/* <orly/type/gen_code.h>

   `GenCode(stream, type)`: writes the C++ source representation of
   an orly `TType` (e.g. `Orly::Rt::TDict<int64_t, std::string>` for
   a `dict<int64_t, str>`). Called by orlyc when emitting code-gen
   output. The in-file `TODO: move to <orly/code_gen/type.h>` comment
   is honest -- this helper has more in common with the code-gen
   layer than the type layer.

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

  namespace Type {

    //TODO: move to <orly/code_gen/type.h>
    void GenCode(std::ostream &strm, const TType &type);

  }  // Type

}  // Orly