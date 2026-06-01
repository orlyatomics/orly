/* <orly/rt/str_replace.h>

   Replace a regex in a string with something else.

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
#include <utility>
#include <string>

#include <base/class_traits.h>
#include <base/utf8/piece.h>
#include <base/utf8/regex.h>

namespace Orly {

  namespace Rt {

    /* Split a text into pieces based on regex delimiters.. */
    class TStrReplace final {
      NO_COPY(TStrReplace);
      public:

      using TPiece = Utf8::TPiece;
      using TRegex = Utf8::TRegex;

      static std::string Replace(std::string oldstr, const std::string &regex, const std::string &newstr) {
        TRegex Regex(regex.c_str());
        TPiece match_delim;
        /* Track position by index, not pointer. std::string::replace() may
           shift the buffer contents (and on some implementations reallocate),
           which invalidates any pointer kept across the call. The previous
           implementation cached match_delim.GetLimit() and oldstr.c_str() as
           raw pointers, then dereferenced them after a mutation -- silently
           UB. Worked for like-for-like replacements (t1) and for monotonically
           shrinking replacements that didn't reuse stale pointer values
           (t3), but corrupted output for any growing or repeated replacement
           (t2). */
        size_t pos = 0;
        while (pos <= oldstr.size()
               && Regex.TryGetMatch(oldstr.c_str() + pos, match_delim, 0)) {
          const size_t match_offset =
              (match_delim.GetStart() - oldstr.c_str());
          const size_t match_len =
              (match_delim.GetLimit() - match_delim.GetStart());
          oldstr.replace(match_offset, match_len, newstr);
          /* Step past the newly inserted text. For zero-length matches we
             must advance at least one position or we loop forever. */
          pos = match_offset + (match_len == 0 ? 1 : newstr.size());
        }
        return oldstr;
      }

    };  // TStrReplace

  }  // Rt

}  // Orly
