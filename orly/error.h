/* <orly/error.h>

   Error classes for the orly compiler.

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

#include <base/as_str.h>
#include <base/code_location.h>
#include <base/thrower.h>
#include <orly/pos_range.h>

namespace Orly {

  /* what() is the message for the person who wrote the orlyscript. The
     compiler source line that threw it is kept apart, in GetCodeLocation(),
     because to that person it reads like a crash rather than a diagnosis of
     their code (#557). Front ends show it only on request. */
  class TSourceError : public std::runtime_error {
    public:

    const Base::TCodeLocation &GetCodeLocation() const {
      return CodeLocation;
    }

    const TPosRange &GetPosRange() const {
      return PosRange;
    }

    protected:

    TSourceError(
        const Base::TCodeLocation &code_location,
        const TPosRange &pos_range,
        const char *msg)
          : std::runtime_error(msg), CodeLocation(code_location), PosRange(pos_range) {}

    private:

    const Base::TCodeLocation CodeLocation;

    const TPosRange PosRange;

  };  // TSourceError

  class TNotImplementedError : public TSourceError {
    public:

    TNotImplementedError(
        const Base::TCodeLocation &code_location,
        const TPosRange &pos_range,
        const char *message = "This feature is not yet implemented")
          : TSourceError(code_location, pos_range, message) {}

  };  // TNotImplementedError

  /* A compiler bug, not a user error, so the message keeps the compiler
     source location: "here is where" is what the report needs. */
  class TImpossibleError : public TSourceError {
    public:

    TImpossibleError(
        const Base::TCodeLocation &code_location,
        const TPosRange &pos_range,
        const char *message = "Internal Compiler Error: We shouldn't have reached this line of code in the compiler.")
          : TSourceError(code_location, pos_range, Base::AsStr(code_location, ' ', message).c_str()) {}

  };  // TImpossibleError

  class TCompileError : public TSourceError {
    public:

    TCompileError(
        const Base::TCodeLocation &code_location,
        const TPosRange &pos_range,
        const char *message)
          : TSourceError(code_location, pos_range, message) {}

  };  // TCompileError

  class TExprError : public TSourceError {
    public:

    /* The message used when a thrower has nothing more specific to say.
       Named so callers can recognize (and upgrade) the generic diagnostic;
       see Symbol::Stmt::TMutate::TypeCheck (issue #314). */
    static constexpr const char *DefaultMessage = "This expression is invalid.";

    TExprError(
        const Base::TCodeLocation &code_location,
        const TPosRange &pos_range,
        const char *message = DefaultMessage)
          : TSourceError(code_location, pos_range, message) {}

  };  // TExprError

}  // Orly
