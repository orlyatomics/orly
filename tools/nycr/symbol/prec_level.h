/* <tools/nycr/symbol/prec_level.h>

   A precedence level, used for describing the precedence of operators.

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

#include <cassert>
#include <cstddef>
#include <vector>

#include <base/class_traits.h>
#include <tools/nycr/symbol/name.h>

namespace Tools {

  namespace Nycr {

    namespace Symbol {

      class TPrecLevel {
        NO_COPY(TPrecLevel);
        public:

        typedef std::vector<TPrecLevel *> TPrecLevels;

        TPrecLevel(const TName &name, size_t idx);

        virtual ~TPrecLevel();

        size_t GetIdx() const {
          return Idx;
        }

        const TName &GetName() const {
          return Name;
        }

        static const TPrecLevels &GetPrecLevels() {
          return PrecLevels;
        }

        private:

        TName Name;

        size_t Idx;

        static TPrecLevels PrecLevels;

      };  // TPrecLevel

    }  // Symbol

  }  // Nycr

}  // Tools
