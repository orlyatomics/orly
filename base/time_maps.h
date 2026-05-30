/* <base/time_maps.h>

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

#include <orly/type/obj.h>

namespace Base {

  namespace Chrono {

    /* These maps are used for checking the structural type of objects later in the compiler.
       For use with implementing time_obj, which converts time_pnts AND time_diffs into objects. */


    const Orly::Type::TObj::TElems &GetTimeDiffMap();
    const Orly::Type::TObj::TElems &GetTimePntMap();

    /* Helper function to check if an object is a time object */
    bool IsTimeObj(const Orly::Type::TObj *type);

  }  // Chrono

}  // Base