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
#include <string>

#include <base/json.h>
#include <orly/sabot/state.h>

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

    /* The JSON for a sabot state: always equal to ToJson(ToVar(state)), but ints, bools, reals,
       ids, strings, times, optionals, lists, sets, addresses and records (variants included) are
       built straight from the state, without the intermediate var tree.  Everything else takes the
       ToJson(ToVar()) path for its subtree. */
    Base::TJson StateToJson(const Sabot::State::TAny &state);

    /* Appends the compact JSON text of a sabot state to 'out': always the text
       StateToJson(state).AppendTo(out) would append, but written straight into 'out' with no
       TJson tree in between (#1053).  Every kind an Orly value can have, maps included, is written
       directly; the sabot kinds no Orly value has (the sized ints, chars, floats, blobs, free,
       tombstone and void) and a malformed variant take the StateToJson path for their subtree. */
    void AppendStateJson(std::string &out, const Sabot::State::TAny &state);

  }  // Var

}  // Orly