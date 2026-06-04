/* <orly/type/real.h>

   The real (double-precision floating-point) type. Singleton via
   `TSingletonType<TReal>`. Result type of any mixed int/real
   arithmetic (see `add_visitor.h` / `mult_visitor.h` /
   `div_visitor.h`).

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

#include <orly/type/managed_type.h>

namespace Orly {

  namespace Type {

    /* TODO */
    class TReal : public TSingletonType<TReal> {
      NO_COPY(TReal);
      public:

      virtual ~TReal();

      private:
      TReal() {}

      virtual void Write(std::ostream &stream) const;

      friend class TSingletonType<TReal>;
    };  // TReal

  }  // Type

}  // Orly