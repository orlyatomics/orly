/* <orly/var/jsonify.h>

   Write a TVar back into json script

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

#include <ostream>

#include <base/json.h>

namespace Orly {

  namespace Var {

    class TVar;

    void Jsonify(std::ostream &stream, const TVar &var);

    /* Build the JSON tree for the var directly -- no render-to-text /
       re-parse round trip (#377).  Value mapping is identical to Jsonify
       followed by Base::TJson::Parse, with one deliberate improvement:
       reals keep full double precision instead of being clipped to the
       ostream's six significant digits by the text hop. */
    Base::TJson ToJson(const TVar &var);

  }  // Var

}  // Orly