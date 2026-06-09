/* <base/opt_ostream.h>

   Text stream inserters/extractors for std::optional<>.

   These preserve the behavior of the former Base::TOpt<> std stream operators:
   inserting an unknown optional writes nothing, and extracting reads a value
   unless the stream is already at end-of-file (in which case the optional is
   reset to the unknown state).

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

#include <istream>
#include <optional>
#include <ostream>

namespace Base {

  /* A std stream inserter for std::optional<>.  If the optional is unknown, then this function inserts nothing. */
  template <typename TVal>
  std::ostream &operator<<(std::ostream &strm, const std::optional<TVal> &that) {
    if (that) {
      strm << *that;
    }
    return strm;
  }

  /* A std stream extractor for std::optional<>. */
  template <typename TVal>
  std::istream &operator>>(std::istream &strm, std::optional<TVal> &that) {
    if (!strm.eof()) {
      if (!that) {
        that.emplace();
      }
      strm >> *that;
    } else {
      that.reset();
    }
    return strm;
  }

}  // Base
