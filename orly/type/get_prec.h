/* <orly/type/get_prec.h>

   `GetPrec(type)` returns a type's precedence for the absolute
   ordering used when sorting keys in the database. The `TPrec` enum
   (`Addr` < `Bool` < `Dict` < `Id` < `Int` < `List` < `Obj` <
   `Real` < `Set` < `Str` < `TimeDiff` < `TimePnt`) is the canonical
   ordering -- used wherever the storage layer needs to compare keys
   of different types deterministically.

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

namespace Orly {

  namespace Type {

    /* This is the absolute ordering of the types for ordering keys in the database */
    enum TPrec {
      Addr,
      Bool,
      Dict,
      Id,
      Int,
      List,
      Obj,
      Real,
      Set,
      Str,
      TimeDiff,
      TimePnt,
      Variant
    };  // TPrec

    TPrec GetPrec(const TType &type);

  }  // Type

}  // Orly
